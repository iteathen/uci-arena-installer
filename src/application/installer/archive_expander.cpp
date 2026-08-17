#include "arena/application/installer/archive_expander.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arena::application::installer {
namespace {

constexpr std::size_t kMaximumPathUtf8Bytes = 131072U;
constexpr std::size_t kMaximumLeafUtf8Bytes = 4096U;
constexpr std::size_t kMaximumDepth = 1024U;
constexpr std::uint64_t kMaximumHeaders = UINT64_C(1048576);
constexpr std::uint64_t kMaximumPathMetadataBytes = UINT64_C(67108864);
constexpr std::uint64_t kExpansionRatio = UINT64_C(16384);
constexpr std::size_t kIoChunkBytes = 65536U;
constexpr auto kMinimumTimeout = std::chrono::seconds{1};
constexpr auto kMaximumTimeout = std::chrono::hours{168};

class CancellationFailure final : public std::runtime_error {
  public:
    CancellationFailure() : std::runtime_error("archive expansion was cancelled") {}
};

class DeadlineFailure final : public std::runtime_error {
  public:
    DeadlineFailure() : std::runtime_error("archive expansion deadline expired") {}
};

[[noreturn]] void throw_archive_error(const char* message) {
    throw std::runtime_error(message);
}

void check_terminal(
    const std::stop_token stop_token,
    const std::chrono::steady_clock::time_point deadline) {
    if (stop_token.stop_requested()) {
        throw CancellationFailure{};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        throw DeadlineFailure{};
    }
}

[[nodiscard]] std::chrono::milliseconds remaining_timeout(
    const std::stop_token stop_token,
    const std::chrono::steady_clock::time_point deadline) {
    check_terminal(stop_token, deadline);
    const auto remaining = deadline - std::chrono::steady_clock::now();
    auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining);
    if (milliseconds <= std::chrono::milliseconds::zero()) {
        throw DeadlineFailure{};
    }
    return milliseconds;
}

