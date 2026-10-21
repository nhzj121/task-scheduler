#include "ts/scheduler/scheduler.h"

#include <algorithm>
#include <set>
#include <utility>

namespace ts::scheduler {
namespace {
struct Ratio { std::uint64_t numerator, denominator; };
bool less(Ratio a, Ratio b) {
    // Continued-fraction comparison is exact and uses only division/remainder.
    // It never forms a potentially overflowing 64x64 cross product.
    bool reversed = false;
    for (;;) {
        const auto qa = a.numerator / a.denominator;
        const auto qb = b.numerator / b.denominator;
        if (qa != qb) return reversed ? qa > qb : qa < qb;
        const auto ra = a.numerator % a.denominator;
        const auto rb = b.numerator % b.denominator;
        if (ra == 0 || rb == 0) return reversed ? ra != 0 && rb == 0 : ra == 0 && rb != 0;
        a = {a.denominator, ra}; b = {b.denominator, rb}; reversed = !reversed;
    }
}
} // namespace

common::Result<std::optional<domain::WorkerId>> select(
    const domain::ResourceRequest& request, const std::vector<WorkerSnapshot>& workers) {
    using Result = common::Result<std::optional<domain::WorkerId>>;
    std::optional<domain::WorkerId> chosen;
    Ratio best{0, 1};
    std::set<domain::WorkerId> seen;
    for (const auto& worker : workers) {
        if (!seen.insert(worker.worker_id).second ||
            worker.used.cpu_slots() > worker.capacity.cpu_slots() ||
            worker.used.memory_mb() > worker.capacity.memory_mb() ||
            (worker.max_concurrent_tasks && (*worker.max_concurrent_tasks == 0 ||
                                            worker.attempts > *worker.max_concurrent_tasks)))
            return Result::failure("invalid or duplicate Worker resource snapshot");
        if (worker.state != domain::WorkerState::HEALTHY || !worker.current_session || worker.invalidating ||
            (worker.max_concurrent_tasks && worker.attempts == *worker.max_concurrent_tasks) ||
            !domain::fits_within(worker.capacity, worker.used, request)) continue;
        // fits_within uses subtraction before addition, proving these sums safe.
        Ratio dominant{static_cast<std::uint64_t>(worker.used.cpu_slots()) + request.cpu_slots(),
                       worker.capacity.cpu_slots()};
        const Ratio memory = worker.capacity.memory_mb() == 0 ? Ratio{0, 1} :
            Ratio{worker.used.memory_mb() + request.memory_mb(), worker.capacity.memory_mb()};
        if (less(dominant, memory)) dominant = memory;
        if (!chosen || less(dominant, best) || (!less(best, dominant) && worker.worker_id < *chosen)) {
            chosen = worker.worker_id; best = dominant;
        }
    }
    return Result::success(std::move(chosen));
}
std::vector<PendingTask> pending_order(std::vector<PendingTask> tasks) {
    std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) {
        return a.created_at < b.created_at || (a.created_at == b.created_at && a.task_id < b.task_id);
    });
    return tasks;
}
} // namespace ts::scheduler
