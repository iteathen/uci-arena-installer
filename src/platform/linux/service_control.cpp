#include "arena/platform/service_control.hpp"

#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace arena::platform {
namespace {

constexpr auto command_timeout = std::chrono::seconds(30);
constexpr std::size_t maximum_captured_characters = 1'024;

struct CommandResult final {
    bool timed_out{false};
    bool exited{false};
    int exit_code{0};
    std::string output;
};

struct SystemdProperties final {
    std::string load_state;
    std::string active_state;
    bool malformed{false};
};

void append_bounded(std::string& output, const char* bytes, const std::size_t count) {
    if (output.size() >= maximum_captured_characters) {
        return;
    }
    const std::size_t accepted = std::min(
        count,
        maximum_captured_characters - output.size());
    output.append(bytes, accepted);
}

void drain_pipe(const int descriptor, std::string& output) {
    char buffer[512];
    while (true) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count > 0) {
            append_bounded(output, buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        return;
    }
}

CommandResult run_systemctl(const std::vector<std::string>& arguments) {
    int descriptors[2]{};
    if (pipe(descriptors) != 0) {
        throw std::runtime_error("cannot create systemctl output pipe");
    }

    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw std::runtime_error("cannot fork systemctl");
    }
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0
            || dup2(descriptors[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(descriptors[1]);

        std::vector<char*> values;
        values.reserve(arguments.size() + 2);
        values.push_back(const_cast<char*>("systemctl"));
        for (const std::string& argument : arguments) {
            values.push_back(const_cast<char*>(argument.c_str()));
        }
        values.push_back(nullptr);
        execv("/usr/bin/systemctl", values.data());
        _exit(127);
    }

    close(descriptors[1]);
    const int flags = fcntl(descriptors[0], F_GETFL, 0);
    if (flags >= 0) {
        static_cast<void>(fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK));
    }

    CommandResult result;
    int wait_status = 0;
    const auto deadline = std::chrono::steady_clock::now() + command_timeout;
    while (!result.exited) {
        drain_pipe(descriptors[0], result.output);
        const pid_t waited = waitpid(child, &wait_status, WNOHANG);
        if (waited == child) {
            result.exited = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            close(descriptors[0]);
            throw std::runtime_error("cannot wait for systemctl");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            static_cast<void>(kill(child, SIGKILL));
            while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {}
            result.exited = true;
            break;
        }
        pollfd descriptor{descriptors[0], POLLIN | POLLHUP, 0};
        static_cast<void>(poll(&descriptor, 1, 25));
    }
    drain_pipe(descriptors[0], result.output);
    close(descriptors[0]);

    if (WIFEXITED(wait_status)) {
        result.exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        result.exit_code = 128 + WTERMSIG(wait_status);
    } else {
        result.exit_code = 125;
    }
    return result;
}

SystemdProperties parse_systemd_properties(const std::string_view output) {
    SystemdProperties result;
    std::size_t cursor = 0;
    while (cursor <= output.size()) {
        const std::size_t newline = output.find('\n', cursor);
        const std::size_t end = newline == std::string_view::npos
            ? output.size()
            : newline;
        std::string_view line = output.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const auto assign = [&result](
            std::string& field,
            const std::string_view value) {
            if (!field.empty() && field != value) {
                result.malformed = true;
                return;
            }
            field = value;
        };
        if (line.starts_with("LoadState=")) {
            assign(result.load_state, line.substr(10));
        } else if (line.starts_with("ActiveState=")) {
            assign(result.active_state, line.substr(12));
        }
        if (newline == std::string_view::npos) {
            break;
        }
        cursor = newline + 1;
    }
    if (result.load_state.empty() || result.active_state.empty()) {
        result.malformed = true;
    }
    return result;
}

ServiceActionResult failed_result(
    const ServiceAction action,
    const ServiceState state,
    const std::uint32_t platform_code,
    const std::string_view diagnostic) {
    return ServiceActionResult{
        action,
        ServiceOutcome::failed,
        state,
        platform_code,
        bounded_service_diagnostic(diagnostic),
    };
}

ServiceActionResult query_service_state(const std::string& service_name) {
    const CommandResult query = run_systemctl({
        "show",
        "--no-pager",
        "--property=LoadState",
        "--property=ActiveState",
        "--",
        service_name,
    });
    if (query.timed_out) {
        return failed_result(
            ServiceAction::status,
            ServiceState::unknown,
            static_cast<std::uint32_t>(ETIMEDOUT),
            "systemd service status query timed out");
    }

    const SystemdProperties properties = parse_systemd_properties(query.output);
    const ServiceState state = properties.malformed
        ? ServiceState::unknown
        : service_state_from_systemd(
            properties.active_state,
            properties.load_state);
    if (state == ServiceState::unavailable) {
        return failed_result(
            ServiceAction::status,
            state,
            static_cast<std::uint32_t>(query.exit_code),
            "systemd service is unavailable");
    }
    if (query.exit_code != 0) {
        return failed_result(
            ServiceAction::status,
            state,
            static_cast<std::uint32_t>(query.exit_code),
            query.output.empty()
                ? "systemd service status query failed"
                : query.output);
    }
    if (properties.malformed || state == ServiceState::unknown) {
        return failed_result(
            ServiceAction::status,
            ServiceState::unknown,
            0,
            "systemd returned incomplete or unrecognized service state");
    }
    return ServiceActionResult{
        ServiceAction::status,
        ServiceOutcome::completed,
        state,
        0,
        {},
    };
}

} // namespace

ServiceActionResult perform_service_action(
    const std::string& platform_name,
    const ServiceAction action) {
    if (platform_name.empty()) {
        throw std::runtime_error("service name is required");
    }
    if (!valid_service_name(platform_name)) {
        throw std::runtime_error("service name is invalid");
    }
    const std::string_view action_name = service_action_name(action);
    if (action_name.empty()) {
        throw std::runtime_error("unsupported service action");
    }

    if (action == ServiceAction::status) {
        return query_service_state(platform_name);
    }

    const CommandResult command = run_systemctl({
        std::string(action_name),
        "--no-ask-password",
        "--no-pager",
        "--",
        platform_name,
    });
    if (command.timed_out) {
        return failed_result(
            action,
            ServiceState::unknown,
            static_cast<std::uint32_t>(ETIMEDOUT),
            "systemd service action timed out");
    }

    ServiceActionResult observed = query_service_state(platform_name);
    observed.action = action;
    if (command.exit_code != 0) {
        observed.outcome = ServiceOutcome::failed;
        observed.platform_code = static_cast<std::uint32_t>(command.exit_code);
        observed.diagnostic = bounded_service_diagnostic(
            command.output.empty()
                ? "systemd service action failed"
                : command.output);
        return observed;
    }
    if (!observed.completed()) {
        return observed;
    }

    const ServiceState expected = action == ServiceAction::stop
        ? ServiceState::inactive
        : ServiceState::active;
    if (observed.state != expected) {
        observed.outcome = ServiceOutcome::failed;
        observed.diagnostic = bounded_service_diagnostic(
            "systemd service did not reach the required final state");
    }
    return observed;
}

} // namespace arena::platform
