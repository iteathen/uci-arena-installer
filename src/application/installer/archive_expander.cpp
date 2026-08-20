#include "arena/application/installer/archive_expander.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace arena::application::installer {
namespace {

constexpr std::size_t kBufferBytes = 64 * 1024;
constexpr std::uint64_t kExpansionRatio = 16384;
constexpr std::uint64_t kMaximumHeaders = 1'048'576;
constexpr std::uint64_t kMaximumPathBytes = 64ULL * 1024 * 1024;
constexpr std::size_t kMaximumPath = 131072;
constexpr std::size_t kMaximumLeaf = 4096;
constexpr std::size_t kMaximumDepth = 1024;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void check_interrupt(const std::stop_token token,
                     const std::chrono::steady_clock::time_point deadline) {
    if (token.stop_requested()) fail("archive expansion cancelled");
    if (std::chrono::steady_clock::now() >= deadline)
        fail("archive expansion deadline exceeded");
}

bool valid_utf8(const std::string_view value) {
    std::size_t i = 0;
    while (i < value.size()) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c <= 0x7f) { ++i; continue; }
        std::size_t length = 0;
        std::uint32_t code = 0;
        if ((c & 0xe0) == 0xc0) { length = 2; code = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { length = 3; code = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { length = 4; code = c & 0x07; }
        else return false;
        if (i + length > value.size()) return false;
        for (std::size_t j = 1; j < length; ++j) {
            const auto continuation = static_cast<unsigned char>(value[i + j]);
            if ((continuation & 0xc0) != 0x80) return false;
            code = (code << 6) | (continuation & 0x3f);
        }
        if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
            (length == 4 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) return false;
        i += length;
    }
    return true;
}

std::size_t validate_portable_path(const std::string_view path) {
    if (path.empty() || path.size() > kMaximumPath || path.front() == '/' ||
        path.find('\\') != std::string_view::npos || !valid_utf8(path) ||
        (path.size() >= 2 && path[1] == ':')) fail("invalid archive path");
    std::size_t depth = 0;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        const auto leaf = path.substr(begin, end == std::string_view::npos
                                               ? path.size() - begin
                                               : end - begin);
        if (leaf.empty() || leaf == "." || leaf == ".." ||
            leaf.size() > kMaximumLeaf) fail("invalid archive path component");
        for (const unsigned char c : leaf)
            if (c < 0x20 || c == 0x7f) fail("control character in archive path");
        if (++depth > kMaximumDepth) fail("archive path depth exceeded");
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return depth;
}

struct ReaderState {
    arena::ports::AdmittedRegularFile* file{};
    std::uint64_t offset{};
    std::array<std::byte, kBufferBytes> buffer{};
    std::stop_token stop_token;
    std::chrono::steady_clock::time_point deadline{};
    std::exception_ptr error;
};

la_ssize_t read_callback(archive*, void* client, const void** output) noexcept {
    auto& state = *static_cast<ReaderState*>(client);
    try {
        check_interrupt(state.stop_token, state.deadline);
        const auto count = state.file->read_at(state.offset, state.buffer);
        if (count > state.buffer.size() ||
            state.offset > (std::numeric_limits<std::uint64_t>::max)() - count)
            fail("archive input offset overflow");
        state.offset += count;
        *output = state.buffer.data();
        return static_cast<la_ssize_t>(count);
    } catch (...) {
        state.error = std::current_exception();
        return ARCHIVE_FATAL;
    }
}

int close_callback(archive*, void*) noexcept { return ARCHIVE_OK; }

struct ArchiveOwner {
    archive* value{};
    ~ArchiveOwner() { if (value) archive_read_free(value); }
};

struct CollisionLess {
    bool operator()(const arena::ports::ArchivePathCollisionKey& left,
                    const arena::ports::ArchivePathCollisionKey& right) const {
        return std::memcmp(left.data(), right.data(), left.size()) < 0;
    }
};

enum class NodeKind { directory, file };
struct NodeState {
    NodeKind kind;
    bool explicit_header;
    std::string portable_path;
};

void reject_metadata(archive_entry* entry) {
    const auto mode = archive_entry_mode(entry);
    if ((mode & 07000) != 0)
        fail("special archive permission bits are forbidden");
    if (archive_entry_symlink(entry) || archive_entry_hardlink(entry))
        fail("archive links are forbidden");
    if (archive_entry_sparse_reset(entry) > 0)
        fail("sparse archive entries are forbidden");
    if (archive_entry_xattr_reset(entry) > 0)
        fail("archive extended attributes are forbidden");
    if (archive_entry_acl_reset(entry,
            ARCHIVE_ENTRY_ACL_TYPE_ACCESS | ARCHIVE_ENTRY_ACL_TYPE_DEFAULT) > 0)
        fail("archive ACLs are forbidden");
    if (archive_entry_is_encrypted(entry) == 1)
        fail("encrypted archive entries are forbidden");
}

} // namespace

