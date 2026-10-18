#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>

namespace ts::protocol {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::uint32_t kMagic = 0x54534348;
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint32_t kMaxPayload = 4194304;

enum class Type : std::uint16_t {
    ErrorResponse = 0x0001,
    SubmitTask = 0x0101, SubmitTaskResponse = 0x0102,
    GetTask = 0x0103, GetTaskResponse = 0x0104,
    ListTasks = 0x0105, ListTasksResponse = 0x0106,
    GetLogs = 0x0107, GetLogsResponse = 0x0108,
    CancelTask = 0x0109, CancelTaskResponse = 0x010A,
    RetryTask = 0x010B, RetryTaskResponse = 0x010C,
    ListWorkers = 0x010D, ListWorkersResponse = 0x010E,
    RegisterWorker = 0x0201, RegisterAck = 0x0202, Heartbeat = 0x0203,
    RunTask = 0x0204, RunTaskAck = 0x0205, TaskStarted = 0x0206,
    LogChunk = 0x0207, LogWindowUpdate = 0x0208, TaskFinished = 0x0209,
    WorkerCancelTask = 0x020A, WorkerCancelTaskAck = 0x020B
};
enum class Direction { ClientToController, ControllerToClient, WorkerToController, ControllerToWorker };
enum class ErrorKind { Framing, Unsupported, Syntax, Schema, Consistency };
struct ProtocolError {
    ErrorKind kind;
    std::string message; // Bounded diagnostic, never the untrusted payload.
};
template <typename T> class ProtocolResult {
public:
    static ProtocolResult success(T value) { return ProtocolResult(std::move(value)); }
    static ProtocolResult failure(ErrorKind kind, std::string message) {
        if (message.size() > 256) message.resize(256);
        return ProtocolResult(ProtocolError{kind, std::move(message)});
    }
    explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
    T& value() & { return std::get<T>(value_); }
    const T& value() const& { return std::get<T>(value_); }
    T&& value() && { return std::get<T>(std::move(value_)); }
    const ProtocolError& error() const { return std::get<ProtocolError>(value_); }
private:
    explicit ProtocolResult(T value) : value_(std::move(value)) {}
    explicit ProtocolResult(ProtocolError error) : value_(std::move(error)) {}
    std::variant<T, ProtocolError> value_;
};

struct Header { Type type; std::uint64_t request_id; std::uint32_t payload_length; };
struct Frame { Header header; std::string payload; };
[[nodiscard]] bool known_type(Type type) noexcept;
[[nodiscard]] Direction direction(Type type);
[[nodiscard]] bool is_event(Type type) noexcept;
[[nodiscard]] Type response_type(Type request);
// Explicit byte helpers, independent of host endianness or struct layout.
[[nodiscard]] std::array<std::uint8_t, 8> encode_u64(std::uint64_t value) noexcept;
[[nodiscard]] std::uint64_t decode_u64(const std::uint8_t* bytes) noexcept;
[[nodiscard]] ProtocolResult<std::array<std::uint8_t, kHeaderSize>> encode_header(Header header, Direction expected);
[[nodiscard]] ProtocolResult<Header> decode_header(const std::uint8_t* bytes, Direction expected);
[[nodiscard]] ProtocolResult<std::string> encode_frame(const Frame& frame, Direction expected);

struct FeedStatus { std::size_t frames; bool incomplete; };
class FrameParser {
public:
    using Consumer = std::function<bool(Frame&&)>;
    explicit FrameParser(Direction expected, std::size_t high_water = kHeaderSize + kMaxPayload);
    // Consumer is synchronous; false is fatal. No completed-frame backlog is retained.
    [[nodiscard]] ProtocolResult<FeedStatus> feed(std::string_view bytes, const Consumer& consumer);
    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    [[nodiscard]] std::size_t payload_capacity() const noexcept { return payload_.capacity(); }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
private:
    Direction expected_;
    std::size_t high_water_;
    std::array<std::uint8_t, kHeaderSize> header_bytes_{};
    std::size_t header_used_{0};
    Header header_{Type::ErrorResponse, 0, 0};
    std::string payload_;
    bool failed_{false};
};
} // namespace ts::protocol
