#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "ts/common/result.h"

namespace ts::logs {
enum class Stream { Out, Err };
struct Position {
    std::uint64_t bytes{0}, sequence{0};
    friend bool operator==(Position a, Position b) noexcept { return a.bytes == b.bytes && a.sequence == b.sequence; }
};
struct Positions { Position out, err; };
struct Binding {
    std::string task, attempt, session;
    std::uint64_t generation{0};
    std::uint32_t number{0};
    std::string stdout_path, stderr_path;
    friend bool operator==(const Binding& a, const Binding& b) noexcept;
};
enum class Kind { Append, Barrier, Read, Forget };
struct Command {
    std::uint64_t operation{0};
    Kind kind{Kind::Append};
    Binding binding;
    Stream stream{Stream::Out};
    std::uint64_t sequence{0};
    std::string data;
    Positions final;
    std::uint64_t offset{0}, limit{262144};
    std::optional<std::uint64_t> snapshot_end;
    // Committed historical bound; active owners use their actual visible position.
    std::uint64_t historical_visible{0};
    bool proven_empty{false};
    bool allow_missing_empty{false}; // incomplete history/active stream with no observed visible bytes
};
enum class Operation { RootOpen, Mkdir, DirectoryOpen, FileOpen, Write, Read, FileSync, DirectorySync, Close, Notify };
struct FaultAction { int error{0}; std::size_t maximum_io{SIZE_MAX}; };
class FilesystemHook {
public:
    virtual ~FilesystemHook() = default;
    // Test seam only: runs exclusively on Writer, never calls application owners.
    virtual FaultAction before(Operation, const std::string&, std::size_t) { return {}; }
    virtual int after(Operation, const std::string&) { return 0; }
};
enum class ErrorCode { Invalid, Io, Invariant, Aborted };
struct Error { ErrorCode code; Operation operation; int system_error; };
struct Completion {
    std::uint64_t operation{0}; Kind kind{Kind::Append}; Binding binding;
    Stream stream{Stream::Out}; std::uint64_t sequence{0}, raw_size{0};
    Positions visible;
    std::uint64_t offset{0}, snapshot_end{0};
    std::string data;
    std::optional<Error> error;
    bool barrier_ok{false};
};
struct Limits {
    std::size_t bytes{67108864}, write_slots{3968}, essential_slots{128};
    std::size_t read_slots{4}, read_bytes{8388608}, owners{4096};
};
struct Usage { std::size_t bytes{0}, writes{0}, essential{0}, reads{0}, read_bytes{0}; };
enum class Admission { Accepted, Full, Invalid, Stopped, Failed };
enum class Health { Starting, Ready, Failed, Stopped };
struct Status { Health health{Health::Starting}; std::optional<Error> error; };

// Sole task-log filesystem owner. Queue usage includes queued, executing AND
// completed-undrained commands. Completions have preallocated delivery slots.
// Caller unregisters fd() before destruction, and stop/join only outside dispatch.
class Writer final {
public:
    static common::Result<std::unique_ptr<Writer>> start(std::string root, Limits limits = {},
        std::shared_ptr<FilesystemHook> hook = {});
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    [[nodiscard]] Admission try_post(Command command);
    [[nodiscard]] common::Result<std::vector<Completion>> drain();
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] Usage usage() const;
    [[nodiscard]] Status status() const;
    void stop();
    void join();
private:
    struct Impl;
    explicit Writer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] bool valid_binding(const Binding& binding);
} // namespace ts::logs
