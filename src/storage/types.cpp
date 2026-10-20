#include "ts/storage/types.h"

#include <limits>
#include <type_traits>

namespace ts::storage {
namespace {
class Bytes {
public:
    void add(std::size_t value) {
        const auto max = std::numeric_limits<std::size_t>::max();
        size = value > max - size ? max : size + value;
    }
    void text(const std::string& s) { add(s.capacity()); add(1); }
    void text(const std::optional<std::string>& s) { if (s) text(*s); }
    void spec(const TaskSpec& s) {
        text(s.task_id); text(s.executable); text(s.cwd);
        add(s.args.capacity() * sizeof(std::string));
        for (const auto& a : s.args) text(a);
        for (const auto& e : s.env) {
            add(sizeof(e) + 4 * sizeof(void*)); text(e.first); text(e.second);
        }
    }
    void task(const TaskRow& t) {
        spec(t.spec); text(t.cancel_requested_at); text(t.latest_attempt_id);
        text(t.created_at); text(t.updated_at);
    }
    void attempt(const AttemptRow& a) {
        text(a.attempt_id); text(a.task_id); text(a.worker_id); text(a.session_id);
        text(a.assigned_at); text(a.acknowledged_at); text(a.started_at); text(a.finished_at);
        text(a.reason); text(a.observed_outcome_json); text(a.stdout_path); text(a.stderr_path);
    }
    void correlation(const Correlation& c) { text(c.task_id); text(c.attempt_id); text(c.session_id); }
    void expected(const TaskVersion& e) { text(e.task_id); }
    void expected(const AttemptVersion& e) { expected(e.task); text(e.attempt_id); text(e.session_id); }
    std::size_t size{0};
};
} // namespace

bool operator==(const Correlation& a, const Correlation& b) noexcept {
    return a.operation_id == b.operation_id && a.task_id == b.task_id &&
        a.attempt_id == b.attempt_id && a.session_id == b.session_id &&
        a.expected_task_version == b.expected_task_version &&
        a.expected_attempt_version == b.expected_attempt_version &&
        a.entity_generation == b.entity_generation && a.session_generation == b.session_generation;
}
Correlation correlate(const Command& command, std::uint64_t operation_id,
                       std::uint64_t entity_generation, std::uint64_t session_generation) {
    Correlation result;
    result.operation_id = operation_id;
    result.entity_generation = entity_generation;
    result.session_generation = session_generation;
    std::visit([&](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, Submit>) result.task_id = c.task.task_id;
        else if constexpr (std::is_same_v<T, Assign> || std::is_same_v<T, CancelIntent> ||
                           std::is_same_v<T, CancelPending> || std::is_same_v<T, Retry>) {
            result.task_id = c.expected.task_id; result.expected_task_version = c.expected.version;
            if constexpr (std::is_same_v<T, Assign>) { result.attempt_id = c.attempt_id; result.session_id = c.session_id; }
        } else if constexpr (std::is_same_v<T, Advance> || std::is_same_v<T, Terminalize>) {
            result.task_id = c.expected.task.task_id; result.expected_task_version = c.expected.task.version;
            result.attempt_id = c.expected.attempt_id; result.expected_attempt_version = c.expected.version;
            result.session_id = c.expected.session_id;
        } else if constexpr (std::is_same_v<T, LoseSession>) result.session_id = c.session_id;
        else if constexpr (std::is_same_v<T, ReadTask> || std::is_same_v<T, ReadAttempts>) result.task_id = c.task_id;
    }, command);
    return result;
}
bool is_read(const Command& c) noexcept {
    return std::holds_alternative<ReadTask>(c) || std::holds_alternative<ReadAttempts>(c) ||
           std::holds_alternative<ListTasks>(c);
}
bool is_essential(const Command& c) noexcept {
    return std::holds_alternative<Terminalize>(c) || std::holds_alternative<LoseSession>(c) ||
           std::holds_alternative<Recover>(c);
}
std::size_t owned_bytes(const Envelope& e) {
    Bytes b; b.add(sizeof(e)); b.correlation(e.correlation);
    std::visit([&](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, Submit>) { b.spec(c.task); b.text(c.at); }
        else if constexpr (std::is_same_v<T, Assign>) {
            b.expected(c.expected); b.text(c.attempt_id); b.text(c.worker_id); b.text(c.session_id);
            b.text(c.stdout_path); b.text(c.stderr_path); b.text(c.at);
        } else if constexpr (std::is_same_v<T, Advance> || std::is_same_v<T, CancelIntent> ||
                             std::is_same_v<T, CancelPending> || std::is_same_v<T, Retry>) {
            b.expected(c.expected); b.text(c.at);
        } else if constexpr (std::is_same_v<T, Terminalize>) {
            b.expected(c.expected); b.text(c.at); b.text(c.reason); b.text(c.observed_outcome_json);
            b.text(c.stdout_path); b.text(c.stderr_path); b.text(c.logs.attempt_id);
        } else if constexpr (std::is_same_v<T, LoseSession>) {
            b.text(c.session_id); b.text(c.at); b.text(c.reason);
        } else if constexpr (std::is_same_v<T, Recover>) b.text(c.at);
        else if constexpr (std::is_same_v<T, ReadTask> || std::is_same_v<T, ReadAttempts>) b.text(c.task_id);
        else if constexpr (std::is_same_v<T, ListTasks>) {
            if (c.after) { b.text(c.after->created_at); b.text(c.after->task_id); }
        }
    }, e.command);
    return b.size;
}
std::size_t owned_bytes(const Outcome& o) {
    Bytes b; b.add(sizeof(o));
    b.add(o.tasks.capacity() * sizeof(TaskRow));
    b.add(o.attempts.capacity() * sizeof(AttemptRow));
    for (const auto& t : o.tasks) b.task(t);
    for (const auto& a : o.attempts) b.attempt(a);
    if (o.error) b.text(o.error->message);
    if (o.next) { b.text(o.next->created_at); b.text(o.next->task_id); }
    return b.size;
}
} // namespace ts::storage