[[nodiscard]] bool valid_utf8(const std::string_view text) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = data[index];
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1U >= text.size() ||
                (data[index + 1U] & 0xc0U) != 0x80U) {
                return false;
            }
            index += 2U;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2U >= text.size()) {
                return false;
            }
            const auto second = data[index + 1U];
            if ((data[index + 2U] & 0xc0U) != 0x80U) {
                return false;
            }
            if (first == 0xe0U) {
                if (second < 0xa0U || second > 0xbfU) {
                    return false;
                }
            } else if (first == 0xedU) {
                if (second < 0x80U || second > 0x9fU) {
                    return false;
                }
            } else if ((second & 0xc0U) != 0x80U) {
                return false;
            }
            index += 3U;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3U >= text.size()) {
                return false;
            }
            const auto second = data[index + 1U];
            if ((data[index + 2U] & 0xc0U) != 0x80U ||
                (data[index + 3U] & 0xc0U) != 0x80U) {
                return false;
            }
            if (first == 0xf0U) {
                if (second < 0x90U || second > 0xbfU) {
                    return false;
                }
            } else if (first == 0xf4U) {
                if (second < 0x80U || second > 0x8fU) {
                    return false;
                }
            } else if ((second & 0xc0U) != 0x80U) {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool has_control(const std::string_view text) noexcept {
    return std::ranges::any_of(text, [](const unsigned char value) {
        return value < 0x20U || value == 0x7fU;
    });
}

struct PortablePath final {
    std::string value;
    std::vector<std::string_view> components;
};

[[nodiscard]] PortablePath parse_portable_path(const std::string_view path) {
    if (path.empty() || path.size() > kMaximumPathUtf8Bytes ||
        path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find("//") != std::string_view::npos ||
        has_control(path) || !valid_utf8(path)) {
        throw_archive_error("archive entry path is invalid");
    }

    PortablePath result;
    result.value.assign(path);
    std::size_t begin = 0;
    while (begin < result.value.size()) {
        const auto slash = result.value.find('/', begin);
        const auto end = slash == std::string::npos
                             ? result.value.size()
                             : slash;
        const std::string_view component{
            result.value.data() + begin, end - begin};
        if (component.empty() || component == "." || component == ".." ||
            component.size() > kMaximumLeafUtf8Bytes) {
            throw_archive_error("archive entry path component is invalid");
        }
        if (result.components.empty() && component.size() >= 2U &&
            ((component[0] >= 'A' && component[0] <= 'Z') ||
             (component[0] >= 'a' && component[0] <= 'z')) &&
            component[1] == ':') {
            throw_archive_error("archive entry path is drive-rooted");
        }
        result.components.push_back(component);
        if (result.components.size() > kMaximumDepth) {
            throw_archive_error("archive entry path exceeds depth limit");
        }
        if (slash == std::string::npos) {
            break;
        }
        begin = slash + 1U;
    }
    return result;
}

[[nodiscard]] bool ratio_allows(
    const std::uint64_t archive_size,
    const std::uint64_t expected_expanded) noexcept {
    if (archive_size == 0) {
        return false;
    }
    if (archive_size > std::numeric_limits<std::uint64_t>::max() /
                           kExpansionRatio) {
        return true;
    }
    return expected_expanded <= archive_size * kExpansionRatio;
}

struct InputContext final {
    ports::AdmittedRegularFile* file{};
    std::stop_token stop_token;
    std::chrono::steady_clock::time_point deadline{};
    std::uint64_t size{};
    std::uint64_t offset{};
    std::array<std::byte, kIoChunkBytes> buffer{};
    std::exception_ptr failure{};
};

int archive_open_callback(struct archive* archive, void* client_data) noexcept {
    auto& context = *static_cast<InputContext*>(client_data);
    try {
        check_terminal(context.stop_token, context.deadline);
        return ARCHIVE_OK;
    } catch (...) {
        context.failure = std::current_exception();
        archive_set_error(archive, ECANCELED, "archive capability open failed");
        return ARCHIVE_FATAL;
    }
}

la_ssize_t archive_read_callback(
    struct archive* archive,
    void* client_data,
    const void** output) noexcept {
    auto& context = *static_cast<InputContext*>(client_data);
    try {
        check_terminal(context.stop_token, context.deadline);
        if (context.offset >= context.size) {
            *output = context.buffer.data();
            return 0;
        }
        const auto remaining = context.size - context.offset;
        const auto request = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, context.buffer.size()));
        const auto read = context.file->read_at(
            context.offset,
            std::span<std::byte>{context.buffer.data(), request});
        if (read > request) {
            throw_archive_error("archive capability returned an invalid read count");
        }
        if (read > std::numeric_limits<std::uint64_t>::max() - context.offset) {
            throw_archive_error("archive input offset overflowed");
        }
        context.offset += static_cast<std::uint64_t>(read);
        *output = context.buffer.data();
        check_terminal(context.stop_token, context.deadline);
        return static_cast<la_ssize_t>(read);
    } catch (...) {
        context.failure = std::current_exception();
        archive_set_error(archive, EIO, "archive capability read failed");
        *output = nullptr;
        return static_cast<la_ssize_t>(-1);
    }
}

int archive_close_callback(struct archive*, void*) noexcept {
    return ARCHIVE_OK;
}

class ArchiveReader final {
  public:
    ArchiveReader() : reader_(archive_read_new()) {
        if (reader_ == nullptr) {
            throw_archive_error("archive reader allocation failed");
        }
    }

    ~ArchiveReader() {
        if (reader_ != nullptr) {
            (void)archive_read_close(reader_);
            (void)archive_read_free(reader_);
        }
    }

    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;

    [[nodiscard]] struct archive* get() const noexcept {
        return reader_;
    }

    void close_successfully() {
        if (reader_ == nullptr) {
            return;
        }
        const auto close_status = archive_read_close(reader_);
        const auto free_status = archive_read_free(reader_);
        reader_ = nullptr;
        if (close_status != ARCHIVE_OK || free_status != ARCHIVE_OK) {
            throw_archive_error("archive reader cleanup failed after EOF");
        }
    }

  private:
    struct archive* reader_{};
};

class SessionGuard final {
  public:
    explicit SessionGuard(std::unique_ptr<ports::ArchiveExtractionSession> session)
        : session_(std::move(session)) {}

    ~SessionGuard() {
        if (session_ != nullptr && !finished_) {
            (void)session_->cleanup();
        }
    }

    [[nodiscard]] ports::ArchiveExtractionSession& get() {
        return *session_;
    }

    void mark_finished() noexcept {
        finished_ = true;
    }

  private:
    std::unique_ptr<ports::ArchiveExtractionSession> session_;
    bool finished_{};
};

enum class NodeKind {
    directory,
    file,
};

struct NodeState final {
    NodeKind kind{};
    bool explicit_header{};
};

using CollisionMap = std::map<ports::ArchivePathCollisionKey, NodeState>;

