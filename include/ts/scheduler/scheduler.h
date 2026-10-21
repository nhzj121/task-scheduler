#pragma once

#include <optional>
#include <vector>

#include "ts/common/timestamp.h"
#include "ts/domain/ids.h"
#include "ts/domain/resources.h"
#include "ts/domain/states.h"

namespace ts::scheduler {

struct WorkerSnapshot {
    domain::WorkerId worker_id;
    domain::ResourceCapacity capacity;
    domain::ResourceUsage used;
    std::uint32_t attempts{0};
    std::optional<std::uint32_t> max_concurrent_tasks;
    domain::WorkerState state{domain::WorkerState::HEALTHY};
    bool current_session{true};
    bool invalidating{false};
};
struct PendingTask {
    domain::TaskId task_id;
    common::Timestamp created_at;
    domain::ResourceRequest request;
};

// Pure decisions. Used resources include tentative AND committed reservations.
// Invalid/overcommitted snapshots are explicit errors, not wrapped arithmetic.
[[nodiscard]] common::Result<std::optional<domain::WorkerId>> select(
    const domain::ResourceRequest& request, const std::vector<WorkerSnapshot>& workers);
[[nodiscard]] std::vector<PendingTask> pending_order(std::vector<PendingTask> tasks);

} // namespace ts::scheduler
