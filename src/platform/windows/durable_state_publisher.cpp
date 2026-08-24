#include "arena/platform/durable_state_publisher.hpp"

#include "arena/application/installer/exact_document.hpp"
#include "arena/platform/filesystem_path.hpp"
#include "durable_state_publisher_private.hpp"

#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace arena::platform {
namespace {

namespace fs = std::filesystem;
namespace installer = arena::application::installer;
namespace ports = arena::ports;
namespace win = arena::platform::windows_detail;

using Clock = std::chrono::steady_clock;

class UniqueHandle final {
  public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this == &other) return *this;
        reset();
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    void reset() noexcept {
        if (valid()) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class LocalAllocation final {
  public:
    LocalAllocation() noexcept = default;
    explicit LocalAllocation(void* value) noexcept : value_(value) {}
    ~LocalAllocation() {
        if (value_ != nullptr) LocalFree(value_);
    }

    LocalAllocation(const LocalAllocation&) = delete;
    LocalAllocation& operator=(const LocalAllocation&) = delete;

    LocalAllocation(LocalAllocation&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}

    LocalAllocation& operator=(LocalAllocation&& other) noexcept {
        if (this == &other) return *this;
        if (value_ != nullptr) LocalFree(value_);
        value_ = std::exchange(other.value_, nullptr);
        return *this;
    }

    [[nodiscard]] void* get() const noexcept { return value_; }

  private:
    void* value_{};
};

class PrivateSecurity final {
  public:
    PrivateSecurity() {
        PSECURITY_DESCRIPTOR descriptor{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)",
                SDDL_REVISION_1, &descriptor, nullptr)) {
            return;
        }
        descriptor_ = LocalAllocation{descriptor};
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = descriptor_.get();
        attributes_.bInheritHandle = FALSE;
        valid_ = true;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept {
        return valid_ ? &attributes_ : nullptr;
    }

  private:
    LocalAllocation descriptor_;
    SECURITY_ATTRIBUTES attributes_{};
    bool valid_{};
};

class ExclusiveFileLock final {
  public:
    ExclusiveFileLock() noexcept = default;
    ~ExclusiveFileLock() {
        if (locked_ && handle_ != nullptr) {
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
        }
    }

    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;

    [[nodiscard]] bool acquire(HANDLE handle,
                               const Clock::time_point deadline) noexcept {
        handle_ = handle;
        while (Clock::now() < deadline) {
            if (LockFileEx(handle_,
                           LOCKFILE_EXCLUSIVE_LOCK |
                               LOCKFILE_FAIL_IMMEDIATELY,
                           0, MAXDWORD, MAXDWORD, &overlapped_)) {
                locked_ = true;
                return true;
            }
            const DWORD error = GetLastError();
            if (error != ERROR_LOCK_VIOLATION && error != ERROR_IO_PENDING) {
                return false;
            }
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) break;
            const auto maximum_pause =
                std::chrono::duration_cast<Clock::duration>(
                    std::chrono::milliseconds{5});
            std::this_thread::sleep_for((std::min)(remaining, maximum_pause));
        }
        return false;
    }

  private:
    HANDLE handle_{};
    OVERLAPPED overlapped_{};
    bool locked_{};
};

enum class AuthorityState {
    accepted,
    broad,
    uninspectable,
};

enum class TargetState {
    absent,
    digest,
    uninspectable,
};

struct TargetInspection {
    TargetState state{TargetState::uninspectable};
    std::optional<std::string> digest;
};

struct StageState {
    fs::path path;
    std::string leaf;
    std::optional<ports::FilesystemIdentityToken> object_identity;
    bool created{};
};

[[nodiscard]] ports::PublicationDiagnostic diagnostic(std::string code) {
    return {std::move(code), {}};
}

[[nodiscard]] ports::PublicationResult result(
    const ports::PublicationRequest& request,
    const std::string_view candidate_digest,
    const ports::PublicationOutcome outcome,
    std::string primary_code,
    std::optional<std::string> prior_digest = std::nullopt,
    std::optional<std::string> observed_digest = std::nullopt,
    std::vector<ports::PublicationDiagnostic> cleanup = {}) {
    std::optional<ports::PublicationDiagnostic> primary;
    if (!primary_code.empty()) {
        primary = diagnostic(std::move(primary_code));
    }
    return {
        outcome,
        static_cast<std::uint64_t>(request.bytes.size()),
        std::string{candidate_digest},
        std::move(prior_digest),
        std::move(observed_digest),
        std::move(primary),
        std::move(cleanup),
    };
}

