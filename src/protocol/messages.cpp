#include "ts/protocol/messages.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace ts::protocol {
namespace {
using Json = nlohmann::json;
struct Invalid : std::runtime_error { using std::runtime_error::runtime_error; };
void require(bool condition, const char* message) { if (!condition) throw Invalid(message); }
// ERROR_RESPONSE.details is an open diagnostic object. Traverse it iteratively rather than
// adding an undocumented wire nesting limit or relying on recursive JSON copy/serialization.
Json clone_json(const Json& source) {
    const auto shell = [](const Json& value) -> Json {
        if (value.is_object()) return Json::object();
        if (value.is_array()) return Json::array();
        return value;
    };
    Json result = shell(source);
    struct Entry { const Json* source; Json* destination; Json::const_iterator next; };
    std::vector<Entry> stack;
    if (source.is_structured()) stack.push_back({&source,&result,source.begin()});
    while (!stack.empty()) {
        auto& current = stack.back();
        if (current.next == current.source->end()) { stack.pop_back(); continue; }
        const auto item = current.next++;
        Json* child;
        if (current.source->is_object()) {
            auto& target = (*current.destination)[item.key()]; target = shell(*item); child = &target;
        } else {
            current.destination->push_back(shell(*item)); child = &current.destination->back();
        }
        if (item->is_structured()) stack.push_back({&*item,child,item->begin()});
    }
    return result;
}
std::string dump_bounded(const Json& source) {
    std::string output;
    auto append = [&](std::string_view bytes) {
        require(bytes.size() <= kMaxPayload - output.size(), "encoded payload exceeds wire maximum");
        output.append(bytes);
    };
    struct Entry { const Json* source; Json::const_iterator next; bool first; };
    std::vector<Entry> stack;
    auto begin = [&](const Json& value) {
        if (!value.is_structured()) { append(value.dump()); return; }
        append(value.is_object() ? "{" : "["); stack.push_back({&value,value.begin(),true});
    };
    begin(source);
    while (!stack.empty()) {
        auto& current = stack.back();
        if (current.next == current.source->end()) {
            append(current.source->is_object() ? "}" : "]"); stack.pop_back(); continue;
        }
        if (!current.first) append(",");
        current.first = false; const auto item = current.next++;
        if (current.source->is_object()) { append(Json(item.key()).dump()); append(":"); }
        begin(*item);
    }
    return output;
}
template <typename T> T read(const Json& value);
template <typename T> Json write(const T& value);
template <typename T> auto schema();
template <typename C, typename M> struct Field {
    const char* name; M C::* member; std::optional<M> default_value;
};
template <typename C, typename M> auto f(const char* name, M C::* member) { return Field<C,M>{name, member, std::nullopt}; }
template <typename C, typename M> auto d(const char* name, M C::* member, M value) { return Field<C,M>{name, member, std::move(value)}; }
#define F(name) f(#name, &T::name)
#define EXEC_FIELDS F(executable), F(args), F(env), F(cwd), F(resources), F(timeout_ms)
#define BINDING F(worker_id), F(session_id)
#define ATTEMPT_BINDING BINDING, F(task_id), F(attempt_id)
#define SCHEMA(type, ...) template <> auto schema<type>() { using T = type; return std::make_tuple(__VA_ARGS__); }
SCHEMA(SubmitTask, F(executable), F(args), d("env", &T::env, Environment{}),
       d("cwd", &T::cwd, std::optional<std::string>{}),
       d("resources", &T::resources, domain::ResourceRequest::create(1,0).value()),
       d("timeout_ms", &T::timeout_ms, Milliseconds{}))
SCHEMA(Task, F(task_id), F(state), EXEC_FIELDS, F(cancel_requested_at), F(created_at), F(updated_at), F(latest_attempt_id))
SCHEMA(Attempt, F(attempt_id), F(task_id), F(attempt_no), F(state), BINDING,
       F(assigned_at), F(started_at), F(finished_at), F(exit_code), F(term_signal), F(reason),
       F(stdout_bytes), F(stderr_bytes), F(logs_complete), F(finalization_pending))
SCHEMA(TaskSummary, F(task_id), F(state), F(executable), F(attempt_count), F(created_at), F(updated_at))
SCHEMA(Worker, F(worker_id), F(state), F(session_id), F(capacity), F(reserved), F(running_attempts), F(max_concurrent_tasks), F(last_heartbeat_at))
SCHEMA(ErrorResponse, F(code), F(message), F(retryable), F(details))
SCHEMA(SubmitTaskResponse, F(task_id), F(state), F(created_at))
SCHEMA(GetTask, F(task_id))
SCHEMA(GetTaskResponse, F(task), F(attempts))
SCHEMA(ListTasks, d("page_size", &T::page_size, std::uint64_t{100}), F(page_token))
SCHEMA(ListTasksResponse, F(tasks), F(next_page_token))
SCHEMA(GetLogs, F(task_id), F(attempt_no), F(stream), F(offset), d("limit_bytes", &T::limit_bytes, std::uint64_t{262144}), F(snapshot_end))
SCHEMA(GetLogsResponse, F(task_id), F(attempt_no), F(stream), F(offset), F(next_offset), F(snapshot_end), F(data_base64), F(eof_snapshot), F(logs_complete))
SCHEMA(CancelTask, F(task_id))
SCHEMA(CancelTaskResponse, F(task_id), F(status), F(task_state), F(cancel_requested_at))
SCHEMA(RetryTask, F(task_id))
SCHEMA(RetryTaskResponse, F(task_id), F(state), F(next_attempt_no))
template <> auto schema<ListWorkers>() { return std::tuple<>{}; }
SCHEMA(ListWorkersResponse, F(workers))
SCHEMA(RegisterWorker, BINDING, F(capacity), F(max_concurrent_tasks), F(worker_version))
SCHEMA(RegistrationAccepted, F(accepted), F(heartbeat_interval_ms), F(offline_timeout_ms), F(termination_grace_ms),
       F(initial_log_window_bytes), F(initial_log_window_chunks), F(controller_instance_id))
SCHEMA(RegistrationRejected, F(accepted), F(error_code), F(message))
SCHEMA(Heartbeat, BINDING, F(sequence), F(running_attempt_ids))
SCHEMA(RunTask, ATTEMPT_BINDING, F(attempt_no), EXEC_FIELDS)
SCHEMA(RunTaskAck, BINDING, F(attempt_id), F(accepted), F(error_code), F(message))
SCHEMA(TaskStarted, ATTEMPT_BINDING, F(pid))
SCHEMA(LogChunk, ATTEMPT_BINDING, F(stream), F(sequence), F(raw_size), F(data_base64))
SCHEMA(LogWindowUpdate, F(grant_bytes), F(grant_chunks))
SCHEMA(TaskFinished, ATTEMPT_BINDING, F(outcome), F(exit_code), F(term_signal), F(reason),
       F(stdout_next_sequence), F(stderr_next_sequence), F(stdout_bytes), F(stderr_bytes))
SCHEMA(WorkerCancelTask, ATTEMPT_BINDING, F(grace_ms))
SCHEMA(WorkerCancelTaskAck, BINDING, F(attempt_id), F(accepted), F(already_terminal), F(message))
#undef SCHEMA
#undef ATTEMPT_BINDING
#undef BINDING
#undef EXEC_FIELDS
#undef F

template <typename T> struct Optional : std::false_type {};
template <typename T> struct Optional<std::optional<T>> : std::true_type { using Inner = T; };
template <typename T> struct Vector : std::false_type {};
template <typename T> struct Vector<std::vector<T>> : std::true_type { using Inner = T; };
template <typename T> struct Uuid : std::false_type {};
template <typename T> struct Uuid<domain::UuidId<T>> : std::true_type {};
template <typename T> T checked(common::Result<T> result) {
    if (!result) throw Invalid(result.error().message);
    return std::move(result).value();
}
template <typename E> const auto& tokens();
template <> const auto& tokens<Stream>() { static const std::array values{"stdout", "stderr"}; return values; }
template <> const auto& tokens<ErrorCode>() { static const std::array values{"INVALID_ARGUMENT", "NOT_FOUND", "CONFLICT", "UNSUPPORTED", "TEMPORARILY_UNAVAILABLE", "INTERNAL"}; return values; }
template <> const auto& tokens<CancelStatus>() { static const std::array values{"accepted", "completed", "already_cancelled"}; return values; }
template <> const auto& tokens<Outcome>() { static const std::array values{"EXITED", "SIGNALED", "TIMED_OUT", "CANCELLED", "LAUNCH_FAILED"}; return values; }
template <typename C, typename M> M read_field(const Json& value, const Field<C,M>& field) {
    auto it = value.find(field.name);
    if (it == value.end()) {
        if (field.default_value) return *field.default_value;
        throw Invalid(std::string("missing field: ") + field.name);
    }
    try { return read<M>(*it); }
    catch (const Invalid& error) { throw Invalid(std::string(field.name) + ": " + error.what()); }
}
template <typename T, typename Tuple, std::size_t... I>
T read_object(const Json& value, const Tuple& fields, std::index_sequence<I...>) {
    require(value.is_object(), "expected object");
    for (auto it = value.begin(); it != value.end(); ++it) {
        const bool known = ((it.key() == std::get<I>(fields).name) || ... || false);
        require(known, "unknown field");
    }
    // Stage each field in an already-owned RAII slot. In GCC 11, direct
    // aggregate-return initialization can leak earlier members if a later
    // field throws. Preserve the same left-to-right validation/diagnostics.
    std::tuple<std::optional<decltype(read_field(value, std::get<I>(fields)))>...> parsed;
    (std::get<I>(parsed).emplace(read_field(value, std::get<I>(fields))), ...);
    return T{std::move(*std::get<I>(parsed))...};
}
template <typename T> T read(const Json& value) {
    if constexpr (std::is_same_v<T, Json>) { require(value.is_object(), "expected details object"); return clone_json(value); }
    else if constexpr (std::is_same_v<T, std::string>) { require(value.is_string(), "expected string"); return value.get<std::string>(); }
    else if constexpr (std::is_same_v<T, bool>) { require(value.is_boolean(), "expected boolean"); return value.get<bool>(); }
    else if constexpr (std::is_integral_v<T>) {
        require(value.is_number_integer(), "expected integer");
        if (value.is_number_unsigned()) {
            const auto n = value.get<std::uint64_t>();
            require(n <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()), "integer overflow");
            return static_cast<T>(n);
        }
        const auto n = value.get<std::int64_t>();
        if constexpr (std::is_unsigned_v<T>) require(n >= 0, "negative unsigned integer");
        else require(n >= std::numeric_limits<T>::min(), "integer underflow");
        require(n < 0 || static_cast<std::uint64_t>(n) <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()), "integer overflow");
        return static_cast<T>(n);
    } else if constexpr (Optional<T>::value) {
        if (value.is_null()) return std::nullopt;
        return read<typename Optional<T>::Inner>(value);
    } else if constexpr (Vector<T>::value) {
        require(value.is_array(), "expected array"); T result; result.reserve(value.size());
        for (const auto& entry : value) result.push_back(read<typename Vector<T>::Inner>(entry));
        return result;
    } else if constexpr (std::is_same_v<T, Environment>) {
        require(value.is_object(), "expected environment object"); T result;
        for (auto it = value.begin(); it != value.end(); ++it) result.emplace(it.key(), read<std::string>(it.value()));
        return result;
    } else if constexpr (Uuid<T>::value || std::is_same_v<T, domain::WorkerId>) return checked(T::parse(read<std::string>(value)));
    else if constexpr (std::is_same_v<T, domain::AttemptNumber>) return checked(T::create(read<std::uint32_t>(value)));
    else if constexpr (std::is_same_v<T, common::Timestamp>) return checked(T::parse(read<std::string>(value)));
    else if constexpr (std::is_same_v<T, domain::TaskState>) return checked(domain::parse_task_state(read<std::string>(value)));
    else if constexpr (std::is_same_v<T, domain::AttemptState>) return checked(domain::parse_attempt_state(read<std::string>(value)));
    else if constexpr (std::is_same_v<T, domain::WorkerState>) return checked(domain::parse_worker_state(read<std::string>(value)));
    else if constexpr (std::is_enum_v<T>) {
        const auto token = read<std::string>(value); const auto& list = tokens<T>();
        for (std::size_t i = 0; i < list.size(); ++i) if (token == list[i]) return static_cast<T>(i);
        throw Invalid("unknown enum token");
    } else if constexpr (std::is_same_v<T, domain::ResourceRequest> || std::is_same_v<T, domain::ResourceCapacity> || std::is_same_v<T, domain::ResourceUsage>) {
        require(value.is_object() && value.size() == 2 && value.contains("cpu_slots") && value.contains("memory_mb"), "invalid resource fields");
        const auto cpu = read<std::uint32_t>(value.at("cpu_slots")); const auto memory = read<std::uint64_t>(value.at("memory_mb"));
        if constexpr (std::is_same_v<T, domain::ResourceUsage>) return T::from_values(cpu, memory);
        else return checked(T::create(cpu, memory));
    } else if constexpr (std::is_same_v<T, RegisterAck>) {
        require(value.is_object() && value.contains("accepted"), "missing accepted");
        if (read<bool>(value.at("accepted"))) return {read<RegistrationAccepted>(value)};
        return {read<RegistrationRejected>(value)};
    } else {
        const auto fields = schema<T>();
        return read_object<T>(value, fields, std::make_index_sequence<std::tuple_size_v<decltype(fields)>>{});
    }
}
template <typename T> Json write(const T& value) {
    if constexpr (std::is_same_v<T, Json>) return clone_json(value);
    else if constexpr (std::is_same_v<T, std::string> || std::is_arithmetic_v<T>) return Json(value);
    else if constexpr (Optional<T>::value) { if (!value) return nullptr; return write(*value); }
    else if constexpr (Vector<T>::value) { auto array = Json::array(); for (const auto& item : value) array.push_back(write(item)); return array; }
    else if constexpr (std::is_same_v<T, Environment>) return Json(value);
    else if constexpr (Uuid<T>::value || std::is_same_v<T, domain::WorkerId> || std::is_same_v<T, domain::AttemptNumber>) return value.value();
    else if constexpr (std::is_same_v<T, common::Timestamp>) {
        try { return value.to_string(); }
        catch (const std::out_of_range&) { throw Invalid("timestamp outside four-digit UTC year range"); }
    }
    else if constexpr (std::is_same_v<T, domain::TaskState> || std::is_same_v<T, domain::AttemptState> || std::is_same_v<T, domain::WorkerState>) return std::string(domain::to_string(value));
    else if constexpr (std::is_enum_v<T>) {
        const auto index = static_cast<std::size_t>(value); const auto& list = tokens<T>();
        require(index < list.size(), "invalid enum value"); return list[index];
    } else if constexpr (std::is_same_v<T, domain::ResourceRequest> || std::is_same_v<T, domain::ResourceCapacity> || std::is_same_v<T, domain::ResourceUsage>) return Json{{"cpu_slots", value.cpu_slots()}, {"memory_mb", value.memory_mb()}};
    else if constexpr (std::is_same_v<T, RegisterAck>) return std::visit([](const auto& item) { return write(item); }, value.result);
    else {
        auto object = Json::object();
        std::apply([&](const auto&... field) { ((object[field.name] = write(value.*(field.member))), ...); }, schema<T>());
        return object;
    }
}
void positive(std::uint64_t value) { require(value > 0, "must be positive"); }
void text_no_nul(const std::string& text) { require(text.find('\0') == std::string::npos, "NUL not permitted"); }
template <typename T> void execution(const T& value, ValidationOptions options) {
    require(!value.executable.empty(), "empty executable"); text_no_nul(value.executable);
    for (const auto& arg : value.args) text_no_nul(arg);
    for (const auto& entry : value.env) {
        require(!entry.first.empty() && entry.first.find('=') == std::string::npos, "invalid environment name");
        text_no_nul(entry.first); text_no_nul(entry.second);
    }
    if (value.cwd) { require(!value.cwd->empty(), "empty cwd"); text_no_nul(*value.cwd); }
    if (value.timeout_ms) { positive(*value.timeout_ms); require(*value.timeout_ms <= options.maximum_timeout_ms, "timeout exceeds configured maximum"); }
}
template <typename T> void validate(T&, ValidationOptions) {}
void validate(SubmitTask& value, ValidationOptions options) { execution(value, options); }
void validate(Task& value, ValidationOptions options) { execution(value, options); }
void validate(RunTask& value, ValidationOptions options) { execution(value, options); }
void validate(ListTasks& value, ValidationOptions) { positive(value.page_size); value.page_size = std::min(value.page_size, std::uint64_t{200}); }
void validate(GetLogs& value, ValidationOptions) {
    positive(value.limit_bytes); value.limit_bytes = std::min(value.limit_bytes, kMaxLogPage);
    if (value.snapshot_end) require(value.offset <= *value.snapshot_end, "offset beyond snapshot");
}
void validate(GetLogsResponse& value, ValidationOptions) {
    const auto data = base64_decode(value.data_base64, static_cast<std::size_t>(kMaxLogPage));
    require(static_cast<bool>(data), "invalid log page base64");
    require(value.offset <= value.next_offset && value.next_offset <= value.snapshot_end, "invalid snapshot offsets");
    require(value.next_offset - value.offset == data.value().size(), "snapshot size mismatch");
    require(value.eof_snapshot == (value.next_offset == value.snapshot_end), "invalid snapshot EOF");
}
void validate(SubmitTaskResponse& value, ValidationOptions) { require(value.state == domain::TaskState::PENDING, "submit state must be PENDING"); }
void validate(RetryTaskResponse& value, ValidationOptions) { require(value.state == domain::TaskState::PENDING, "retry state must be PENDING"); positive(value.next_attempt_no.value()); }
void validate(Worker& value, ValidationOptions) {
    require((value.state == domain::WorkerState::OFFLINE) == !value.session_id, "invalid current Worker session");
    if (value.max_concurrent_tasks) positive(*value.max_concurrent_tasks);
}
void validate(ListWorkersResponse& value, ValidationOptions options) { for (auto& worker : value.workers) validate(worker, options); }
void validate(RegisterWorker& value, ValidationOptions) { if (value.max_concurrent_tasks) positive(*value.max_concurrent_tasks); }
void validate(RegisterAck& value, ValidationOptions) {
    if (auto* accepted = std::get_if<RegistrationAccepted>(&value.result)) {
        require(accepted->accepted, "invalid accepted variant"); positive(accepted->heartbeat_interval_ms);
        require(accepted->offline_timeout_ms > accepted->heartbeat_interval_ms, "offline timeout must exceed heartbeat interval");
        positive(accepted->termination_grace_ms);
        require(accepted->initial_log_window_bytes > 0 || accepted->initial_log_window_chunks > 0, "empty initial window");
    } else require(!std::get<RegistrationRejected>(value.result).accepted, "invalid rejected variant");
}
void validate(Heartbeat& value, ValidationOptions) { positive(value.sequence); }
void validate(TaskStarted& value, ValidationOptions) { require(value.pid > 0, "invalid pid"); }
void validate(LogWindowUpdate& value, ValidationOptions) { require(value.grant_bytes > 0 || value.grant_chunks > 0, "empty credit grant"); }
void validate(LogChunk& value, ValidationOptions) {
    require(value.stream == Stream::Stdout || value.stream == Stream::Stderr, "invalid stream");
    require(value.raw_size > 0 && value.raw_size <= 65536, "invalid log raw_size");
    auto decoded = base64_decode(value.data_base64, 65536);
    require(decoded && decoded.value().size() == value.raw_size, "invalid base64 or raw_size mismatch");
}
void validate(TaskFinished& value, ValidationOptions) {
    if (value.outcome == Outcome::Exited) require(value.exit_code.has_value(), "EXITED requires exit_code");
    if (value.outcome == Outcome::Signaled) require(value.term_signal.has_value(), "SIGNALED requires term_signal");
    if (value.exit_code) require(*value.exit_code >= 0, "negative exit code");
    if (value.term_signal) require(*value.term_signal > 0, "invalid term signal");
}
void validate(WorkerCancelTask& value, ValidationOptions) { positive(value.grace_ms); }
void validate(GetTaskResponse& value, ValidationOptions options) {
    validate(value.task, options); std::uint32_t previous = 0;
    for (const auto& attempt : value.attempts) {
        require(attempt.task_id == value.task.task_id, "Attempt belongs to another Task");
        require(attempt.attempt_no.value() > previous, "Attempts not ordered"); previous = attempt.attempt_no.value();
        if (attempt.exit_code) require(*attempt.exit_code >= 0, "negative exit code");
        if (attempt.term_signal) require(*attempt.term_signal > 0, "invalid term signal");
    }
}
// Type mapping is explicit, never derived from variant order or C++ object representation.
#define MESSAGES(X) \
 X(ErrorResponse) X(SubmitTask) X(SubmitTaskResponse) X(GetTask) X(GetTaskResponse) \
 X(ListTasks) X(ListTasksResponse) X(GetLogs) X(GetLogsResponse) X(CancelTask) X(CancelTaskResponse) \
 X(RetryTask) X(RetryTaskResponse) X(ListWorkers) X(ListWorkersResponse) X(RegisterWorker) X(RegisterAck) \
 X(Heartbeat) X(RunTask) X(RunTaskAck) X(TaskStarted) X(LogChunk) X(LogWindowUpdate) X(TaskFinished) \
 X(WorkerCancelTask) X(WorkerCancelTaskAck)
