#include "arena/application/process_supervisor.hpp"
#include "arena/ports/process_command_failure.hpp"

#include <stdexcept>
#include <limits>
#include <thread>

namespace arena::application {

ProcessSupervisor::~ProcessSupervisor() {
    std::map<std::string, std::unique_ptr<ports::NativeProcess>, std::less<>> processes;
    {
        std::scoped_lock lock(mutex_);
        processes.swap(processes_);
    }
    for (auto& [id, process] : processes) {
        (void)id;
        try {
            process->stop(std::chrono::seconds(2));
        } catch (...) {
        }
    }
}

void ProcessSupervisor::start(
    const std::string& resource_id,
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& arguments) {
    auto process = std::make_unique<ports::NativeProcess>();
    process->start(executable, working_directory, arguments);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!process->running()) {
        process->stop(std::chrono::milliseconds(100));
        throw std::runtime_error("managed service exited during startup");
    }
    std::scoped_lock lock(mutex_);
    const auto existing = processes_.find(resource_id);
    if (existing != processes_.end() && existing->second->running()) {
        process->stop(std::chrono::milliseconds(250));
        throw std::runtime_error("managed service is already running");
    }
    processes_[resource_id] = std::move(process);
}

void ProcessSupervisor::stop(
    const std::string& resource_id,
    std::chrono::milliseconds graceful_timeout) {
    std::unique_ptr<ports::NativeProcess> process;
    {
        std::scoped_lock lock(mutex_);
        const auto existing = processes_.find(resource_id);
        if (existing == processes_.end()) {
            return;
        }
        process = std::move(existing->second);
        processes_.erase(existing);
    }
    process->stop(graceful_timeout);
}

bool ProcessSupervisor::running(const std::string& resource_id) const {
    std::scoped_lock lock(mutex_);
    const auto process = processes_.find(resource_id);
    return process != processes_.end() && process->second->running();
}

nlohmann::json ProcessSupervisor::run_json(
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::vector<std::string>& arguments,
    std::chrono::milliseconds timeout,
    std::optional<std::uint64_t> maximum_stdout_bytes) const {
    if (maximum_stdout_bytes && (*maximum_stdout_bytes == 0 ||
        *maximum_stdout_bytes >= std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument("invalid machine stdout byte bound");
    }
    ports::NativeProcess process;
    try {
        process.start(executable, working_directory, arguments,
                      ports::NativeProcessOutputMode::machine_stdout, maximum_stdout_bytes);
    } catch (const std::bad_alloc&) { throw; }
    catch (const ports::ProcessCommandFailure&) { throw; }
    catch (const std::exception&) {
        throw ports::ProcessCommandFailure(ports::ProcessCommandFailureKind::start_failed);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string output;
    const auto append = [&](const std::string& line) {
        // A final no-newline frame gains one whitespace byte in this line projection.
        if (maximum_stdout_bytes &&
            line.size() + 1 > *maximum_stdout_bytes + 1 - output.size()) {
            process.stop(std::chrono::milliseconds(0));
            throw ports::ProcessCommandFailure(ports::ProcessCommandFailureKind::output_limit,
                                               process.captured_stdout_bytes(), process.exit_code());
        }
        output += line;
        output.push_back('\n');
    };
    while (std::chrono::steady_clock::now() < deadline) {
        if (process.stdout_limit_exceeded()) {
            process.stop(std::chrono::milliseconds(0));
            throw ports::ProcessCommandFailure(ports::ProcessCommandFailureKind::output_limit,
                                               process.captured_stdout_bytes(), process.exit_code());
        }
        if (const auto line = process.read_line(std::chrono::milliseconds(100)); line) {
            append(*line);
            continue;
        }
        if (!process.running()) {
            break;
        }
    }
    if (process.running()) {
        process.stop(std::chrono::milliseconds(250));
        throw ports::ProcessCommandFailure(ports::ProcessCommandFailureKind::timeout,
                                           process.captured_stdout_bytes(), process.exit_code());
    }
    // Join the bounded reader before draining, including the final no-newline frame.
    process.stop(std::chrono::milliseconds(0));
    while (const auto line = process.read_line(std::chrono::milliseconds(0))) {
        append(*line);
    }
    if (process.stdout_limit_exceeded() || process.dropped_observational_lines() != 0) {
        throw ports::ProcessCommandFailure(ports::ProcessCommandFailureKind::output_limit,
                                           process.captured_stdout_bytes(), process.exit_code());
    }
    try {
        return nlohmann::json::parse(output);
    } catch (const nlohmann::json::exception&) {
        const auto code = process.exit_code();
        throw ports::ProcessCommandFailure(
            code && *code != 0 ? ports::ProcessCommandFailureKind::nonzero_exit
                              : ports::ProcessCommandFailureKind::invalid_json,
            process.captured_stdout_bytes(), code);
    }
}

}  // namespace arena::application