[[nodiscard]] std::string path_leaf_utf8(const fs::path& path) {
    const auto encoded = path.filename().generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] fs::path ascii_leaf(const std::string_view leaf) {
    std::u8string encoded;
    encoded.reserve(leaf.size());
    for (const unsigned char byte : leaf) {
        encoded.push_back(static_cast<char8_t>(byte));
    }
    return fs::path{encoded};
}

[[nodiscard]] std::string lock_leaf(const std::string_view target_leaf) {
    constexpr std::string_view domain{
        "arena.durable-state.lock.windows.v1"};
    std::vector<unsigned char> bytes;
    bytes.reserve(domain.size() + target_leaf.size());
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    bytes.insert(bytes.end(), target_leaf.begin(), target_leaf.end());
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest_bytes{};
    crypto_hash_sha256(digest_bytes.data(), bytes.data(), bytes.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest_bytes.data(),
                   digest_bytes.size());
    return ".arena-state-lock-" +
           std::string{encoded.data(), encoded.size() - 1};
}

[[nodiscard]] bool native_regular_file(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &tag,
            static_cast<DWORD>(sizeof(tag))) ||
        (tag.FileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0) {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    if (GetFileType(handle) != FILE_TYPE_DISK) return false;
    FILE_STANDARD_INFO standard{};
    return GetFileInformationByHandleEx(
               handle, FileStandardInfo, &standard,
               static_cast<DWORD>(sizeof(standard))) != FALSE &&
           !standard.Directory;
}

[[nodiscard]] bool native_directory(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &tag,
            static_cast<DWORD>(sizeof(tag))) ||
        (tag.FileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0) {
        return false;
    }
    FILE_STANDARD_INFO standard{};
    return GetFileType(handle) == FILE_TYPE_DISK &&
           GetFileInformationByHandleEx(
               handle, FileStandardInfo, &standard,
               static_cast<DWORD>(sizeof(standard))) != FALSE &&
           standard.Directory;
}

[[nodiscard]] AuthorityState authority_state(HANDLE handle) noexcept {
    PACL dacl{};
    PSECURITY_DESCRIPTOR raw_descriptor{};
    const DWORD status = GetSecurityInfo(
        handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
        &dacl, nullptr, &raw_descriptor);
    LocalAllocation descriptor{raw_descriptor};
    if (status != ERROR_SUCCESS || raw_descriptor == nullptr || dacl == nullptr) {
        return status == ERROR_SUCCESS ? AuthorityState::broad
                                       : AuthorityState::uninspectable;
    }

    constexpr std::array broad_sids{
        WinWorldSid,
        WinAnonymousSid,
        WinAuthenticatedUserSid,
        WinBuiltinUsersSid,
        WinBuiltinGuestsSid,
    };
    constexpr ACCESS_MASK dangerous =
        FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | DELETE |
        WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
    for (const auto sid_type : broad_sids) {
        std::array<std::byte, SECURITY_MAX_SID_SIZE> sid_storage{};
        DWORD sid_size = static_cast<DWORD>(sid_storage.size());
        auto* sid = reinterpret_cast<PSID>(sid_storage.data());
        if (!CreateWellKnownSid(sid_type, nullptr, sid, &sid_size)) {
            return AuthorityState::uninspectable;
        }
        TRUSTEE_W trustee{};
        trustee.TrusteeForm = TRUSTEE_IS_SID;
        trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        trustee.ptstrName = static_cast<LPWSTR>(sid);
        ACCESS_MASK rights{};
        if (GetEffectiveRightsFromAclW(dacl, &trustee, &rights) !=
            ERROR_SUCCESS) {
            return AuthorityState::uninspectable;
        }
        if ((rights & dangerous) != 0) return AuthorityState::broad;
    }
    return AuthorityState::accepted;
}

[[nodiscard]] std::optional<std::vector<std::byte>> current_user_sid() {
    UniqueHandle token;
    HANDLE raw_token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return std::nullopt;
    }
    token = UniqueHandle{raw_token};
    DWORD bytes{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return std::nullopt;
    }
    std::vector<std::byte> storage(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, storage.data(), bytes,
                             &bytes)) {
        return std::nullopt;
    }
    return storage;
}