[[nodiscard]] ports::ArchiveTargetPath validate_target_path(
    ports::ArchiveExtractionSession& session,
    const std::string_view portable_path,
    const std::size_t expected_depth) {
    ports::ArchiveTargetPath target;
    try {
        target = session.validate_path(portable_path);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_archive_error("archive target path validation failed");
    }
    if (target.portable_relative_path != portable_path ||
        target.depth != expected_depth) {
        throw_archive_error("archive target path validation was inconsistent");
    }
    return target;
}

void ensure_directory_output(
    ports::ArchiveExtractionSession& session,
    const ports::ArchiveTargetPath& target) {
    try {
        session.ensure_directory(target);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_archive_error("archive directory creation failed");
    }
}

[[nodiscard]] std::unique_ptr<ports::ArchiveOutputFile> create_file_output(
    ports::ArchiveExtractionSession& session,
    const ports::ArchiveTargetPath& target,
    const std::uint64_t expected_size,
    const bool executable) {
    try {
        auto output = session.create_file(target, expected_size, executable);
        if (!output) {
            throw_archive_error("archive extraction platform returned no output file");
        }
        return output;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::runtime_error&) {
        throw;
    } catch (...) {
        throw_archive_error("archive output file creation failed");
    }
}

void write_output(
    ports::ArchiveOutputFile& output,
    const std::span<const std::byte> bytes) {
    try {
        output.write(bytes);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_archive_error("archive output write failed");
    }
}

void finish_output(ports::ArchiveOutputFile& output) {
    try {
        output.finish();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_archive_error("archive output finish failed");
    }
}

void ensure_parent_directories(
    ports::ArchiveExtractionSession& session,
    const PortablePath& path,
    CollisionMap& collisions,
    std::uint64_t& directory_count) {
    std::string parent;
    for (std::size_t index = 0; index + 1U < path.components.size(); ++index) {
        if (!parent.empty()) {
            parent.push_back('/');
        }
        parent.append(path.components[index]);
        const auto target = validate_target_path(session, parent, index + 1U);
        const auto found = collisions.find(target.collision_key);
        if (found != collisions.end()) {
            if (found->second.kind != NodeKind::directory) {
                throw_archive_error("archive path collides with a file");
            }
            continue;
        }
        ensure_directory_output(session, target);
        collisions.emplace(
            target.collision_key,
            NodeState{NodeKind::directory, false});
        if (directory_count == std::numeric_limits<std::uint64_t>::max()) {
            throw_archive_error("archive directory count overflowed");
        }
        ++directory_count;
    }
}

void validate_entry_metadata(struct archive_entry* entry) {
    if (archive_entry_hardlink_is_set(entry) != 0 ||
        archive_entry_symlink_utf8(entry) != nullptr) {
        throw_archive_error("archive link entries are not permitted");
    }
    if (archive_entry_sparse_count(entry) > 0) {
        throw_archive_error("archive sparse entries are not permitted");
    }
    if (archive_entry_is_encrypted(entry) > 0) {
        throw_archive_error("archive encrypted entries are not permitted");
    }
    if (archive_entry_xattr_count(entry) > 0) {
        throw_archive_error("archive extended attributes are not permitted");
    }
    const int acl_types =
        ARCHIVE_ENTRY_ACL_TYPE_POSIX1E | ARCHIVE_ENTRY_ACL_TYPE_NFS4;
    if (archive_entry_acl_count(entry, acl_types) > 0) {
        throw_archive_error("archive ACL metadata is not permitted");
    }
    const auto permissions = static_cast<unsigned int>(archive_entry_perm(entry));
    if ((permissions & 07000U) != 0U) {
        throw_archive_error("archive special permission bits are not permitted");
    }
}

void require_supported_format(struct archive* reader) {
    if (archive_filter_code(reader, 0) != ARCHIVE_FILTER_GZIP ||
        (archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) !=
            ARCHIVE_FORMAT_TAR) {
        throw_archive_error("archive must be gzip-compressed tar");
    }
}

[[nodiscard]] std::uint64_t regular_entry_size(
    struct archive_entry* entry,
    const std::uint64_t remaining_expected) {
    if (archive_entry_size_is_set(entry) == 0) {
        throw_archive_error("archive regular file size is unknown");
    }
    const auto signed_size = archive_entry_size(entry);
    if (signed_size < 0) {
        throw_archive_error("archive regular file size is negative");
    }
    const auto size = static_cast<std::uint64_t>(signed_size);
    if (size > remaining_expected) {
        throw_archive_error("archive regular file exceeds expected expanded bytes");
    }
    return size;
}

