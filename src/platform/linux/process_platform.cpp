#include "arena/ports/native_process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace arena::ports {
namespace {

class ScopedSigpipeBlock final {
  public:
    ScopedSigpipeBlock() {
        sigemptyset(&blocked_);
        sigaddset(&blocked_, SIGPIPE);
        const int mask_result = pthread_sigmask(SIG_BLOCK, &blocked_, &previous_);
        if (mask_result != 0) {
            throw std::runtime_error(
                std::string("failed to block SIGPIPE for process write: ") +
                std::strerror(mask_result));
        }
        active_ = true;

        sigset_t pending{};
        if (sigpending(&pending) != 0) {
            const int pending_error = errno;
            pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
            active_ = false;
            throw std::runtime_error(
                std::string("failed to inspect pending SIGPIPE state: ") +
                std::strerror(pending_error));
        }
        sigpipe_was_pending_ = sigismember(&pending, SIGPIPE) == 1;
    }

    ~ScopedSigpipeBlock() {
        if (active_) {
            pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
        }
    }

    ScopedSigpipeBlock(const ScopedSigpipeBlock&) = delete;
    ScopedSigpipeBlock& operator=(const ScopedSigpipeBlock&) = delete;

    void consume_generated_sigpipe() const noexcept {
        if (sigpipe_was_pending_) {
            return;
        }
        timespec timeout{};
        for (;;) {
            const int result = sigtimedwait(&blocked_, nullptr, &timeout);
            if (result == SIGPIPE) {
                return;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
    }

  private:
    sigset_t blocked_{};
    sigset_t previous_{};
    bool active_{};
    bool sigpipe_was_pending_{};
};

void close_fd(int& descriptor) noexcept {
    if (descriptor >= 0) {
        close(descriptor);
        descriptor = -1;
    }
}

void remember_failure(std::string& failure, std::string message) {
    if (failure.empty()) {
        failure = std::move(message);
    }
}

void terminate_startup_child(const pid_t child) noexcept {
    if (kill(-child, SIGKILL) != 0) {
        kill(child, SIGKILL);
    }
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
}

bool wait_for_process_group_exit(
    const pid_t process_group,
    const std::chrono::milliseconds timeout) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (kill(-process_group, 0) != 0) {
            return errno == ESRCH;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        poll(nullptr, 0, 10);
    }
}

} // namespace

struct NativeProcess::Impl {
    static constexpr std::size_t max_lines = 8192;

    pid_t process_id{-1};
    int stdin_fd{-1};
    int stdout_fd{-1};
    std::thread reader;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::string> lines;
    std::uint64_t dropped{};
    bool reader_finished{true};
    std::atomic_bool reader_stop_requested{false};

    void close_descriptors() noexcept {
        close_fd(stdin_fd);
        close_fd(stdout_fd);
    }

    void enqueue(std::string line) {
        std::scoped_lock lock(mutex);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (lines.size() >= max_lines) {
            if (line.starts_with("info ")) {
                ++dropped;
                return;
            }
            const auto observational = std::find_if(
                lines.begin(), lines.end(), [](const std::string& queued) {
                    return queued.starts_with("info ");
                });
            if (observational != lines.end()) {
                lines.erase(observational);
            } else {
                lines.pop_front();
            }
            ++dropped;
        }
        lines.push_back(std::move(line));
        condition.notify_one();
    }

    void read_output() {
        std::string pending;
        char buffer[4096];
        bool finished = false;
        while (!finished && !reader_stop_requested.load(std::memory_order_acquire)) {
            pollfd descriptor{stdout_fd, POLLIN | POLLHUP | POLLERR, 0};
            const int result = poll(&descriptor, 1, 100);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (result == 0) {
                continue;
            }
            if ((descriptor.revents & POLLIN) != 0) {
                for (;;) {
                    const ssize_t count = read(stdout_fd, buffer, sizeof(buffer));
                    if (count > 0) {
                        pending.append(buffer, static_cast<std::size_t>(count));
                        std::size_t newline = 0;
                        while ((newline = pending.find('\n')) != std::string::npos) {
                            enqueue(pending.substr(0, newline));
                            pending.erase(0, newline + 1);
                        }
                        continue;
                    }
                    if (count == 0) {
                        finished = true;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        finished = true;
                    }
                    break;
                }
            }
            if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                finished = true;
            }
        }
        if (!pending.empty()) {
            enqueue(std::move(pending));
        }
        {
            std::scoped_lock lock(mutex);
            reader_finished = true;
        }
        condition.notify_all();
    }
};

NativeProcess::NativeProcess() : impl_(std::make_unique<Impl>()) {}

NativeProcess::~NativeProcess() {
    try {
        stop(std::chrono::milliseconds(250));
    } catch (...) {
    }
}

