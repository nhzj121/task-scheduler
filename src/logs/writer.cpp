#include "ts/logs/writer.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ts/common/unique_fd.h"
#include "ts/domain/ids.h"
#include "ts/io/event_fd.h"

namespace ts::logs {
bool operator==(const Binding& a, const Binding& b) noexcept {
    return a.task == b.task && a.attempt == b.attempt && a.session == b.session && a.generation == b.generation &&
        a.number == b.number && a.stdout_path == b.stdout_path && a.stderr_path == b.stderr_path;
}
bool valid_binding(const Binding& b) {
    if (!domain::TaskId::parse(b.task) || !domain::AttemptId::parse(b.attempt) ||
        !domain::SessionId::parse(b.session) || !b.number) return false;
    const auto stem = "tasks/" + b.task + "/attempt-" + std::to_string(b.number);
    return b.stdout_path == stem + ".stdout.log" && b.stderr_path == stem + ".stderr.log";
}
namespace {
struct Failure { Error error; };
[[noreturn]] void bad(ErrorCode code, Operation op, int number = 0) { throw Failure{{code, op, number}}; }
constexpr std::uint64_t max_page = 3144960;
}
struct Writer::Impl {
    struct Record { Binding binding; Positions visible; bool failed{false}, sealed{false}; };
    struct Finished { Completion value; std::size_t read_charge; };
    std::string root;
    Limits limits;
    std::shared_ptr<FilesystemHook> hook;
    io::EventFd event;
    mutable std::mutex mutex;
    std::condition_variable available;
    std::deque<Command> writes, essential, reads;
    std::vector<Finished> finished;
    std::size_t finished_count{0};
    Usage used;
    Status state;
    bool stopping{false};
    std::thread thread;
    // Everything below is touched on LogWriterThread only.
    common::UniqueFd root_fd, tasks_fd;
    bool root_dirty{true}, tasks_dirty{true}; // preexisting names may precede an uncommitted crashed barrier
    std::map<std::string, bool> dirty_tasks;
    std::map<std::string, Record> records;
    Impl(std::string path, Limits bounds, std::shared_ptr<FilesystemHook> seam, io::EventFd fd)
        : root(std::move(path)), limits(bounds), hook(std::move(seam)), event(std::move(fd)),
          finished(bounds.write_slots + bounds.essential_slots + bounds.read_slots) {}
    FaultAction before(Operation op, const std::string& path, std::size_t size = 0) {
        auto action = hook ? hook->before(op, path, size) : FaultAction{};
        if (action.error) bad(ErrorCode::Io, op, action.error);
        return action;
    }
    void after(Operation op, const std::string& path) {
        if (hook) { const int error = hook->after(op, path); if (error) bad(ErrorCode::Io, op, error); }
    }
    template<class F> auto retry(Operation op, const std::string& path, F call) {
        // EINTR can safely retry these idempotent/offset-stable operations. Close
        // is deliberately excluded: Linux may already have released that fd.
        for (;;) {
            try {
                before(op, path); const auto result = call();
                if (result < 0) bad(ErrorCode::Io, op, errno);
                after(op, path); return result;
            } catch (const Failure& e) { if (e.error.system_error != EINTR) throw; }
        }
    }
    template<class F> common::UniqueFd opened(Operation op, const std::string& path, F call) {
        for (;;) {
            try {
                before(op, path); common::UniqueFd fd(call());
                if (!fd) bad(ErrorCode::Io, op, errno);
                after(op, path); return fd;
            } catch (const Failure& e) { if (e.error.system_error != EINTR) throw; }
        }
    }
    void close(common::UniqueFd& fd, const std::string& path) {
        if (!fd) return;
        std::optional<Error> failure;
        try { before(Operation::Close, path); } catch (const Failure& e) { failure = e.error; }
        const int number = fd.release();
        if (::close(number) != 0 && !failure) failure = Error{ErrorCode::Io, Operation::Close, errno};
        if (failure) throw Failure{*failure};
        after(Operation::Close, path);
    }
    void directory_safety(int fd) {
        struct stat st{};
        if (::fstat(fd, &st) != 0) bad(ErrorCode::Io, Operation::DirectoryOpen, errno);
        if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0022))
            bad(ErrorCode::Invalid, Operation::DirectoryOpen);
    }
    common::UniqueFd directory(int parent, const std::string& name, const std::string& path, bool create, bool& parent_dirty) {
        if (create) {
            bool made = false;
            try {
                retry(Operation::Mkdir, path, [&] {
                    const auto result = ::mkdirat(parent, name.c_str(), 0700);
                    if (result == 0) parent_dirty = true; // before any after-syscall hook/interrupt
                    return result;
                }); made = true;
            } catch (const Failure& e) { if (e.error.system_error != EEXIST) throw; }
            if (made) parent_dirty = true;
        }
        auto fd = opened(Operation::DirectoryOpen, path, [&] {
            return ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        });
        directory_safety(fd.get()); return fd;
    }
    void initialize() {
        if (root.empty() || root.front() != '/' || root == "/" || root.find('\0') != std::string::npos)
            bad(ErrorCode::Invalid, Operation::RootOpen);
        root_fd = opened(Operation::RootOpen, ".", [&] { return ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); });
        directory_safety(root_fd.get());
        // Do not create tasks/ at startup: explicit ProvenEmpty paths must remain
        // independent of namespace-creation failures. First real log work creates it.
    }
    common::UniqueFd task_directory(const Binding& b, bool create) {
        if (!tasks_fd) tasks_fd = directory(root_fd.get(), "tasks", "tasks", create, root_dirty);
        if (!create) return directory(tasks_fd.get(), b.task, "tasks/" + b.task, false, tasks_dirty);
        auto found = dirty_tasks.find(b.task);
        if (found == dirty_tasks.end()) {
            if (dirty_tasks.size() == limits.owners) {
                const auto clean = std::find_if(dirty_tasks.begin(), dirty_tasks.end(), [](const auto& x) { return !x.second; });
                if (clean == dirty_tasks.end()) bad(ErrorCode::Invariant, Operation::DirectoryOpen);
                dirty_tasks.erase(clean);
            }
            found = dirty_tasks.emplace(b.task, true).first;
        }
        return directory(tasks_fd.get(), b.task, "tasks/" + b.task, create, tasks_dirty);
    }
    common::UniqueFd file(int parent, const std::string& path, bool create, bool& dirty) {
        const auto name = path.substr(path.find_last_of('/') + 1);
        common::UniqueFd result;
        if (create) {
            try {
                result = opened(Operation::FileOpen, path, [&] {
                    const auto fd = ::openat(parent, name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                    if (fd >= 0) dirty = true;
                    return fd;
                }); dirty = true;
            } catch (const Failure& e) { if (e.error.system_error != EEXIST) throw; }
        }
        if (!result) result = opened(Operation::FileOpen, path, [&] {
            return ::openat(parent, name.c_str(), (create ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        });
        struct stat st{};
        if (::fstat(result.get(), &st) != 0) bad(ErrorCode::Io, Operation::FileOpen, errno);
        if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0022) || st.st_nlink != 1)
            bad(ErrorCode::Invalid, Operation::FileOpen);
        return result;
    }
    void size(int fd, std::uint64_t expected, bool exact) {
        struct stat st{};
        if (::fstat(fd, &st) != 0) bad(ErrorCode::Io, Operation::FileOpen, errno);
        if (st.st_size < 0 || (exact ? static_cast<std::uint64_t>(st.st_size) != expected : static_cast<std::uint64_t>(st.st_size) < expected))
            bad(ErrorCode::Invariant, Operation::FileOpen);
    }
    Record& record(const Binding& b) {
        auto it = records.find(b.attempt);
        if (it == records.end()) {
            if (records.size() >= limits.owners) bad(ErrorCode::Invariant, Operation::FileOpen);
            it = records.emplace(b.attempt, Record{b, {}, false, false}).first;
        }
        if (!(it->second.binding == b)) bad(ErrorCode::Invariant, Operation::FileOpen);
        return it->second;
    }
    void perform(Command& c, Completion& result) {
        if (!valid_binding(c.binding)) bad(ErrorCode::Invalid, Operation::FileOpen);
        if (c.kind == Kind::Forget) {
            const auto it = records.find(c.binding.attempt);
            if (it != records.end() && !(it->second.binding == c.binding)) bad(ErrorCode::Invariant, Operation::Close);
            records.erase(c.binding.attempt);
            const bool retained = std::any_of(records.begin(), records.end(), [&](const auto& record) { return record.second.binding.task == c.binding.task; });
            if (!retained) dirty_tasks.erase(c.binding.task);
            return;
        }
        if (c.kind == Kind::Read) {
            auto visible = c.historical_visible;
            const auto known = records.find(c.binding.attempt);
            if (known != records.end() && !(known->second.binding == c.binding)) bad(ErrorCode::Invariant, Operation::Read);
            if (known != records.end() && known->second.binding == c.binding)
                visible = c.stream == Stream::Out ? known->second.visible.out.bytes : known->second.visible.err.bytes;
            result.snapshot_end = c.snapshot_end.value_or(visible);
            if (result.snapshot_end > visible || c.offset > result.snapshot_end) bad(ErrorCode::Invalid, Operation::Read);
            const auto length = static_cast<std::size_t>(std::min(c.limit, result.snapshot_end - c.offset));
            result.offset = c.offset;
            if (c.proven_empty) {
                if (visible || result.snapshot_end || c.offset) bad(ErrorCode::Invalid, Operation::Read);
                return;
            }
            if (c.allow_missing_empty && !visible && !result.snapshot_end) return;
            auto directory_fd = task_directory(c.binding, false);
            const auto& path = c.stream == Stream::Out ? c.binding.stdout_path : c.binding.stderr_path;
            bool unused = false; auto stream = file(directory_fd.get(), path, false, unused);
            size(stream.get(), visible, false);
            result.data.resize(length); std::size_t done = 0;
            while (done < length) {
                try {
                    const auto action = before(Operation::Read, path, length - done);
                    const auto count = ::pread(stream.get(), result.data.data() + done, std::min(action.maximum_io, length - done),
                        static_cast<off_t>(c.offset + done));
                    if (count <= 0) bad(ErrorCode::Io, Operation::Read, count < 0 ? errno : EIO);
                    done += static_cast<std::size_t>(count); after(Operation::Read, path);
                } catch (const Failure& e) { if (e.error.system_error != EINTR) throw; }
            }
            close(stream, path); close(directory_fd, "tasks/" + c.binding.task); return;
        }
        auto& owned = record(c.binding);
        if (owned.failed || owned.sealed) bad(ErrorCode::Invariant, Operation::Write);
        auto directory_fd = task_directory(c.binding, true);
        auto& dirty = dirty_tasks.at(c.binding.task);
        auto out = file(directory_fd.get(), c.binding.stdout_path, true, dirty);
        auto err = file(directory_fd.get(), c.binding.stderr_path, true, dirty);
        size(out.get(), owned.visible.out.bytes, true); size(err.get(), owned.visible.err.bytes, true);
        if (c.kind == Kind::Append) {
            auto& position = c.stream == Stream::Out ? owned.visible.out : owned.visible.err;
            if (c.sequence != position.sequence || position.sequence == UINT64_MAX || c.data.size() > static_cast<std::uint64_t>(INT64_MAX) - position.bytes)
                bad(ErrorCode::Invariant, Operation::Write);
            auto& fd = c.stream == Stream::Out ? out : err;
            const auto& path = c.stream == Stream::Out ? c.binding.stdout_path : c.binding.stderr_path;
            if (::lseek(fd.get(), static_cast<off_t>(position.bytes), SEEK_SET) < 0) bad(ErrorCode::Io, Operation::Write, errno);
            std::size_t done = 0;
            while (done < c.data.size()) {
                try {
                    const auto action = before(Operation::Write, path, c.data.size() - done);
                    const auto count = ::write(fd.get(), c.data.data() + done, std::min(action.maximum_io, c.data.size() - done));
                    if (count <= 0) bad(ErrorCode::Io, Operation::Write, count < 0 ? errno : EIO);
                    done += static_cast<std::size_t>(count); after(Operation::Write, path);
                } catch (const Failure& e) { if (e.error.system_error != EINTR) throw; }
            }
            close(out, c.binding.stdout_path); close(err, c.binding.stderr_path); close(directory_fd, "tasks/" + c.binding.task);
            position.bytes += c.data.size(); ++position.sequence; result.visible = owned.visible;
        } else {
            if (!(owned.visible.out == c.final.out) || !(owned.visible.err == c.final.err)) bad(ErrorCode::Invariant, Operation::FileSync);
            retry(Operation::FileSync, c.binding.stdout_path, [&] { return ::fdatasync(out.get()); });
            retry(Operation::FileSync, c.binding.stderr_path, [&] { return ::fdatasync(err.get()); });
            if (dirty) { retry(Operation::DirectorySync, "tasks/" + c.binding.task, [&] { return ::fsync(directory_fd.get()); }); dirty = false; }
            if (tasks_dirty) { retry(Operation::DirectorySync, "tasks", [&] { return ::fsync(tasks_fd.get()); }); tasks_dirty = false; }
            if (root_dirty) { retry(Operation::DirectorySync, ".", [&] { return ::fsync(root_fd.get()); }); root_dirty = false; }
            close(out, c.binding.stdout_path); close(err, c.binding.stderr_path); close(directory_fd, "tasks/" + c.binding.task);
            owned.sealed = true; result.visible = owned.visible; result.barrier_ok = true;
        }
    }
    bool prior_write(const Command& barrier) const {
        const auto same = [&](const auto& c) { return c.binding.attempt == barrier.binding.attempt; };
        // Retirement must not erase visible positions still owned by an accepted
        // snapshot. A final barrier retains priority over lower-priority reads.
        return std::any_of(writes.begin(), writes.end(), same) ||
            (barrier.kind == Kind::Forget && std::any_of(reads.begin(), reads.end(), same));
    }
    void publish(Completion result, std::size_t read_charge) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (finished_count == finished.size()) std::terminate(); // pre-reserved live command ownership
            finished[finished_count++] = {std::move(result), read_charge};
        }
        try { before(Operation::Notify, "eventfd"); if (!event.notify()) bad(ErrorCode::Io, Operation::Notify); after(Operation::Notify, "eventfd"); }
        catch (...) {
            std::lock_guard<std::mutex> lock(mutex); state = {Health::Failed, Error{ErrorCode::Io, Operation::Notify, EIO}}; stopping = true;
        }
    }
    void run() noexcept {
        try {
            initialize();
            { std::lock_guard<std::mutex> lock(mutex); state.health = Health::Ready; }
            static_cast<void>(event.notify());
            for (;;) {
                Command command;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    available.wait(lock, [&] { return stopping || !writes.empty() || !essential.empty() || !reads.empty(); });
                    if (writes.empty() && essential.empty() && reads.empty()) { if (stopping) break; else continue; }
                    auto* queue = !essential.empty() && !prior_write(essential.front()) ? &essential : !writes.empty() ? &writes : &reads;
                    command = std::move(queue->front()); queue->pop_front();
                }
                Completion result; result.operation = command.operation; result.kind = command.kind;
                result.binding = std::move(command.binding); command.binding = result.binding;
                result.stream = command.stream; result.sequence = command.sequence; result.raw_size = command.data.size();
                try { perform(command, result); }
                catch (const Failure& e) {
                    result.error = e.error; result.barrier_ok = false; result.data.clear();
                    if (command.kind != Kind::Read) {
                        const auto record_it = records.find(command.binding.attempt);
                        if (record_it != records.end()) record_it->second.failed = true;
                    }
                }
                publish(std::move(result), command.kind == Kind::Read ? static_cast<std::size_t>(command.limit) : 0);
            }
            close(tasks_fd, "tasks"); close(root_fd, "."); records.clear();
            { std::lock_guard<std::mutex> lock(mutex); if (state.health != Health::Failed) state.health = Health::Stopped; }
            static_cast<void>(event.notify());
        } catch (const Failure& e) {
            std::lock_guard<std::mutex> lock(mutex); state = {Health::Failed, e.error}; stopping = true;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex); state = {Health::Failed, Error{ErrorCode::Invariant, Operation::Notify, 0}}; stopping = true;
        }
        // Any initialization/internal failure is explicit fail-stop. Close owned
        // filesystem descriptors ON THIS THREAD, never in the EventLoop destructor.
        tasks_fd.reset(); root_fd.reset();
        static_cast<void>(event.notify());
    }
};
Writer::Writer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Writer::~Writer() { stop(); join(); }
common::Result<std::unique_ptr<Writer>> Writer::start(std::string root, Limits limits, std::shared_ptr<FilesystemHook> hook) {
    using R = common::Result<std::unique_ptr<Writer>>;
    if (!limits.bytes || !limits.write_slots || !limits.essential_slots || !limits.read_slots || !limits.owners ||
        limits.read_bytes < max_page || limits.write_slots > SIZE_MAX - limits.essential_slots ||
        limits.read_slots > SIZE_MAX - limits.write_slots - limits.essential_slots) return R::failure("invalid log channel bounds");
    auto fd = io::EventFd::create(); if (!fd) return R::failure(fd.error().message);
    try {
        auto impl = std::make_unique<Impl>(std::move(root), limits, std::move(hook), std::move(fd).value());
        auto result = std::unique_ptr<Writer>(new Writer(std::move(impl)));
        auto* owner = result->impl_.get(); owner->thread = std::thread([owner] { owner->run(); });
        return R::success(std::move(result));
    } catch (const std::exception& e) { return R::failure(e.what()); }
}
Admission Writer::try_post(Command command) {
    auto& s = *impl_; std::lock_guard<std::mutex> lock(s.mutex);
    if (s.state.health == Health::Failed) return Admission::Failed;
    if (s.stopping || s.state.health != Health::Ready) return Admission::Stopped;
    if (!command.operation || !valid_binding(command.binding)) return Admission::Invalid;
    if (command.kind == Kind::Append) {
        if (command.data.empty() || command.data.size() > 65536) return Admission::Invalid;
        if (s.used.writes == s.limits.write_slots || command.data.size() > s.limits.bytes - s.used.bytes) return Admission::Full;
        const auto size = command.data.size(); s.writes.push_back(std::move(command)); s.used.bytes += size; ++s.used.writes;
    } else if (command.kind == Kind::Read) {
        if (!command.limit || command.limit > max_page || command.historical_visible > static_cast<std::uint64_t>(INT64_MAX)) return Admission::Invalid;
        if (s.used.reads == s.limits.read_slots || command.limit > s.limits.read_bytes - s.used.read_bytes) return Admission::Full;
        const auto size = static_cast<std::size_t>(command.limit); s.reads.push_back(std::move(command)); s.used.read_bytes += size; ++s.used.reads;
    } else {
        if (s.used.essential == s.limits.essential_slots) return Admission::Full;
        s.essential.push_back(std::move(command)); ++s.used.essential;
    }
    s.available.notify_one(); return Admission::Accepted;
}
common::Result<std::vector<Completion>> Writer::drain() {
    using R = common::Result<std::vector<Completion>>;
    const auto notification = impl_->event.drain(); if (!notification) return R::failure(notification.error().message);
    auto& s = *impl_; std::lock_guard<std::mutex> lock(s.mutex);
    std::vector<Completion> results; results.reserve(s.finished_count);
    for (std::size_t i = 0; i < s.finished_count; ++i) {
        auto& item = s.finished[i];
        if (item.value.kind == Kind::Append) { s.used.bytes -= static_cast<std::size_t>(item.value.raw_size); --s.used.writes; }
        else if (item.value.kind == Kind::Read) { s.used.read_bytes -= item.read_charge; --s.used.reads; }
        else --s.used.essential;
        results.push_back(std::move(item.value));
    }
    s.finished_count = 0; return R::success(std::move(results));
}
int Writer::fd() const noexcept { return impl_->event.fd(); }
Usage Writer::usage() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->used; }
Status Writer::status() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->state; }
void Writer::stop() { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stopping = true; impl_->available.notify_one(); }
void Writer::join() { if (impl_->thread.joinable()) impl_->thread.join(); }
} // namespace ts::logs