void stream_regular_file(
    struct archive* reader,
    InputContext& input,
    ports::ArchiveOutputFile& output,
    const std::uint64_t declared_size,
    const std::stop_token stop_token,
    const std::chrono::steady_clock::time_point deadline) {
    std::array<std::byte, kIoChunkBytes> buffer{};
    std::uint64_t total = 0;
    while (total < declared_size) {
        check_terminal(stop_token, deadline);
        const auto remaining = declared_size - total;
        const auto request = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        const auto read = archive_read_data(reader, buffer.data(), request);
        if (input.failure) {
            std::rethrow_exception(input.failure);
        }
        if (read < 0) {
            throw_archive_error("archive entry data read failed");
        }
        if (read == 0) {
            throw_archive_error("archive entry ended before declared size");
        }
        const auto count = static_cast<std::uint64_t>(read);
        if (count > remaining) {
            throw_archive_error("archive entry exceeded declared size");
        }
        write_output(
            output,
            std::span<const std::byte>{
                buffer.data(), static_cast<std::size_t>(read)});
        total += count;
        check_terminal(stop_token, deadline);
    }

    std::array<std::byte, 1> probe{};
    const auto extra = archive_read_data(reader, probe.data(), probe.size());
    if (input.failure) {
        std::rethrow_exception(input.failure);
    }
    if (extra < 0) {
        throw_archive_error("archive entry data termination failed");
    }
    if (extra != 0) {
        throw_archive_error("archive entry contains more data than declared");
    }
}

} // namespace

ArchiveExpander::ArchiveExpander(
    arena::ports::ArchiveExtractionPlatform& extraction_platform)
    : extraction_platform_(extraction_platform) {}