ArchiveExpander::ArchiveExpander(
    arena::ports::ArchiveExtractionPlatform& extraction_platform)
    : extraction_platform_(extraction_platform) {}

ExpandedArchive ArchiveExpander::expand(
    arena::ports::AdmittedRegularFile& archive_file,
    const ArchiveExpansionRequest& request) const {
    const auto initial = archive_file.initial_observation();
    if (initial.kind != arena::ports::FilesystemObjectKind::regular_file ||
        initial.size_bytes == 0 || request.expected_expanded_file_bytes == 0)
        fail("archive input or expected expanded size is invalid");
    if (archive_file.observe_open_object() != initial)
        fail("archive input is not stable before expansion");
    if (initial.size_bytes <= (std::numeric_limits<std::uint64_t>::max)() /
                                  kExpansionRatio) {
        if (request.expected_expanded_file_bytes >
            initial.size_bytes * kExpansionRatio) fail("archive expansion ratio exceeded");
    }
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    check_interrupt(request.stop_token, deadline);

    auto session = extraction_platform_.begin(
        request.destination_root, request.target_platform, request.timeout);
    try {
        ReaderState reader{&archive_file, 0, {}, request.stop_token, deadline, {}};
        ArchiveOwner input{archive_read_new()};
        if (!input.value) fail("could not allocate archive reader");
        if (archive_read_support_filter_gzip(input.value) != ARCHIVE_OK ||
            archive_read_support_format_tar(input.value) != ARCHIVE_OK)
            fail("could not configure tar+gzip reader");
        if (archive_read_open2(input.value, &reader, nullptr, read_callback,
                               nullptr, close_callback) != ARCHIVE_OK)
            fail("could not open archive stream");

        ExpandedArchive result{session->root(), 0, 1, 0};
        std::uint64_t headers = 0;
        std::uint64_t path_bytes = 0;
        std::map<arena::ports::ArchivePathCollisionKey, NodeState, CollisionLess> nodes;
        archive_entry* entry = nullptr;
        for (;;) {
            check_interrupt(request.stop_token, deadline);
            const int status = archive_read_next_header(input.value, &entry);
            if (reader.error) std::rethrow_exception(reader.error);
            if (status == ARCHIVE_EOF) break;
            if (status != ARCHIVE_OK) fail("archive header parse failed");
            if (++headers > kMaximumHeaders) fail("archive header count exceeded");
            if (archive_filter_code(input.value, 0) != ARCHIVE_FILTER_GZIP ||
                (archive_format(input.value) & ARCHIVE_FORMAT_BASE_MASK) !=
                    ARCHIVE_FORMAT_TAR) fail("archive is not gzip-compressed tar");
            if (archive_read_has_encrypted_entries(input.value) == 1)
                fail("encrypted archives are forbidden");

            const char* raw_path = archive_entry_pathname_utf8(entry);
            if (!raw_path) fail("archive path is not valid UTF-8");
            std::string path{raw_path};
            if (path_bytes > kMaximumPathBytes - path.size())
                fail("archive pathname byte limit exceeded");
            path_bytes += path.size();
            reject_metadata(entry);
            const auto type = archive_entry_filetype(entry);
            const bool is_directory = type == AE_IFDIR;
            if (path == ".") {
                if (!is_directory || archive_entry_size(entry) != 0)
                    fail("invalid archive root entry");
                continue;
            }
            const auto depth = validate_portable_path(path);

            std::size_t slash = 0;
            while ((slash = path.find('/', slash)) != std::string::npos) {
                const auto parent_text = path.substr(0, slash);
                auto parent = session->validate_path(parent_text);
                if (parent.depth != static_cast<std::size_t>(
                                        std::count(parent_text.begin(), parent_text.end(), '/')) + 1)
                    fail("target returned inconsistent archive path depth");
                auto [it, inserted] = nodes.emplace(parent.collision_key,
                    NodeState{NodeKind::directory, false, parent_text});
                if (!inserted && (it->second.kind != NodeKind::directory ||
                                  it->second.portable_path != parent_text))
                    fail("archive target path collision");
                if (inserted) {
                    session->ensure_directory(parent);
                    if (result.directory_count ==
                        (std::numeric_limits<std::uint64_t>::max)())
                        fail("archive directory count overflow");
                    ++result.directory_count;
                }
                ++slash;
            }

            auto target = session->validate_path(path);
            if (target.depth != depth) fail("target returned inconsistent archive path depth");
            if (is_directory) {
                if (archive_entry_size(entry) != 0) fail("archive directory has data size");
                auto [it, inserted] = nodes.emplace(target.collision_key,
                    NodeState{NodeKind::directory, true, path});
                if (!inserted) {
                    if (it->second.kind != NodeKind::directory ||
                        it->second.portable_path != path ||
                        it->second.explicit_header)
                        fail("duplicate or conflicting archive path");
                    it->second.explicit_header = true;
                } else {
                    session->ensure_directory(target);
                    if (result.directory_count ==
                        (std::numeric_limits<std::uint64_t>::max)())
                        fail("archive directory count overflow");
                    ++result.directory_count;
                }
                continue;
            }
            if (type != AE_IFREG) fail("unsupported archive entry type");
            const auto signed_size = archive_entry_size(entry);
            if (signed_size < 0) fail("archive file size is unknown");
            const auto size = static_cast<std::uint64_t>(signed_size);
            if (size > request.expected_expanded_file_bytes - result.total_file_bytes)
                fail("archive expanded byte count exceeded");
            if (!nodes.emplace(target.collision_key,
                    NodeState{NodeKind::file, true, path}).second)
                fail("duplicate or conflicting archive path");
            auto output = session->create_file(
                target, size, (archive_entry_mode(entry) & 0111) != 0);
            std::array<std::byte, kBufferBytes> data{};
            std::uint64_t written = 0;
            while (written < size) {
                check_interrupt(request.stop_token, deadline);
                const auto wanted = static_cast<std::size_t>(
                    std::min<std::uint64_t>(data.size(), size - written));
                const auto count = archive_read_data(input.value, data.data(), wanted);
                if (reader.error) std::rethrow_exception(reader.error);
                if (count <= 0 || static_cast<std::uint64_t>(count) > size - written)
                    fail("archive file data is shorter than declared");
                output->write(std::span<const std::byte>{data.data(),
                    static_cast<std::size_t>(count)});
                written += static_cast<std::uint64_t>(count);
                check_interrupt(request.stop_token, deadline);
            }
            std::byte extra{};
            const auto trailing = archive_read_data(input.value, &extra, 1);
            if (reader.error) std::rethrow_exception(reader.error);
            if (trailing != 0) fail("archive file data exceeds declared size");
            output->finish();
            ++result.file_count;
            result.total_file_bytes += size;
        }
        if (result.total_file_bytes != request.expected_expanded_file_bytes)
            fail("archive expanded byte total does not match expectation");
        if (archive_read_close(input.value) != ARCHIVE_OK)
            fail("archive close failed");
        auto* closed = input.value;
        input.value = nullptr;
        if (archive_read_free(closed) != ARCHIVE_OK)
            fail("archive release failed");
        const auto final = archive_file.observe_open_object();
        if (final != initial || archive_file.observe_path_binding() !=
                                    arena::ports::PathBindingState::same_object)
            fail("archive input identity changed during expansion");
        session->finish();
        return result;
    } catch (...) {
        const auto primary = std::current_exception();
        (void)session->cleanup();
        std::rethrow_exception(primary);
    }
}

} // namespace arena::application::installer
