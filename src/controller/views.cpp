#include "ts/controller/views.h"

namespace ts::controller {
namespace {
std::optional<common::Timestamp> at(const std::optional<std::string>& value) {
    return value ? std::optional<common::Timestamp>(common::Timestamp::parse(*value).value()) : std::nullopt;
}
}
protocol::Task task_view(const storage::TaskRow& row) {
    const auto& s = row.spec;
    return {domain::TaskId::parse(s.task_id).value(), row.state, s.executable, s.args, s.env, s.cwd,
        domain::ResourceRequest::create(s.cpu_slots, s.memory_mb).value(), s.timeout_ms, at(row.cancel_requested_at),
        common::Timestamp::parse(row.created_at).value(), common::Timestamp::parse(row.updated_at).value(),
        row.latest_attempt_id ? std::optional<domain::AttemptId>(domain::AttemptId::parse(*row.latest_attempt_id).value()) : std::nullopt};
}
protocol::Attempt attempt_view(const storage::AttemptRow& row, bool finalizing) {
    return {domain::AttemptId::parse(row.attempt_id).value(), domain::TaskId::parse(row.task_id).value(),
        domain::AttemptNumber::create(row.attempt_no).value(), row.state, domain::WorkerId::parse(row.worker_id).value(),
        domain::SessionId::parse(row.session_id).value(), at(row.assigned_at), at(row.started_at), at(row.finished_at),
        row.exit_code, row.term_signal, row.reason, row.stdout_bytes, row.stderr_bytes, row.logs_complete,
        finalizing && !domain::is_terminal(row.state)};
}
std::string page_token(const storage::TaskCursor& cursor) {
    return protocol::base64_encode(nlohmann::json::array({cursor.created_at, cursor.task_id}).dump());
}
common::Result<storage::TaskCursor> parse_page_token(const std::string& token) {
    using R = common::Result<storage::TaskCursor>;
    if (token.empty() || token.size() > 256) return R::failure("invalid page token size");
    const auto decoded = protocol::base64_decode(token, 128);
    if (!decoded) return R::failure("invalid page token encoding");
    try {
        const auto value = nlohmann::json::parse(decoded.value());
        if (!value.is_array() || value.size() != 2 || !value[0].is_string() || !value[1].is_string())
            return R::failure("invalid page token tuple");
        storage::TaskCursor cursor{value[0].get<std::string>(), value[1].get<std::string>()};
        if (!common::Timestamp::parse(cursor.created_at) || !domain::TaskId::parse(cursor.task_id))
            return R::failure("invalid page token cursor");
        return R::success(std::move(cursor));
    } catch (const std::exception&) { return R::failure("invalid page token JSON"); }
}
} // namespace ts::controller