void NativeProcess::start(
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& arguments) {
    if (running()) {
        throw std::runtime_error("process is already running");
    }

    const std::string executable_string = executable.string();
    const std::string working_directory_string = working_directory.string();
    std::vector<std::string> owned_arguments;
    owned_arguments.reserve(arguments.size() + 1);
    owned_arguments.push_back(executable_string);
    owned_arguments.insert(
        owned_arguments.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(owned_arguments.size() + 1);
    for (auto& argument : owned_arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    int stdin_pipe[2]{-1, -1};
    int stdout_pipe[2]{-1, -1};
    if (pipe2(stdin_pipe, O_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("failed to create process stdin pipe: ") +
                                 std::strerror(errno));
    }
    if (pipe2(stdout_pipe, O_CLOEXEC) != 0) {
        const int pipe_error = errno;
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        throw std::runtime_error(std::string("failed to create process stdout pipe: ") +
                                 std::strerror(pipe_error));
    }

    const pid_t child = fork();
    if (child < 0) {
        const int fork_error = errno;
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        throw std::runtime_error(std::string("failed to fork: ") +
                                 std::strerror(fork_error));
    }
    if (child == 0) {
        if (setpgid(0, 0) != 0 ||
            chdir(working_directory_string.c_str()) != 0 ||
            dup2(stdin_pipe[0], STDIN_FILENO) < 0 ||
            dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stdout_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        execv(executable_string.c_str(), argv.data());
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        const int group_error = errno;
        terminate_startup_child(child);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        throw std::runtime_error(std::string("failed to establish process group: ") +
                                 std::strerror(group_error));
    }
    const int flags = fcntl(stdout_pipe[0], F_GETFL, 0);
    int output_error = 0;
    if (flags < 0) {
        output_error = errno;
    } else if (fcntl(stdout_pipe[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        output_error = errno;
    }
    if (output_error != 0) {
        terminate_startup_child(child);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        throw std::runtime_error(std::string("failed to configure process output pipe: ") +
                                 std::strerror(output_error));
    }

    impl_->process_id = child;
    impl_->stdin_fd = stdin_pipe[1];
    impl_->stdout_fd = stdout_pipe[0];
    impl_->reader_stop_requested.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->lines.clear();
        impl_->dropped = 0;
        impl_->reader_finished = false;
    }
    impl_->reader = std::thread([this] { impl_->read_output(); });
}

void NativeProcess::write_line(const std::string& line) {
    if (!running()) {
        throw std::runtime_error("cannot write to a stopped process");
    }
    const std::string framed = line + "\n";
    ScopedSigpipeBlock sigpipe;
    std::size_t offset = 0;
    while (offset < framed.size()) {
        const ssize_t written = write(
            impl_->stdin_fd, framed.data() + offset, framed.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            const int write_error = errno;
            if (write_error == EPIPE) {
                sigpipe.consume_generated_sigpipe();
            }
            throw std::runtime_error(std::string("failed to write to process: ") +
                                     std::strerror(write_error));
        }
        offset += static_cast<std::size_t>(written);
    }
}

std::optional<std::string> NativeProcess::read_line(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    impl_->condition.wait_for(lock, timeout, [this] {
        return !impl_->lines.empty() || impl_->reader_finished;
    });
    if (impl_->lines.empty()) {
        return std::nullopt;
    }
    std::string line = std::move(impl_->lines.front());
    impl_->lines.pop_front();
    return line;
}

bool NativeProcess::running() const {
    if (impl_->process_id <= 0) {
        return false;
    }
    siginfo_t information{};
    if (waitid(P_PID, impl_->process_id, &information,
               WEXITED | WNOHANG | WNOWAIT) != 0) {
        if (errno == ECHILD) {
            return false;
        }
        throw std::runtime_error(std::string("failed to inspect process state: ") +
                                 std::strerror(errno));
    }
    return information.si_pid == 0;
}

std::uint64_t NativeProcess::pid() const {
    return impl_->process_id > 0 ? static_cast<std::uint64_t>(impl_->process_id) : 0;
}

std::uint64_t NativeProcess::dropped_observational_lines() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->dropped;
}

void NativeProcess::stop(std::chrono::milliseconds graceful_timeout) {
    if (graceful_timeout.count() < 0) {
        throw std::runtime_error("process graceful timeout must be non-negative");
    }
    if (impl_->process_id <= 0) {
        return;
    }

    const pid_t process_id = impl_->process_id;
    std::string failure;
    bool direct_running = true;
    try {
        direct_running = running();
    } catch (const std::exception& error) {
        remember_failure(failure, error.what());
    }

    if (direct_running) {
        try {
            write_line("quit");
        } catch (...) {
        }
        const auto deadline = std::chrono::steady_clock::now() + graceful_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                if (!running()) {
                    direct_running = false;
                    break;
                }
            } catch (const std::exception& error) {
                remember_failure(failure, error.what());
                break;
            }
            poll(nullptr, 0, 10);
        }
    }

    if (kill(-process_id, SIGKILL) != 0 && errno != ESRCH) {
        const int group_error = errno;
        remember_failure(
            failure,
            std::string("failed to terminate process group: ") +
                std::strerror(group_error));
        if (kill(process_id, SIGKILL) != 0 && errno != ESRCH) {
            remember_failure(
                failure,
                std::string("failed to terminate direct process: ") +
                    std::strerror(errno));
        }
    }

    int status = 0;
    pid_t wait_result = -1;
    do {
        wait_result = waitpid(process_id, &status, 0);
    } while (wait_result < 0 && errno == EINTR);
    if (wait_result < 0 && errno != ECHILD) {
        remember_failure(
            failure,
            std::string("failed to reap direct process: ") +
                std::strerror(errno));
    }

    if (!wait_for_process_group_exit(process_id, std::chrono::seconds(2))) {
        remember_failure(
            failure,
            "process group still has live members after forced shutdown");
    }

    close_fd(impl_->stdin_fd);
    impl_->reader_stop_requested.store(true, std::memory_order_release);
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    close_fd(impl_->stdout_fd);
    impl_->process_id = -1;

    if (!failure.empty()) {
        throw std::runtime_error(failure);
    }
}

} // namespace arena::ports
