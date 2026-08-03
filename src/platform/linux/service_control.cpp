#include "arena/platform/service_control.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace arena::platform {

int perform_service_action(
    const std::string& platform_name,
    const ServiceAction action) {
    if (platform_name.empty()) {
        throw std::runtime_error("service name is required");
    }
    const std::string_view action_name = service_action_name(action);
    if (action_name.empty()) {
        throw std::runtime_error("service action is invalid");
    }

    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("cannot fork systemctl");
    }
    if (child == 0) {
        execl(
            "/usr/bin/systemctl",
            "systemctl",
            action_name.data(),
            "--",
            platform_name.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error("cannot wait for systemctl");
        }
    }
    if (!WIFEXITED(status)) {
        throw std::runtime_error("systemctl did not exit normally");
    }
    return WEXITSTATUS(status);
}

}  // namespace arena::platform
