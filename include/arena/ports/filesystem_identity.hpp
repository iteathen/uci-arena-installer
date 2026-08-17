#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace arena::ports {

enum class FilesystemObjectKind {
    regular_file,
    directory,
};

enum class PathBindingState {
    same_object,
    missing,
    different_object,
    uninspectable,
};

enum class FilesystemIdentityErrorCode {
    invalid_path,
    missing,
    wrong_kind,
    link_or_reparse,
    special_object,
    access_denied,
    identity_unavailable,
    identity_changed,
    io_failure,
};

using FilesystemIdentityToken = std::array<std::byte, 32>;

struct FilesystemObservation {
    FilesystemObjectKind kind{};
    std::uint64_t size_bytes{};
    FilesystemIdentityToken object_identity{};
    FilesystemIdentityToken metadata_identity{};

    [[nodiscard]] bool operator==(const FilesystemObservation&) const noexcept = default;
};

class FilesystemIdentityException final : public std::runtime_error {
  public:
    FilesystemIdentityException(FilesystemIdentityErrorCode code,
                                std::string message);

    [[nodiscard]] FilesystemIdentityErrorCode code() const noexcept;

  private:
    FilesystemIdentityErrorCode code_;
};

class AdmittedRegularFile {
  public:
    virtual ~AdmittedRegularFile() = default;

    [[nodiscard]] virtual const std::filesystem::path&
    absolute_path() const noexcept = 0;
    [[nodiscard]] virtual const FilesystemObservation&
    initial_observation() const noexcept = 0;
    [[nodiscard]] virtual std::size_t
    read_at(std::uint64_t offset, std::span<std::byte> destination) = 0;
    [[nodiscard]] virtual FilesystemObservation observe_open_object() const = 0;
    [[nodiscard]] virtual PathBindingState observe_path_binding() const = 0;
};

class AdmittedDirectory {
  public:
    virtual ~AdmittedDirectory() = default;

    [[nodiscard]] const std::filesystem::path& absolute_path() const noexcept;
    [[nodiscard]] const FilesystemObservation& initial_observation() const noexcept;

    [[nodiscard]] std::unique_ptr<AdmittedRegularFile>
    open_regular_file(std::string_view leaf) const;
    [[nodiscard]] std::unique_ptr<AdmittedDirectory>
    open_directory(std::string_view leaf) const;

    [[nodiscard]] virtual FilesystemObservation observe_open_object() const = 0;
    [[nodiscard]] virtual PathBindingState observe_path_binding() const = 0;

  protected:
    AdmittedDirectory(std::filesystem::path absolute_path,
                      FilesystemObservation initial_observation);

    [[nodiscard]] virtual std::unique_ptr<AdmittedRegularFile>
    open_regular_file_validated(std::string_view leaf) const = 0;
    [[nodiscard]] virtual std::unique_ptr<AdmittedDirectory>
    open_directory_validated(std::string_view leaf) const = 0;

  private:
    std::filesystem::path absolute_path_;
    FilesystemObservation initial_observation_;
};

class FilesystemIdentity {
  public:
    virtual ~FilesystemIdentity() = default;

    [[nodiscard]] std::unique_ptr<AdmittedRegularFile>
    admit_regular_file(const std::filesystem::path& absolute_path);
    [[nodiscard]] std::unique_ptr<AdmittedDirectory>
    admit_directory(const std::filesystem::path& absolute_path);

  protected:
    [[nodiscard]] virtual std::unique_ptr<AdmittedRegularFile>
    admit_regular_file_validated(const std::filesystem::path& absolute_path) = 0;
    [[nodiscard]] virtual std::unique_ptr<AdmittedDirectory>
    admit_directory_validated(const std::filesystem::path& absolute_path) = 0;
};

} // namespace arena::ports