ExpandedArchive ArchiveExpander::expand(
    arena::ports::AdmittedRegularFile& archive_file,
    const ArchiveExpansionRequest& request) const {
    if (request.expected_expanded_file_bytes == 0) {
        throw_archive_error("archive expected expanded byte count is invalid");
    }
    if (request.timeout < kMinimumTimeout || request.timeout > kMaximumTimeout) {
        throw_archive_error("archive expansion timeout is invalid");
    }

    const auto initial = archive_file.initial_observation();
    if (initial.kind != ports::FilesystemObjectKind::regular_file ||
        initial.size_bytes == 0) {
        throw_archive_error("archive input capability is invalid");
    }
    if (!ratio_allows(initial.size_bytes, request.expected_expanded_file_bytes)) {
        throw_archive_error("archive expansion ratio exceeds configured limit");
    }

    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    check_terminal(request.stop_token, deadline);
    auto session = extraction_platform_.begin(
        request.destination_root,
        request.target_platform,
        remaining_timeout(request.stop_token, deadline));
    SessionGuard session_guard{std::move(session)};
    auto& extraction = session_guard.get();
    if (extraction.root() != request.destination_root) {
        throw_archive_error("archive extraction root is inconsistent");
    }

    InputContext input{
        &archive_file,
        request.stop_token,
        deadline,
        initial.size_bytes,
    };
    ArchiveReader reader;
    if (archive_read_support_filter_gzip(reader.get()) != ARCHIVE_OK ||
        archive_read_support_format_tar(reader.get()) != ARCHIVE_OK) {
        throw_archive_error("archive reader format support is unavailable");
    }
    const auto open_status = archive_read_open(
        reader.get(),
        &input,
        archive_open_callback,
        archive_read_callback,
        archive_close_callback);
    if (input.failure) {
        std::rethrow_exception(input.failure);
    }
    if (open_status != ARCHIVE_OK) {
        throw_archive_error("archive reader open failed");
    }

    CollisionMap collisions;
    std::uint64_t header_count = 0;
    std::uint64_t pathname_bytes = 0;
    std::uint64_t file_count = 0;
    std::uint64_t directory_count = 1;
    std::uint64_t total_file_bytes = 0;
    bool root_header_seen = false;

    for (;;) {
        check_terminal(request.stop_token, deadline);
        struct archive_entry* entry = nullptr;
        const auto status = archive_read_next_header(reader.get(), &entry);
        if (input.failure) {
            std::rethrow_exception(input.failure);
        }
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status != ARCHIVE_OK || entry == nullptr) {
            throw_archive_error("archive header parsing failed");
        }
        require_supported_format(reader.get());

        if (header_count >= kMaximumHeaders) {
            throw_archive_error("archive header count exceeds configured limit");
        }
        ++header_count;

        validate_entry_metadata(entry);
        const auto filetype = archive_entry_filetype(entry);
        if (filetype != AE_IFREG && filetype != AE_IFDIR) {
            throw_archive_error("archive entry type is not permitted");
        }

        const char* pathname_text = archive_entry_pathname_utf8(entry);
        if (pathname_text == nullptr) {
            throw_archive_error("archive entry pathname is not valid UTF-8");
        }
        const std::string_view pathname{pathname_text};
        if (pathname.size() >
            kMaximumPathMetadataBytes - pathname_bytes) {
            throw_archive_error("archive pathname metadata exceeds configured limit");
        }
        pathname_bytes += static_cast<std::uint64_t>(pathname.size());

        if (pathname == ".") {
            if (filetype != AE_IFDIR || archive_entry_size(entry) != 0 ||
                root_header_seen) {
                throw_archive_error("archive root directory header is invalid");
            }
            root_header_seen = true;
            continue;
        }

        auto path = parse_portable_path(pathname);
        ensure_parent_directories(
            extraction, path, collisions, directory_count);
        const auto target = validate_target_path(
            extraction, path.value, path.components.size());

        if (filetype == AE_IFDIR) {
            if (archive_entry_size(entry) != 0) {
                throw_archive_error("archive directory entry has nonzero size");
            }
            const auto found = collisions.find(target.collision_key);
            if (found != collisions.end()) {
                if (found->second.kind != NodeKind::directory ||
                    found->second.explicit_header) {
                    throw_archive_error("archive directory header collides with another entry");
                }
                found->second.explicit_header = true;
                continue;
            }
            ensure_directory_output(extraction, target);
            collisions.emplace(
                target.collision_key,
                NodeState{NodeKind::directory, true});
            if (directory_count == std::numeric_limits<std::uint64_t>::max()) {
                throw_archive_error("archive directory count overflowed");
            }
            ++directory_count;
            continue;
        }

        if (collisions.contains(target.collision_key)) {
            throw_archive_error("archive file header collides with another entry");
        }
        const auto remaining_expected =
            request.expected_expanded_file_bytes - total_file_bytes;
        const auto declared_size = regular_entry_size(entry, remaining_expected);
        const auto executable =
            (static_cast<unsigned int>(archive_entry_perm(entry)) & 0111U) != 0U;
        collisions.emplace(
            target.collision_key,
            NodeState{NodeKind::file, true});
        auto output = create_file_output(
            extraction, target, declared_size, executable);
        stream_regular_file(
            reader.get(),
            input,
            *output,
            declared_size,
            request.stop_token,
            deadline);
        finish_output(*output);
        if (file_count == std::numeric_limits<std::uint64_t>::max()) {
            throw_archive_error("archive file count overflowed");
        }
        ++file_count;
        total_file_bytes += declared_size;
    }

    if (archive_read_has_encrypted_entries(reader.get()) > 0) {
        throw_archive_error("archive contains encrypted entries");
    }
    if (total_file_bytes != request.expected_expanded_file_bytes) {
        throw_archive_error("archive expanded byte total does not match expected value");
    }
    reader.close_successfully();

    const auto final = archive_file.observe_open_object();
    if (final != initial ||
        archive_file.observe_path_binding() != ports::PathBindingState::same_object) {
        throw_archive_error("archive input changed during expansion");
    }
    check_terminal(request.stop_token, deadline);
    try {
        extraction.finish();
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        throw_archive_error("archive extraction session finish failed");
    }
    session_guard.mark_finished();

    return ExpandedArchive{
        request.destination_root,
        file_count,
        directory_count,
        total_file_bytes,
    };
}

namespace archive_expander_test_seam {

bool portable_path_is_valid(const std::string_view value) noexcept {
    try {
        (void)parse_portable_path(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool expansion_ratio_allows(
    const std::uint64_t archive_size,
    const std::uint64_t expanded_size) noexcept {
    return ratio_allows(archive_size, expanded_size);
}

std::size_t maximum_path_bytes() noexcept {
    return kMaximumPathUtf8Bytes;
}

std::size_t maximum_leaf_bytes() noexcept {
    return kMaximumLeafUtf8Bytes;
}

std::size_t maximum_depth() noexcept {
    return kMaximumDepth;
}

std::uint64_t maximum_headers() noexcept {
    return kMaximumHeaders;
}

std::uint64_t maximum_path_metadata_bytes() noexcept {
    return kMaximumPathMetadataBytes;
}

} // namespace archive_expander_test_seam

} // namespace arena::application::installer
