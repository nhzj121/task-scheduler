#include "ts/protocol/frame.h"

#include <algorithm>
#include <stdexcept>

namespace ts::protocol {
namespace {
void put(std::uint8_t* out, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        out[width - 1 - i] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}
std::uint64_t get(const std::uint8_t* in, std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) value = (value << 8U) | in[i];
    return value;
}
ProtocolResult<Header> validate(Header header, Direction expected) {
    if (!known_type(header.type)) return ProtocolResult<Header>::failure(ErrorKind::Unsupported, "reserved message type");
    if (direction(header.type) != expected) return ProtocolResult<Header>::failure(ErrorKind::Unsupported, "wrong message direction");
    if ((is_event(header.type) && header.request_id != 0) ||
        (!is_event(header.type) && header.type != Type::ErrorResponse && header.request_id == 0))
        return ProtocolResult<Header>::failure(ErrorKind::Schema, "invalid request_id");
    if (header.payload_length == 0 || header.payload_length > kMaxPayload)
        return ProtocolResult<Header>::failure(ErrorKind::Framing, "invalid payload length");
    return ProtocolResult<Header>::success(header);
}
} // namespace

bool known_type(Type type) noexcept {
    const auto id = static_cast<std::uint16_t>(type);
    return id == 1 || (id >= 0x101 && id <= 0x10e) || (id >= 0x201 && id <= 0x20b);
}
Direction direction(Type type) {
    if (!known_type(type)) throw std::invalid_argument("reserved message type");
    const auto id = static_cast<std::uint16_t>(type);
    if (id == 1 || (id >= 0x101 && id <= 0x10e && id % 2 == 0)) return Direction::ControllerToClient;
    if (id <= 0x10e) return Direction::ClientToController;
    switch (type) {
    case Type::RegisterAck: case Type::RunTask: case Type::LogWindowUpdate: case Type::WorkerCancelTask:
        return Direction::ControllerToWorker;
    default: return Direction::WorkerToController;
    }
}
bool is_event(Type type) noexcept {
    return type == Type::Heartbeat || type == Type::TaskStarted || type == Type::LogChunk ||
           type == Type::LogWindowUpdate || type == Type::TaskFinished;
}
Type response_type(Type request) {
    switch (request) {
    case Type::SubmitTask: case Type::GetTask: case Type::ListTasks: case Type::GetLogs:
    case Type::CancelTask: case Type::RetryTask: case Type::ListWorkers:
    case Type::RegisterWorker: case Type::RunTask: case Type::WorkerCancelTask:
        return static_cast<Type>(static_cast<std::uint16_t>(request) + 1U);
    default: throw std::invalid_argument("not a request type");
    }
}
std::array<std::uint8_t, 8> encode_u64(std::uint64_t value) noexcept {
    std::array<std::uint8_t, 8> bytes{};
    put(bytes.data(), value, bytes.size());
    return bytes;
}
std::uint64_t decode_u64(const std::uint8_t* bytes) noexcept { return get(bytes, 8); }
ProtocolResult<std::array<std::uint8_t, kHeaderSize>> encode_header(Header header, Direction expected) {
    auto valid = validate(header, expected);
    if (!valid) return ProtocolResult<std::array<std::uint8_t, kHeaderSize>>::failure(valid.error().kind, valid.error().message);
    std::array<std::uint8_t, kHeaderSize> bytes{};
    put(bytes.data(), kMagic, 4); put(bytes.data() + 4, kVersion, 2);
    put(bytes.data() + 6, static_cast<std::uint16_t>(header.type), 2);
    put(bytes.data() + 8, header.request_id, 8); put(bytes.data() + 16, header.payload_length, 4);
    return ProtocolResult<decltype(bytes)>::success(bytes);
}
ProtocolResult<Header> decode_header(const std::uint8_t* bytes, Direction expected) {
    if (get(bytes, 4) != kMagic || get(bytes + 4, 2) != kVersion)
        return ProtocolResult<Header>::failure(ErrorKind::Framing, "invalid magic or version");
    return validate(Header{static_cast<Type>(get(bytes + 6, 2)), get(bytes + 8, 8),
                           static_cast<std::uint32_t>(get(bytes + 16, 4))}, expected);
}
ProtocolResult<std::string> encode_frame(const Frame& frame, Direction expected) {
    if (frame.payload.size() != frame.header.payload_length)
        return ProtocolResult<std::string>::failure(ErrorKind::Framing, "payload/header length mismatch");
    auto header = encode_header(frame.header, expected);
    if (!header) return ProtocolResult<std::string>::failure(header.error().kind, header.error().message);
    std::string bytes(reinterpret_cast<const char*>(header.value().data()), kHeaderSize);
    bytes += frame.payload;
    return ProtocolResult<std::string>::success(std::move(bytes));
}
FrameParser::FrameParser(Direction expected, std::size_t high_water) : expected_(expected), high_water_(high_water) {
    if (high_water < kHeaderSize + kMaxPayload) throw std::invalid_argument("input bound must support maximum wire frame");
}
std::size_t FrameParser::buffered_bytes() const noexcept { return header_used_ + payload_.size(); }
ProtocolResult<FeedStatus> FrameParser::feed(std::string_view bytes, const Consumer& consumer) {
    if (failed_) return ProtocolResult<FeedStatus>::failure(ErrorKind::Framing, "parser already failed");
    std::size_t frames = 0;
    while (!bytes.empty()) {
        if (header_used_ < kHeaderSize) {
            const auto count = std::min(kHeaderSize - header_used_, bytes.size());
            std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()), count, header_bytes_.data() + header_used_);
            header_used_ += count; bytes.remove_prefix(count);
            if (header_used_ != kHeaderSize) break;
            auto decoded = decode_header(header_bytes_.data(), expected_);
            if (!decoded) {
                failed_ = true;
                return ProtocolResult<FeedStatus>::failure(decoded.error().kind, decoded.error().message);
            }
            header_ = decoded.value();
            if (kHeaderSize + header_.payload_length > high_water_) {
                failed_ = true;
                return ProtocolResult<FeedStatus>::failure(ErrorKind::Framing, "input high-water exceeded");
            }
            payload_.reserve(header_.payload_length);
        }
        const auto count = std::min(static_cast<std::size_t>(header_.payload_length) - payload_.size(), bytes.size());
        payload_.append(bytes.data(), count); bytes.remove_prefix(count);
        if (payload_.size() != header_.payload_length) break;
        Frame frame{header_, std::move(payload_)};
        payload_ = {}; header_used_ = 0;
        // Consumer owns the completed frame; exceptions make this parser unusable as well.
        try {
            if (!consumer(std::move(frame))) {
                failed_ = true;
                return ProtocolResult<FeedStatus>::failure(ErrorKind::Schema, "frame consumer rejected payload");
            }
        } catch (...) { failed_ = true; throw; }
        ++frames;
    }
    return ProtocolResult<FeedStatus>::success({frames, header_used_ != 0});
}
} // namespace ts::protocol