template <typename T> Type identity();
#define ID(T) template <> Type identity<T>() { return Type::T; }
MESSAGES(ID)
#undef ID
Message decode_object(Type type, const Json& object, ValidationOptions options) {
    switch (type) {
#define DECODE(T) case Type::T: { auto value = read<T>(object); validate(value, options); return Message{std::move(value)}; }
    MESSAGES(DECODE)
#undef DECODE
    }
    throw Invalid("reserved type");
}
#undef MESSAGES
Json parse_strict(std::string_view payload) {
    std::vector<std::set<std::string>> keys;
    bool duplicate = false;
    auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            if (!keys.back().insert(parsed.get<std::string>()).second) duplicate = true;
        } else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    auto object = Json::parse(payload.begin(), payload.end(), callback);
    // Syntax/UTF-8 faults take precedence even if an earlier key was duplicated.
    if (duplicate) throw Invalid("duplicate JSON key");
    return object;
}
} // namespace

Type message_type(const Message& message) { return std::visit([](const auto& value) { return identity<std::decay_t<decltype(value)>>(); }, message); }
ProtocolResult<Message> decode_message(const Frame& frame, Direction expected, ValidationOptions options) {
    const auto header = encode_header(frame.header, expected);
    if (!header) return ProtocolResult<Message>::failure(header.error().kind, header.error().message);
    if (frame.payload.size() != frame.header.payload_length) return ProtocolResult<Message>::failure(ErrorKind::Framing, "payload length mismatch");
    try {
        auto object = parse_strict(frame.payload);
        require(object.is_object(), "payload must be object");
        return ProtocolResult<Message>::success(decode_object(frame.header.type, object, options));
    } catch (const Invalid& error) { return ProtocolResult<Message>::failure(ErrorKind::Schema, error.what()); }
      catch (const Json::exception&) { return ProtocolResult<Message>::failure(ErrorKind::Syntax, "invalid UTF-8 or JSON"); }
}
ProtocolResult<Frame> encode_message(const Message& message, std::uint64_t request_id, Direction expected, ValidationOptions options) {
    try {
        const auto type = message_type(message);
        auto object = std::visit([](const auto& value) { return write(value); }, message);
        auto normalized = decode_object(type, object, options);
        auto payload = std::visit([](const auto& value) { return dump_bounded(write(value)); }, normalized);
        if (payload.size() > kMaxPayload) return ProtocolResult<Frame>::failure(ErrorKind::Framing, "encoded payload exceeds wire maximum");
        Frame frame{{type, request_id, static_cast<std::uint32_t>(payload.size())}, std::move(payload)};
        const auto header = encode_header(frame.header, expected);
        if (!header) return ProtocolResult<Frame>::failure(header.error().kind, header.error().message);
        return ProtocolResult<Frame>::success(std::move(frame));
    } catch (const Invalid& error) { return ProtocolResult<Frame>::failure(ErrorKind::Schema, error.what()); }
      catch (const Json::exception&) { return ProtocolResult<Frame>::failure(ErrorKind::Syntax, "invalid UTF-8 or JSON"); }
}
std::string base64_encode(std::string_view raw) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result; result.reserve(4 * ((raw.size() + 2) / 3));
    for (std::size_t i = 0; i < raw.size(); i += 3) {
        const auto a = static_cast<unsigned char>(raw[i]);
        const unsigned b = i + 1 < raw.size() ? static_cast<unsigned char>(raw[i + 1]) : 0U;
        const unsigned c = i + 2 < raw.size() ? static_cast<unsigned char>(raw[i + 2]) : 0U;
        result += alphabet[a >> 2U]; result += alphabet[((a & 3U) << 4U) | (b >> 4U)];
        result += i + 1 < raw.size() ? alphabet[((b & 15U) << 2U) | (c >> 6U)] : '=';
        result += i + 2 < raw.size() ? alphabet[c & 63U] : '=';
    }
    return result;
}
ProtocolResult<std::string> base64_decode(std::string_view text, std::size_t maximum_raw) {
    const auto fail = [] { return ProtocolResult<std::string>::failure(ErrorKind::Consistency, "invalid base64"); };
    if (text.size() % 4 != 0 || text.size() / 4 > (maximum_raw + 2) / 3) return fail();
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string raw; raw.reserve(std::min(text.size() / 4 * 3, maximum_raw));
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const int a = digit(text[i]), b = digit(text[i+1]);
        const int c = text[i+2] == '=' ? 0 : digit(text[i+2]);
        const int d4 = text[i+3] == '=' ? 0 : digit(text[i+3]);
        if (a < 0 || b < 0 || c < 0 || d4 < 0) return fail();
        const bool pad2 = text[i+2] == '=', pad1 = text[i+3] == '=';
        if ((pad2 && !pad1) || ((pad1 || pad2) && i + 4 != text.size()) ||
            (pad2 && (b & 15) != 0) || (pad1 && !pad2 && (c & 3) != 0)) return fail();
        raw += static_cast<char>((a << 2) | (b >> 4));
        if (!pad2) raw += static_cast<char>((b << 4) | (c >> 2));
        if (!pad1) raw += static_cast<char>((c << 6) | d4);
    }
    if (raw.size() > maximum_raw) return fail();
    return ProtocolResult<std::string>::success(std::move(raw));
}
ProtocolResult<LogContext> check_log(const LogChunk& chunk, LogContext context) {
    try { auto copy = chunk; validate(copy, {}); }
    catch (const Invalid& error) { return ProtocolResult<LogContext>::failure(ErrorKind::Consistency, error.what()); }
    auto& stream = chunk.stream == Stream::Stdout ? context.out : context.err;
    if (chunk.sequence != stream.next_sequence || context.credit_bytes < chunk.raw_size || context.credit_chunks == 0 ||
        stream.next_sequence == std::numeric_limits<std::uint64_t>::max() ||
        stream.bytes > std::numeric_limits<std::uint64_t>::max() - chunk.raw_size)
        return ProtocolResult<LogContext>::failure(ErrorKind::Consistency, "sequence, count or credit violation");
    ++stream.next_sequence; stream.bytes += chunk.raw_size;
    context.credit_bytes -= chunk.raw_size; --context.credit_chunks;
    return ProtocolResult<LogContext>::success(context);
}
ProtocolResult<bool> check_finish(const TaskFinished& finish, const LogContext& context) {
    if (finish.stdout_next_sequence != context.out.next_sequence || finish.stderr_next_sequence != context.err.next_sequence ||
        finish.stdout_bytes != context.out.bytes || finish.stderr_bytes != context.err.bytes)
        return ProtocolResult<bool>::failure(ErrorKind::Consistency, "terminal log counters mismatch");
    return ProtocolResult<bool>::success(true);
}
} // namespace ts::protocol
