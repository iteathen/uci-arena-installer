#ifdef _WIN32

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "arena/application/installer/result.hpp"
#include "arena/platform/windows_installer_launcher.hpp"

namespace {

void encode_u32(std::vector<std::byte>& bytes, const std::size_t offset,
                const std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void encode_u64(std::vector<std::byte>& bytes, const std::size_t offset,
                const std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

std::vector<std::byte> envelope_fixture() {
    constexpr std::size_t stub_size = 3;
    constexpr std::size_t per_user_size = 2;
    constexpr std::size_t per_machine_size = 4;
    constexpr std::array<unsigned char, 16> magic{
        'U', 'C', 'I', 'A', 'R', 'E', 'N', 'A',
        'I', 'N', 'S', 'T', 'V', '1', '!', 0,
    };
    std::vector<std::byte> bytes(
        stub_size + per_user_size + per_machine_size +
        arena::platform::windows_installer_envelope_footer_size);
    const std::size_t footer =
        bytes.size() - arena::platform::windows_installer_envelope_footer_size;
    for (std::size_t index = 0; index < magic.size(); ++index) {
        bytes[footer + index] = static_cast<std::byte>(magic[index]);
    }
    encode_u32(bytes, footer + 16, 1);
    encode_u64(bytes, footer + 24, stub_size);
    encode_u64(bytes, footer + 32, stub_size);
    encode_u64(bytes, footer + 40, per_user_size);
    encode_u64(bytes, footer + 80, stub_size + per_user_size);
    encode_u64(bytes, footer + 88, per_machine_size);
    return bytes;
}

} // namespace

TEST_CASE("Windows installer envelope exposes exact fixed-scope boundaries", "[setup]") {
    const auto bytes = envelope_fixture();
    const auto envelope =
        arena::platform::parse_windows_installer_envelope(bytes);
    REQUIRE(envelope.stub_size == 3);
    REQUIRE(envelope.per_user.offset == 3);
    REQUIRE(envelope.per_user.size == 2);
    REQUIRE(envelope.per_machine.offset == 5);
    REQUIRE(envelope.per_machine.size == 4);
}

TEST_CASE("Windows installer envelope rejects noncontiguous payloads", "[setup]") {
    auto bytes = envelope_fixture();
    const std::size_t footer =
        bytes.size() - arena::platform::windows_installer_envelope_footer_size;
    encode_u64(bytes, footer + 80, 6);
    REQUIRE_THROWS_AS(
        arena::platform::parse_windows_installer_envelope(bytes),
        std::runtime_error);
}

TEST_CASE("Windows installer scope plan preserves installed authority", "[setup]") {
    using arena::platform::WindowsInstallScope;
    const auto personal = arena::platform::plan_windows_install_scope(
        {.per_user_installed = true, .per_machine_installed = false},
        std::nullopt);
    REQUIRE(personal.allowed);
    REQUIRE(personal.selected_scope == WindowsInstallScope::per_user);
    REQUIRE_FALSE(personal.selection_required);

    const auto cross_scope = arena::platform::plan_windows_install_scope(
        {.per_user_installed = true, .per_machine_installed = false},
        WindowsInstallScope::per_machine);
    REQUIRE_FALSE(cross_scope.allowed);

    const auto conflicting = arena::platform::plan_windows_install_scope(
        {.per_user_installed = true, .per_machine_installed = true},
        std::nullopt);
    REQUIRE_FALSE(conflicting.allowed);
}

TEST_CASE("Windows installer scope plan recommends an explicit clean choice", "[setup]") {
    using arena::platform::WindowsInstallScope;
    const auto interactive = arena::platform::plan_windows_install_scope(
        {}, std::nullopt);
    REQUIRE(interactive.allowed);
    REQUIRE(interactive.selection_required);

    const auto personal = arena::platform::plan_windows_install_scope(
        {}, WindowsInstallScope::per_user);
    REQUIRE(personal.allowed);
    REQUIRE(personal.selected_scope == WindowsInstallScope::per_user);
    REQUIRE_FALSE(personal.selection_required);
}

TEST_CASE("installer exit categories are stable", "[setup]") {
    using arena::application::installer::InstallerExitCode;
    using arena::application::installer::installer_exit_category;
    REQUIRE(static_cast<int>(InstallerExitCode::success) == 0);
    REQUIRE(static_cast<int>(InstallerExitCode::invalid_invocation) == 2);
    REQUIRE(static_cast<int>(InstallerExitCode::operator_cancelled) == 15);
    REQUIRE(installer_exit_category(InstallerExitCode::rollback_failed) ==
            "rollback_failed");
}

#endif
