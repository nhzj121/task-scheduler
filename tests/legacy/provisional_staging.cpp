#include "ts/controller/provisional_staging.h"
// Preserves the accepted M7 staging unit cases, never the M8 production data path.

#include <algorithm>
#include <stdexcept>

namespace ts::controller {
ProvisionalStaging::ProvisionalStaging(domain::QueueConfig limits) : limits_(limits) {
    if (!limits.log_bytes || !limits.log_control_reserve || limits.log_control_reserve >= limits.log_commands ||
        !limits.initial_session_log_window_bytes || limits.initial_session_log_window_bytes > limits.log_bytes ||
        !limits.initial_session_log_window_chunks ||
        limits.initial_session_log_window_chunks > limits.log_commands - limits.log_control_reserve)
        throw std::invalid_argument("invalid provisional staging capacity");
}
common::Result<void> ProvisionalStaging::reserve(const domain::SessionId& session) {
    if (sessions_.count(session)) return common::Result<void>::failure("duplicate staging reservation");
    const auto bytes = byte_window(), chunks = chunk_window();
    if (bytes > limits_.log_bytes - held_bytes_ ||
        chunks > limits_.log_commands - limits_.log_control_reserve - held_slots_)
        return common::Result<void>::failure("initial credit cannot be backed by global staging capacity");
    try {
        Buffer buffer; buffer.raw.resize(bytes); buffer.chunks.resize(chunks);
        sessions_.emplace(session, std::move(buffer));
    } catch (const std::exception&) { return common::Result<void>::failure("cannot allocate reserved provisional staging"); }
    held_bytes_ += bytes; held_slots_ += chunks;
    return common::Result<void>::success();
}
StagingAdmission ProvisionalStaging::append(const protocol::LogChunk& chunk, protocol::LogContext& attempt) {
    const auto it = sessions_.find(chunk.session_id);
    if (it == sessions_.end()) return StagingAdmission::ProtocolViolation;
    auto& buffer = it->second;
    auto context = attempt;
    context.credit_bytes = byte_window() - buffer.bytes;
    context.credit_chunks = chunk_window() - buffer.slots;
    auto checked = protocol::check_log(chunk, context);
    if (!checked) return StagingAdmission::ProtocolViolation;
    // A valid credited chunk already owns both dimensions. Failing this branch
    // is a program invariant failure, NOT ordinary admission pressure.
    if (buffer.bytes > buffer.raw.size() || chunk.raw_size > buffer.raw.size() - buffer.bytes ||
        buffer.slots >= buffer.chunks.size()) return StagingAdmission::InvariantFailure;
    const auto raw = protocol::base64_decode(chunk.data_base64, 65536);
    if (!raw || raw.value().size() != chunk.raw_size) return StagingAdmission::ProtocolViolation;
    std::copy(raw.value().begin(), raw.value().end(), buffer.raw.data() + buffer.bytes);
    auto& record = buffer.chunks[buffer.slots];
    std::copy(chunk.attempt_id.value().begin(), chunk.attempt_id.value().end(), record.attempt.begin());
    record.stream = chunk.stream; record.sequence = chunk.sequence; record.offset = buffer.bytes; record.size = chunk.raw_size;
    buffer.bytes += chunk.raw_size; ++buffer.slots; attempt = checked.value();
    return StagingAdmission::Accepted;
}
bool ProvisionalStaging::reclaim(const domain::SessionId& session) {
    const auto it = sessions_.find(session); if (it == sessions_.end()) return false;
    held_bytes_ -= byte_window(); held_slots_ -= chunk_window(); sessions_.erase(it); return true;
}
StagingUsage ProvisionalStaging::usage() const {
    StagingUsage result{held_bytes_, held_slots_, 0, 0};
    for (const auto& item : sessions_) { result.staged_bytes += item.second.bytes; result.staged_chunks += item.second.slots; }
    return result;
}
} // namespace ts::controller