[[nodiscard]] bool trusted_owner(HANDLE handle) {
    PSID owner{};
    PSECURITY_DESCRIPTOR raw_descriptor{};
    const DWORD status = GetSecurityInfo(
        handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr,
        nullptr, nullptr, &raw_descriptor);
    LocalAllocation descriptor{raw_descriptor};
    if (status != ERROR_SUCCESS || owner == nullptr) return false;

    const auto user = current_user_sid();
    if (!user.has_value()) return false;
    const auto* token_user =
        reinterpret_cast<const TOKEN_USER*>(user->data());
    if (EqualSid(owner, token_user->User.Sid)) return true;

    for (const auto sid_type : {WinLocalSystemSid,
                                WinBuiltinAdministratorsSid}) {
        std::array<std::byte, SECURITY_MAX_SID_SIZE> sid_storage{};
        DWORD sid_size = static_cast<DWORD>(sid_storage.size());
        auto* sid = reinterpret_cast<PSID>(sid_storage.data());
        if (!CreateWellKnownSid(sid_type, nullptr, sid, &sid_size)) {
            return false;
        }
        if (EqualSid(owner, sid)) return true;
    }
    return false;
}

[[nodiscard]] UniqueHandle open_parent_for_acl(const fs::path& path) {
    const auto api_path = filesystem_api_path(path);
    return UniqueHandle{CreateFileW(
        api_path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
}

[[nodiscard]] bool before_deadline(const Clock::time_point deadline) noexcept {
    return Clock::now() < deadline;
}

class WindowsDurableStatePublisher final : public ports::DurableStatePublisher {
  public:
    WindowsDurableStatePublisher(
        ports::FilesystemIdentity& identity,
        win::DurableStatePublisherTestControl control = {})
        : identity_(identity), control_(control) {}

  protected:
    [[nodiscard]] ports::PublicationResult publish_validated(
        const ports::PublicationRequest& request,
        const std::string_view candidate_digest,
        const std::string_view random_stage_id,
        const Clock::time_point deadline) override {
        return publish_impl(request, candidate_digest, random_stage_id,
                            deadline);
    }

  private:
    [[nodiscard]] installer::ExactDocument exact_document(
        const ports::AdmittedDirectory& parent,
        const std::string_view leaf,
        const std::uint64_t maximum_bytes) {
        if (control_.exact_document_proofs != nullptr) {
            ++*control_.exact_document_proofs;
        }
        installer::ExactDocumentReader reader;
        return reader.read_child(parent, leaf, {maximum_bytes, false});
    }

    [[nodiscard]] TargetInspection inspect_target(
        const ports::AdmittedDirectory& parent,
        const std::string_view target_leaf,
        const std::uint64_t maximum_bytes) {
        if (control_.fail_post_move_inspection) {
            return {};
        }
        try {
            auto document = exact_document(parent, target_leaf, maximum_bytes);
            return {TargetState::digest, std::move(document.sha256)};
        } catch (const ports::FilesystemIdentityException& error) {
            if (error.code() == ports::FilesystemIdentityErrorCode::missing) {
                return {TargetState::absent, std::nullopt};
            }
            return {};
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] bool write_stage(
        HANDLE handle,
        const std::span<const std::byte> bytes,
        const Clock::time_point deadline) noexcept {
        std::size_t offset{};
        while (offset < bytes.size()) {
            if (!before_deadline(deadline)) return false;
            if (control_.simulated_write_failure_after_bytes != 0 &&
                offset >= control_.simulated_write_failure_after_bytes) {
                return false;
            }
            auto request_size = (std::min)(
                bytes.size() - offset,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()));
            if (control_.simulated_write_failure_after_bytes != 0) {
                request_size = (std::min)(
                    request_size,
                    control_.simulated_write_failure_after_bytes - offset);
            }
            if (control_.maximum_write_chunk != 0) {
                request_size =
                    (std::min)(request_size, control_.maximum_write_chunk);
            }
            if (request_size == 0) return false;
            if (control_.write_calls != nullptr) ++*control_.write_calls;
            DWORD written{};
            if (!WriteFile(handle, bytes.data() + offset,
                           static_cast<DWORD>(request_size), &written,
                           nullptr) ||
                written == 0 || written > request_size) {
                return false;
            }
            offset += written;
        }
        return true;
    }

    [[nodiscard]] bool flush_target(const fs::path& target) {
        if (control_.fail_target_flush) return false;
        const auto api_path = filesystem_api_path(target);
        UniqueHandle handle{CreateFileW(
            api_path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        return handle.valid() && native_regular_file(handle.get()) &&
               FlushFileBuffers(handle.get()) != FALSE;
    }

    void cleanup_stage(
        const ports::AdmittedDirectory& parent,
        const StageState& stage,
        std::vector<ports::PublicationDiagnostic>& cleanup) {
        if (!stage.created) return;
        if (control_.fail_stage_cleanup) {
            cleanup.push_back(diagnostic("stage_cleanup_failed"));
            return;
        }
        try {
            auto current = parent.open_regular_file(stage.leaf);
            if (!stage.object_identity.has_value() ||
                current->initial_observation().object_identity !=
                    *stage.object_identity ||
                current->observe_path_binding() !=
                    ports::PathBindingState::same_object) {
                cleanup.push_back(diagnostic("stage_cleanup_identity_changed"));
                return;
            }
        } catch (const ports::FilesystemIdentityException& error) {
            if (error.code() == ports::FilesystemIdentityErrorCode::missing) {
                return;
            }
            cleanup.push_back(diagnostic("stage_cleanup_uninspectable"));
            return;
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            cleanup.push_back(diagnostic("stage_cleanup_uninspectable"));
            return;
        }
        try {
            const auto api_path = filesystem_api_path(stage.path);
            if (!DeleteFileW(api_path.c_str())) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND &&
                    error != ERROR_PATH_NOT_FOUND) {
                    cleanup.push_back(diagnostic("stage_cleanup_failed"));
                }
            }
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            cleanup.push_back(diagnostic("stage_cleanup_failed"));
        }
    }

    [[nodiscard]] ports::PublicationResult publish_impl(
        const ports::PublicationRequest& request,
        const std::string_view candidate_digest,
        const std::string_view random_stage_id,
        const Clock::time_point deadline) {
        const auto parent_path = request.target.parent_path();
        const auto target_leaf = path_leaf_utf8(request.target);
        std::optional<std::string> prior_digest;

        std::unique_ptr<ports::AdmittedDirectory> parent;
        UniqueHandle native_parent;
        try {
            parent = identity_.admit_directory(parent_path);
            if (parent->observe_path_binding() !=
                ports::PathBindingState::same_object) {
                return result(request, candidate_digest,
                              ports::PublicationOutcome::not_activated,
                              "parent_binding_invalid");
            }
            native_parent = open_parent_for_acl(parent_path);
            if (!native_parent.valid() ||
                !native_directory(native_parent.get()) ||
                authority_state(native_parent.get()) !=
                    AuthorityState::accepted ||
                parent->observe_path_binding() !=
                    ports::PathBindingState::same_object) {
                return result(request, candidate_digest,
                              ports::PublicationOutcome::not_activated,
                              "parent_authority_rejected");
            }
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            return result(request, candidate_digest,
                          ports::PublicationOutcome::not_activated,
                          "parent_admission_failed");
        }

        const auto lease_leaf = lock_leaf(target_leaf);
        const auto lease_path = parent_path / ascii_leaf(lease_leaf);
        PrivateSecurity private_security;
        if (!private_security.valid()) {
            return result(request, candidate_digest,
                          ports::PublicationOutcome::not_activated,
                          "private_acl_unavailable");
        }
        const auto lease_api_path = filesystem_api_path(lease_path);
        UniqueHandle lease_handle{CreateFileW(
            lease_api_path.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            private_security.attributes(), OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!lease_handle.valid() || !native_regular_file(lease_handle.get()) ||
            authority_state(lease_handle.get()) != AuthorityState::accepted ||
            !trusted_owner(lease_handle.get())) {
            return result(request, candidate_digest,
                          ports::PublicationOutcome::not_activated,
                          "lease_object_rejected");
        }

        std::unique_ptr<ports::AdmittedRegularFile> lease_identity;
        try {
            lease_identity = parent->open_regular_file(lease_leaf);
            if (lease_identity->observe_path_binding() !=
                    ports::PathBindingState::same_object ||
                parent->observe_path_binding() !=
                    ports::PathBindingState::same_object) {
                return result(request, candidate_digest,
                              ports::PublicationOutcome::not_activated,
                              "lease_binding_invalid");
            }
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            return result(request, candidate_digest,
                          ports::PublicationOutcome::not_activated,
                          "lease_identity_failed");
        }

        ExclusiveFileLock lease;
        if (!lease.acquire(lease_handle.get(), deadline)) {
            return result(request, candidate_digest,
                          ports::PublicationOutcome::not_activated,
                          "lease_deadline");
        }
        if (lease_identity->observe_path_binding() !=
                ports::PathBindingState::same_object ||
            parent->observe_path_binding() !=
                ports::PathBindingState::same_object) {
            return result(request, candidate_digest,
                          ports::PublicationOutcome::not_activated,
                          "lease_binding_changed");
        }

        if (request.precondition == ports::PublicationPrecondition::absent) {
            try {
                auto occupied = parent->open_regular_file(target_leaf);
                (void)occupied;
                return result(request, candidate_digest,
                              ports::PublicationOutcome::not_activated,
                              "precondition_occupied");
            } catch (const ports::FilesystemIdentityException& error) {
                if (error.code() !=
                    ports::FilesystemIdentityErrorCode::missing) {
                    return result(request, candidate_digest,
                                  ports::PublicationOutcome::not_activated,
                                  "precondition_uninspectable");
                }
            } catch (const std::bad_alloc&) {
                throw;
            } catch (...) {
                return result(request, candidate_digest,
                              ports::PublicationOutcome::not_activated,
                              "precondition_uninspectable");
            }
            if (parent->observe_path_binding() !=
                ports::PathBindingState::same_object) {
                return result(request, candidate_digest,
                              ports::PublicationOutcome::not_activated,
                              "precondition_parent_changed");
            }
        } else {
            try {
                auto prior = exact_document(*parent, target_leaf,
                                            request.maximum_bytes);
                prior_digest = prior.sha256;
                if (prior.sha256 != *request.expected_prior_sha256) {
                    return result(request, candidate_digest,
                                  ports::PublicationOutcome::not_activated,
                                  "precondition_digest_mismatch",
                                  prior_digest, prior_digest);
                }
            } catch (const std::bad_alloc&) {
                throw;
            } catch (...) {
                return result(request, candidate_digest,
                              ports::PublicationOutcome::not_activated,
                              "precondition_proof_failed");
            }
        }

        StageState stage;
        stage.leaf = ".arena-state-stage-" + std::string{random_stage_id} +
                     ".tmp";
        stage.path = parent_path / ascii_leaf(stage.leaf);
        std::vector<ports::PublicationDiagnostic> cleanup;
        const auto finish = [&](ports::PublicationResult value) {
            cleanup_stage(*parent, stage, cleanup);
            value.cleanup_diagnostics = std::move(cleanup);
            return value;
        };

        const auto stage_api_path = filesystem_api_path(stage.path);
        if (control_.stage_hook != nullptr) {
            control_.stage_hook(win::PublicationStageTestMoment::before_create,
                                stage.path, control_.stage_hook_context);
        }
        UniqueHandle stage_handle{CreateFileW(
            stage_api_path.c_str(), GENERIC_WRITE | READ_CONTROL,
            FILE_SHARE_READ,
            private_security.attributes(), CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY |
                FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)};
        if (!stage_handle.valid()) {
            return finish(result(request, candidate_digest,
                                 ports::PublicationOutcome::not_activated,
                                 GetLastError() == ERROR_FILE_EXISTS
                                     ? "stage_collision"
                                     : "stage_create_failed",
                                 prior_digest));
        }
        stage.created = true;
        if (!native_regular_file(stage_handle.get()) ||
            authority_state(stage_handle.get()) != AuthorityState::accepted ||
            !trusted_owner(stage_handle.get())) {
            stage_handle.reset();
            return finish(result(request, candidate_digest,
                                 ports::PublicationOutcome::not_activated,
                                 "stage_object_rejected", prior_digest));
        }
        try {
            auto admitted_stage = parent->open_regular_file(stage.leaf);
            if (admitted_stage->observe_path_binding() !=
                ports::PathBindingState::same_object) {
                stage_handle.reset();
                return finish(result(request, candidate_digest,
                                     ports::PublicationOutcome::not_activated,
                                     "stage_binding_failed", prior_digest));
            }
            stage.object_identity =
                admitted_stage->initial_observation().object_identity;
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            stage_handle.reset();
            return finish(result(request, candidate_digest,
                                 ports::PublicationOutcome::not_activated,
                                 "stage_identity_failed", prior_digest));
        }
        if (!write_stage(stage_handle.get(), request.bytes, deadline)) {
            stage_handle.reset();
            return finish(result(request, candidate_digest,
                                 ports::PublicationOutcome::not_activated,
                                 "stage_write_failed", prior_digest));
        }
        if (control_.fail_stage_flush ||
            FlushFileBuffers(stage_handle.get()) == FALSE) {
            stage_handle.reset();
            return finish(result(request, candidate_digest,
                                 ports::PublicationOutcome::not_activated,
                                 "stage_flush_failed", prior_digest));
        }
        stage_handle.reset();
        if (control_.stage_hook != nullptr) {
            control_.stage_hook(win::PublicationStageTestMoment::after_close,
                                stage.path, control_.stage_hook_context);
        }

        try {
            auto staged = exact_document(*parent, stage.leaf,
                                         request.maximum_bytes);
            if (!stage.object_identity.has_value() ||
                staged.initial_observation.object_identity !=
                    *stage.object_identity ||
                staged.sha256 != candidate_digest ||
                staged.bytes.size() != request.bytes.size()) {
                return finish(result(request, candidate_digest,
                                     ports::PublicationOutcome::not_activated,
                                     "stage_verification_failed", prior_digest));
            }
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            return finish(result(request, candidate_digest,
                                 ports::PublicationOutcome::not_activated,
                                 "stage_verification_failed", prior_digest));
        }
        if (!before_deadline(deadline)) {
            return finish(result(request, candidate_digest,
                                 ports::PublicationOutcome::not_activated,
                                 "stage_deadline", prior_digest));
        }

        DWORD move_flags = MOVEFILE_WRITE_THROUGH;
        if (request.precondition ==
            ports::PublicationPrecondition::exact_sha256) {
            move_flags |= MOVEFILE_REPLACE_EXISTING;
        }
        bool move_succeeded{};
        switch (control_.move_behavior) {
        case win::PublicationMoveTestBehavior::native:
            move_succeeded = MoveFileExW(stage_api_path.c_str(),
                                         filesystem_api_path(request.target).c_str(),
                                         move_flags) != FALSE;
            break;
        case win::PublicationMoveTestBehavior::report_failure_without_move:
            move_succeeded = false;
            break;
        case win::PublicationMoveTestBehavior::move_then_report_failure:
            (void)MoveFileExW(stage_api_path.c_str(),
                              filesystem_api_path(request.target).c_str(),
                              move_flags);
            move_succeeded = false;
            break;
        }

        auto observed = inspect_target(*parent, target_leaf,
                                       request.maximum_bytes);
        if (observed.state == TargetState::digest &&
            observed.digest == candidate_digest) {
            bool target_flushed{};
            try {
                target_flushed = flush_target(request.target);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (...) {
                target_flushed = false;
            }
            auto final_observed = inspect_target(
                *parent, target_leaf, request.maximum_bytes);
            if (final_observed.state != TargetState::digest ||
                final_observed.digest != candidate_digest) {
                return finish(result(request, candidate_digest,
                                     ports::PublicationOutcome::indeterminate,
                                     "target_changed_after_activation",
                                     prior_digest, final_observed.digest));
            }
            if (move_succeeded && target_flushed &&
                before_deadline(deadline)) {
                return finish(result(request, candidate_digest,
                                     ports::PublicationOutcome::activated, {},
                                     prior_digest,
                                     std::string{candidate_digest}));
            }
            return finish(result(
                request, candidate_digest,
                ports::PublicationOutcome::activated_durability_unproven,
                "activation_durability_unproven", prior_digest,
                std::string{candidate_digest}));
        }

        const bool prior_visible =
            request.precondition == ports::PublicationPrecondition::exact_sha256 &&
            observed.state == TargetState::digest && prior_digest.has_value() &&
            observed.digest == prior_digest;
        const bool absence_visible =
            request.precondition == ports::PublicationPrecondition::absent &&
            observed.state == TargetState::absent;
        if (prior_visible || absence_visible) {
            return finish(result(request, candidate_digest,
                                 ports::PublicationOutcome::not_activated,
                                 "move_not_activated", prior_digest,
                                 observed.digest));
        }
        return finish(result(request, candidate_digest,
                             ports::PublicationOutcome::indeterminate,
                             "target_state_indeterminate", prior_digest,
                             observed.digest));
    }

    ports::FilesystemIdentity& identity_;
    win::DurableStatePublisherTestControl control_;
};

} // namespace

std::unique_ptr<arena::ports::DurableStatePublisher>
make_durable_state_publisher(arena::ports::FilesystemIdentity& identity) {
    return std::make_unique<WindowsDurableStatePublisher>(identity);
}

namespace windows_detail {

std::unique_ptr<arena::ports::DurableStatePublisher>
make_durable_state_publisher_for_testing(
    arena::ports::FilesystemIdentity& identity,
    DurableStatePublisherTestControl control) {
    return std::make_unique<WindowsDurableStatePublisher>(identity, control);
}

} // namespace windows_detail

} // namespace arena::platform
