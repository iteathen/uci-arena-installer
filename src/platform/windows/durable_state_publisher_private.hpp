#pragma once

#include "arena/ports/durable_state_publisher.hpp"
#include "arena/ports/filesystem_identity.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>

namespace arena::platform::windows_detail {

enum class PublicationMoveTestBehavior {
    native,
    report_failure_without_move,
    move_then_report_failure,
};

enum class PublicationStageTestMoment {
    before_create,
    after_close,
};

using PublicationStageTestHook = void (*)(
    PublicationStageTestMoment,
    const std::filesystem::path&,
    void*) noexcept;

struct DurableStatePublisherTestControl {
    std::size_t maximum_write_chunk{};
    std::size_t simulated_write_failure_after_bytes{};
    PublicationMoveTestBehavior move_behavior{
        PublicationMoveTestBehavior::native};
    bool fail_stage_flush{};
    bool fail_target_flush{};
    bool fail_post_move_inspection{};
    bool fail_stage_cleanup{};
    PublicationStageTestHook stage_hook{};
    void* stage_hook_context{};
    std::size_t* write_calls{};
    std::size_t* exact_document_proofs{};
};

[[nodiscard]] std::unique_ptr<arena::ports::DurableStatePublisher>
make_durable_state_publisher_for_testing(
    arena::ports::FilesystemIdentity& identity,
    DurableStatePublisherTestControl control);

} // namespace arena::platform::windows_detail
