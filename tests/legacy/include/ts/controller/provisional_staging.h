#pragma once
// Historical M7 unit fixture only. Not linked into production applications.

#include <array>
#include <map>

#include "ts/domain/config.h"
#include "ts/protocol/messages.h"

namespace ts::controller {
enum class StagingAdmission { Accepted, ProtocolViolation, InvariantFailure };
struct StagingUsage {
    std::size_t reserved_bytes, reserved_chunks, staged_bytes, staged_chunks;
};
// M7 ONLY: reserved volatile memory, never file storage or durability evidence.
// There is deliberately no replenish, barrier, snapshot-read or filesystem API.
class ProvisionalStaging final {
public:
    explicit ProvisionalStaging(domain::QueueConfig limits);
    [[nodiscard]] common::Result<void> reserve(const domain::SessionId& session);
    [[nodiscard]] StagingAdmission append(const protocol::LogChunk& chunk, protocol::LogContext& attempt);
    // The caller has invalidated this session. Discarded volatile output is
    // incomplete; this operation can never authorize a Task terminal result.
    [[nodiscard]] bool reclaim(const domain::SessionId& session);
    [[nodiscard]] StagingUsage usage() const;
    [[nodiscard]] std::size_t byte_window() const noexcept { return limits_.initial_session_log_window_bytes; }
    [[nodiscard]] std::size_t chunk_window() const noexcept { return limits_.initial_session_log_window_chunks; }
private:
    friend class StagingTestPeer;
    struct Chunk {
        std::array<char, 36> attempt{};
        protocol::Stream stream{protocol::Stream::Stdout};
        std::uint64_t sequence{0};
        std::size_t offset{0}, size{0};
    };
    struct Buffer {
        std::vector<char> raw;
        std::vector<Chunk> chunks;
        std::size_t bytes{0}, slots{0};
    };
    domain::QueueConfig limits_;
    std::map<domain::SessionId, Buffer> sessions_;
    std::size_t held_bytes_{0}, held_slots_{0};
};
} // namespace ts::controller
