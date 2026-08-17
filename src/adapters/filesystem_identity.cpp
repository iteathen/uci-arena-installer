#include "arena/ports/filesystem_identity.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace arena::ports {
namespace {

constexpr std::size_t kMaximumPathUtf8Bytes = 131072;
constexpr std::size_t kMaximumLeafUtf8Bytes = 4096;
constexpr std::size_t kMaximumDiagnosticUtf8Bytes = 512;

[[nodiscard]] std::string bounded_message(std::string message) {
    if (message.size() > kMaximumDiagnosticUtf8Bytes) {
        message.resize(kMaximumDiagnosticUtf8Bytes);
    }
    return message;
}

[[nodiscard]] bool contains_disallowed_character(
    const std::string_view text) noexcept {
    return std::ranges::any_of(text, [](const char value) {
        return value == '\0' || value == '\r' || value == '\n';
    });
}

[[nodiscard]] std::string path_as_utf8(
    const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void throw_invalid_path() {
    throw FilesystemIdentityException(
        FilesystemIdentityErrorCode::invalid_path,
        "filesystem path is invalid");
}

void validate_absolute_path(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute() || path.lexically_normal() != path) {
        throw_invalid_path();
    }

    std::string encoded;
    try {
        encoded = path_as_utf8(path);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_invalid_path();
    }

    if (encoded.empty() || encoded.size() > kMaximumPathUtf8Bytes ||
        contains_disallowed_character(encoded)) {
        throw_invalid_path();
    }
}

void validate_regular_file_path(const std::filesystem::path& path) {
    validate_absolute_path(path);
    if (path.filename().empty() || path.parent_path().empty()) {
        throw_invalid_path();
    }
}

void validate_leaf(const std::string_view leaf) {
    if (leaf.empty() || leaf == "." || leaf == ".." ||
        leaf.size() > kMaximumLeafUtf8Bytes ||
        contains_disallowed_character(leaf) ||
        leaf.find('/') != std::string_view::npos ||
        leaf.find('\\') != std::string_view::npos) {
        throw_invalid_path();
    }

    try {
        std::u8string encoded;
        encoded.reserve(leaf.size());
        for (const unsigned char byte : leaf) {
            encoded.push_back(static_cast<char8_t>(byte));
        }
        const std::filesystem::path candidate{encoded};
        if (candidate.is_absolute() || candidate.has_root_name() ||
            candidate.has_root_directory() || candidate.filename().empty() ||
            candidate.parent_path() != std::filesystem::path{}) {
            throw_invalid_path();
        }
    } catch (const FilesystemIdentityException&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_invalid_path();
    }
}

void validate_regular_file(const AdmittedRegularFile& file) {
    if (file.initial_observation().kind !=
        FilesystemObjectKind::regular_file) {
        throw FilesystemIdentityException(
            FilesystemIdentityErrorCode::wrong_kind,
            "filesystem adapter returned the wrong object kind");
    }
}

void validate_directory(const AdmittedDirectory& directory) {
    const auto& observation = directory.initial_observation();
    if (observation.kind != FilesystemObjectKind::directory ||
        observation.size_bytes != 0) {
        throw FilesystemIdentityException(
            FilesystemIdentityErrorCode::wrong_kind,
            "filesystem adapter returned an invalid directory observation");
    }
}

[[noreturn]] void throw_unexpected_adapter_failure() {
    throw FilesystemIdentityException(
        FilesystemIdentityErrorCode::io_failure,
        "filesystem adapter failed unexpectedly");
}

} // namespace

FilesystemIdentityException::FilesystemIdentityException(
    const FilesystemIdentityErrorCode code,
    std::string message)
    : std::runtime_error(bounded_message(std::move(message))), code_(code) {}

FilesystemIdentityErrorCode FilesystemIdentityException::code() const noexcept {
    return code_;
}

AdmittedDirectory::AdmittedDirectory(
    std::filesystem::path absolute_path,
    FilesystemObservation initial_observation)
    : absolute_path_(std::move(absolute_path)),
      initial_observation_(std::move(initial_observation)) {}

const std::filesystem::path&
AdmittedDirectory::absolute_path() const noexcept {
    return absolute_path_;
}

const FilesystemObservation&
AdmittedDirectory::initial_observation() const noexcept {
    return initial_observation_;
}

std::unique_ptr<AdmittedRegularFile>
AdmittedDirectory::open_regular_file(const std::string_view leaf) const {
    validate_leaf(leaf);
    try {
        auto child = open_regular_file_validated(leaf);
        if (!child) {
            throw_unexpected_adapter_failure();
        }
        validate_regular_file(*child);
        return child;
    } catch (const FilesystemIdentityException&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_unexpected_adapter_failure();
    }
}

std::unique_ptr<AdmittedDirectory>
AdmittedDirectory::open_directory(const std::string_view leaf) const {
    validate_leaf(leaf);
    try {
        auto child = open_directory_validated(leaf);
        if (!child) {
            throw_unexpected_adapter_failure();
        }
        validate_directory(*child);
        return child;
    } catch (const FilesystemIdentityException&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_unexpected_adapter_failure();
    }
}

std::unique_ptr<AdmittedRegularFile>
FilesystemIdentity::admit_regular_file(
    const std::filesystem::path& absolute_path) {
    validate_regular_file_path(absolute_path);
    try {
        auto file = admit_regular_file_validated(absolute_path);
        if (!file) {
            throw_unexpected_adapter_failure();
        }
        validate_regular_file(*file);
        return file;
    } catch (const FilesystemIdentityException&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_unexpected_adapter_failure();
    }
}

std::unique_ptr<AdmittedDirectory>
FilesystemIdentity::admit_directory(
    const std::filesystem::path& absolute_path) {
    validate_absolute_path(absolute_path);
    try {
        auto directory = admit_directory_validated(absolute_path);
        if (!directory) {
            throw_unexpected_adapter_failure();
        }
        validate_directory(*directory);
        return directory;
    } catch (const FilesystemIdentityException&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_unexpected_adapter_failure();
    }
}

} // namespace arena::ports
