#include "ts/protocol/messages.h"
#include "../helpers/protocol_fixtures.h"
#include <gtest/gtest.h>
#include <limits>
#include <set>

namespace ts::protocol {
namespace {
using Json = nlohmann::json;
Frame frame(Type type, std::string payload) {
    return {{type, is_event(type) ? 0U : 0x0123456789abcdefULL, static_cast<std::uint32_t>(payload.size())}, std::move(payload)};
}
Json fixture(Type type) {
    for (const auto& item : test::fixtures) if (item.type == type) return Json::parse(item.json);
    throw std::logic_error("missing fixture");
}
ProtocolResult<Message> decode(Type type, const Json& payload) { return decode_message(frame(type, payload.dump()), direction(type)); }
class MessageGolden : public testing::TestWithParam<test::Fixture> {};
TEST_P(MessageGolden, IndependentFixtureRoundTripAndRoleCorrelation) {
    const auto item = GetParam(); const auto input = frame(item.type, std::string(item.json));
    const auto decoded = decode_message(input, direction(item.type));
    ASSERT_TRUE(decoded) << (decoded ? "" : decoded.error().message);
    EXPECT_EQ(message_type(decoded.value()), item.type);
    const auto encoded = encode_message(decoded.value(), input.header.request_id, direction(item.type));
    ASSERT_TRUE(encoded); EXPECT_EQ(Json::parse(encoded.value().payload), Json::parse(item.json));
    EXPECT_EQ(encoded.value().header.request_id, input.header.request_id);
    for (auto role : {Direction::ClientToController, Direction::ControllerToClient, Direction::WorkerToController, Direction::ControllerToWorker})
        if (role != direction(item.type)) { EXPECT_FALSE(decode_message(input, role)); }
    auto wrong_id = input; wrong_id.header.request_id = is_event(item.type) ? 1 : 0;
    if (item.type != Type::ErrorResponse) { EXPECT_FALSE(decode_message(wrong_id, direction(item.type))); }
}
INSTANTIATE_TEST_SUITE_P(All26, MessageGolden, testing::ValuesIn(test::fixtures),
    [](const testing::TestParamInfo<test::Fixture>& parameter) { return "Id" + std::to_string(static_cast<std::uint16_t>(parameter.param.type)); });

TEST(Messages, EveryRequiredAndUnknownField) {
    for (const auto& item : test::fixtures) {
        auto json = Json::parse(item.json); auto extra = json; extra["unknown"] = 1;
        EXPECT_FALSE(decode(item.type, extra));
        for (auto it = json.begin(); it != json.end(); ++it) {
            auto missing = json; missing.erase(it.key());
            const bool optional = (item.type == Type::SubmitTask && (it.key() == "env" || it.key() == "cwd" || it.key() == "resources" || it.key() == "timeout_ms")) ||
                                  (item.type == Type::ListTasks && it.key() == "page_size") || (item.type == Type::GetLogs && it.key() == "limit_bytes");
            EXPECT_EQ(static_cast<bool>(decode(item.type, missing)), optional) << static_cast<int>(item.type) << ':' << it.key();
            auto wrong = json; wrong[it.key()] = Json::array({true, false});
            EXPECT_FALSE(decode(item.type, wrong)) << static_cast<int>(item.type) << ':' << it.key();
        }
    }
}
TEST(Messages, IndependentFrozenNumericAndDirectionTable) {
    const std::array<std::uint16_t,26> ids{1,0x101,0x102,0x103,0x104,0x105,0x106,0x107,0x108,0x109,0x10a,0x10b,0x10c,0x10d,0x10e,0x201,0x202,0x203,0x204,0x205,0x206,0x207,0x208,0x209,0x20a,0x20b};
    using D = Direction;
    const std::array roles{D::ControllerToClient,D::ClientToController,D::ControllerToClient,D::ClientToController,D::ControllerToClient,
        D::ClientToController,D::ControllerToClient,D::ClientToController,D::ControllerToClient,D::ClientToController,D::ControllerToClient,
        D::ClientToController,D::ControllerToClient,D::ClientToController,D::ControllerToClient,
        D::WorkerToController,D::ControllerToWorker,D::WorkerToController,D::ControllerToWorker,D::WorkerToController,
        D::WorkerToController,D::WorkerToController,D::ControllerToWorker,D::WorkerToController,D::ControllerToWorker,D::WorkerToController};
    const std::set<std::uint16_t> events{0x203,0x206,0x207,0x208,0x209};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint16_t>(test::fixtures[i].type),ids[i]); EXPECT_EQ(direction(test::fixtures[i].type),roles[i]);
        EXPECT_EQ(is_event(test::fixtures[i].type),events.count(ids[i]) != 0);
        auto header = encode_header({test::fixtures[i].type,events.count(ids[i]) != 0 ? 0U : 7U,2},roles[i]); ASSERT_TRUE(header);
        EXPECT_EQ(header.value()[6],ids[i] >> 8U); EXPECT_EQ(header.value()[7],ids[i] & 255U);
    }
}
TEST(Messages, DuplicatesIncludingNestedEnvironmentAndDetails) {
    for (const auto& text : {R"({"page_size":100,"page_size":200,"page_token":null})",
                            R"({"page_size":100,"page_token":null,"page_token":null})"}) {
        auto result = decode_message(frame(Type::ListTasks, text), Direction::ClientToController);
        ASSERT_FALSE(result); EXPECT_EQ(result.error().kind, ErrorKind::Schema);
    }
    auto submit = fixture(Type::SubmitTask).dump();
    const auto begin = submit.find("\"MODE\":\"test\"");
    submit.replace(begin, 13, "\"MODE\":\"test\",\"MODE\":\"other\"");
    EXPECT_FALSE(decode_message(frame(Type::SubmitTask, submit), Direction::ClientToController));
    auto error = R"({"code":"INTERNAL","message":"bad","retryable":false,"details":{"nested":{"x":1,"x":2}}})";
    EXPECT_FALSE(decode_message(frame(Type::ErrorResponse, error), Direction::ControllerToClient));
}
TEST(Messages, SyntaxUtf8TrailingDocumentsAndBoundedDiagnostic) {
    for (const auto& text : {std::string("{"), std::string("{}{}"), std::string("{\"x\":\"") + char(0xff) + "\"}", std::string("{\"page_size\":NaN}"), std::string("{\"x\":1,\"x\":2," )}) {
        const auto result = decode_message(frame(Type::ListTasks, text), Direction::ClientToController);
        ASSERT_FALSE(result); EXPECT_EQ(result.error().kind, ErrorKind::Syntax); EXPECT_LE(result.error().message.size(), 256U);
    }
    EXPECT_FALSE(decode(Type::ListTasks, Json::array()));
    std::string deep(70, '['); deep += '0'; deep += std::string(70, ']');
    EXPECT_FALSE(decode_message(frame(Type::ListTasks, deep), Direction::ClientToController));
}
TEST(Messages, ApprovedRegistrationDisjointVariants) {
    auto accepted = fixture(Type::RegisterAck);
    auto rejected = Json{{"accepted",false},{"error_code","REJECTED"},{"message","capacity"}};
    auto decoded = decode(Type::RegisterAck, rejected); ASSERT_TRUE(decoded);
    EXPECT_TRUE(std::holds_alternative<RegistrationRejected>(std::get<RegisterAck>(decoded.value()).result));
    EXPECT_EQ(Json::parse(encode_message(decoded.value(), 7, Direction::ControllerToWorker).value().payload), rejected);
    for (auto it = accepted.begin(); it != accepted.end(); ++it) {
        if (it.key() == "accepted") continue;
        auto extra = rejected; extra[it.key()] = it.value(); EXPECT_FALSE(decode(Type::RegisterAck, extra));
    }
    for (const auto& key : {"error_code", "message"}) {
        auto missing = rejected; missing.erase(key); EXPECT_FALSE(decode(Type::RegisterAck, missing));
        auto null = rejected; null[key] = nullptr; EXPECT_FALSE(decode(Type::RegisterAck, null));
        auto extra = accepted; extra[key] = "bad"; EXPECT_FALSE(decode(Type::RegisterAck, extra));
    }
    accepted["controller_instance_id"] = nullptr; EXPECT_FALSE(decode(Type::RegisterAck, accepted));
}
TEST(Messages, ApprovedOfflineNullableRepresentation) {
    auto json = fixture(Type::ListWorkersResponse); auto& worker = json["workers"][0];
    worker["state"] = "OFFLINE"; EXPECT_FALSE(decode(Type::ListWorkersResponse,json));
    worker["session_id"] = nullptr; worker["max_concurrent_tasks"] = nullptr;
    ASSERT_TRUE(decode(Type::ListWorkersResponse,json));
    worker["last_heartbeat_at"] = nullptr; ASSERT_TRUE(decode(Type::ListWorkersResponse,json));
    auto missing = json; missing["workers"][0].erase("last_heartbeat_at"); EXPECT_FALSE(decode(Type::ListWorkersResponse, missing));
    worker["state"] = "HEALTHY"; EXPECT_FALSE(decode(Type::ListWorkersResponse,json));
}
TEST(Messages, ApprovedPaginationDefaultsCapsInvalidAndOverflow) {
    auto list = fixture(Type::ListTasks); list.erase("page_size");
    ASSERT_TRUE(decode(Type::ListTasks, list)); EXPECT_EQ(std::get<ListTasks>(decode(Type::ListTasks, list).value()).page_size,100U);
    auto logs = fixture(Type::GetLogs); logs.erase("limit_bytes");
    EXPECT_EQ(std::get<GetLogs>(decode(Type::GetLogs,logs).value()).limit_bytes,262144U);
    for (std::uint64_t value : {1ULL,200ULL,201ULL,0xffffffffffffffffULL}) {
        list["page_size"] = value; auto result = decode(Type::ListTasks,list); ASSERT_TRUE(result);
        EXPECT_EQ(std::get<ListTasks>(result.value()).page_size,std::min(value,std::uint64_t{200}));
        logs["limit_bytes"] = value; auto log_result = decode(Type::GetLogs,logs); ASSERT_TRUE(log_result);
        EXPECT_EQ(std::get<GetLogs>(log_result.value()).limit_bytes,std::min(value,kMaxLogPage));
    }
    for (const Json& value : {Json(0),Json(-1),Json(1.0),Json(true),Json("200"),Json(nullptr),Json(1e30)}) {
        list["page_size"] = value; logs["limit_bytes"] = value;
        EXPECT_FALSE(decode(Type::ListTasks,list)); EXPECT_FALSE(decode(Type::GetLogs,logs));
    }
    EXPECT_FALSE(decode_message(frame(Type::ListTasks,R"({"page_size":18446744073709551616,"page_token":null})"),Direction::ClientToController));
}
TEST(Messages, MaximumLogPageEnvelopeAndSnapshotBounds) {
    auto json = fixture(Type::GetLogsResponse);
    json["attempt_no"] = std::numeric_limits<std::uint32_t>::max();
    json["offset"] = std::numeric_limits<std::uint64_t>::max() - kMaxLogPage;
    json["next_offset"] = std::numeric_limits<std::uint64_t>::max(); json["snapshot_end"] = json["next_offset"];
    json["data_base64"] = base64_encode(std::string(static_cast<std::size_t>(kMaxLogPage),'x'));
    auto result = decode(Type::GetLogsResponse,json); ASSERT_TRUE(result);
    auto encoded = encode_message(result.value(),7,Direction::ControllerToClient); ASSERT_TRUE(encoded);
    EXPECT_LE(encoded.value().payload.size(),kMaxPayload); EXPECT_LE(encoded.value().payload.size() - json["data_base64"].get<std::string>().size(),1024U);
    json["eof_snapshot"] = false; EXPECT_FALSE(decode(Type::GetLogsResponse,json));
    json = fixture(Type::GetLogsResponse); json["next_offset"] = 5; EXPECT_FALSE(decode(Type::GetLogsResponse,json));
}
TEST(Messages, SubmitDefaultsAndExecutionValidation) {
    auto json = Json{{"executable","/bin/true"},{"args",Json::array()}};
    auto result = decode(Type::SubmitTask,json); ASSERT_TRUE(result);
    const auto& task = std::get<SubmitTask>(result.value());
    EXPECT_TRUE(task.env.empty()); EXPECT_FALSE(task.cwd); EXPECT_FALSE(task.timeout_ms); EXPECT_EQ(task.resources.cpu_slots(),1U); EXPECT_EQ(task.resources.memory_mb(),0U);
    for (const auto& key : {"executable","args","env","resources"}) { auto invalid = fixture(Type::SubmitTask); invalid[key] = nullptr; EXPECT_FALSE(decode(Type::SubmitTask, invalid)); }
    for (const auto& key : {"cwd","timeout_ms"}) { auto valid = fixture(Type::SubmitTask); valid[key] = nullptr; EXPECT_TRUE(decode(Type::SubmitTask, valid)); }
    for (const auto& invalid : {Json{{"executable",""}},Json{{"args",Json::array({1})}},Json{{"env",{{"BAD=KEY","x"}}}},Json{{"timeout_ms",0}},Json{{"executable",std::string("x\0y",3)}}}) {
        auto input = fixture(Type::SubmitTask); input.update(invalid); EXPECT_FALSE(decode(Type::SubmitTask,input));
    }
    const auto input = frame(Type::SubmitTask,fixture(Type::SubmitTask).dump());
    EXPECT_FALSE(decode_message(input,direction(Type::SubmitTask),{100}));
}
TEST(Messages, DomainNumericNestedAndEnumValidation) {
    auto json = fixture(Type::RunTask);
    for (const auto& key : {"worker_id","session_id","task_id","attempt_id"}) { auto bad = json; bad[key] = ""; EXPECT_FALSE(decode(Type::RunTask,bad)) << key; }
    for (const Json& n : {Json(0),Json(-1),Json(1.5),Json(4294967296ULL)}) { auto bad = json; bad["attempt_no"] = n; EXPECT_FALSE(decode(Type::RunTask,bad)); }
    for (const Json& resource : {Json{{"cpu_slots",0},{"memory_mb",0}},Json{{"cpu_slots",1},{"memory_mb",-1}},Json{{"cpu_slots",1.0},{"memory_mb",0}},Json{{"cpu_slots",1},{"memory_mb",0},{"extra",0}}}) {
        auto bad = json; bad["resources"] = resource; EXPECT_FALSE(decode(Type::RunTask,bad));
    }
    auto task = fixture(Type::GetTaskResponse); task["task"]["created_at"] = "2026-09-26"; EXPECT_FALSE(decode(Type::GetTaskResponse,task));
    task = fixture(Type::GetTaskResponse); task["attempts"][0]["extra"] = true; EXPECT_FALSE(decode(Type::GetTaskResponse,task));
    for (auto state : {"PENDING","ACTIVE","SUCCEEDED","FAILED","CANCELLED"}) { task = fixture(Type::GetTaskResponse); task["task"]["state"] = state; EXPECT_TRUE(decode(Type::GetTaskResponse,task)); }
    for (auto state : {"ASSIGNED","STARTING","RUNNING","SUCCEEDED","FAILED","TIMED_OUT","CANCELLED","LOST"}) { task = fixture(Type::GetTaskResponse); task["attempts"][0]["state"] = state; EXPECT_TRUE(decode(Type::GetTaskResponse,task)); }
    task["attempts"][0]["state"] = "FINISHED"; EXPECT_FALSE(decode(Type::GetTaskResponse,task));
    auto error = fixture(Type::ErrorResponse);
    for (auto code : {"INVALID_ARGUMENT","NOT_FOUND","CONFLICT","UNSUPPORTED","TEMPORARILY_UNAVAILABLE","INTERNAL"}) { error["code"] = code; EXPECT_TRUE(decode(Type::ErrorResponse,error)); }
    error["code"] = "UNKNOWN"; EXPECT_FALSE(decode(Type::ErrorResponse,error));
}
TEST(Messages, BinaryBase64CanonicalPaddingAndChunkBounds) {
    std::string binary; for (unsigned i = 0; i < 256; ++i) binary += static_cast<char>(i);
    EXPECT_EQ(base64_decode(base64_encode(binary),256).value(),binary);
    for (auto bad : {"A","A===","=AAA","AA=A","AA==AAAA","AB==","AAB=","AA-_","AA\n="}) EXPECT_FALSE(base64_decode(bad,65536));
    auto chunk = fixture(Type::LogChunk);
    for (const auto size : {1U,65536U}) { chunk["raw_size"] = size; chunk["data_base64"] = base64_encode(std::string(size,'x')); EXPECT_TRUE(decode(Type::LogChunk,chunk)); }
    for (const auto size : {0U,65537U}) { chunk["raw_size"] = size; chunk["data_base64"] = base64_encode(std::string(size,'x')); EXPECT_FALSE(decode(Type::LogChunk,chunk)); }
    chunk = fixture(Type::LogChunk); chunk["raw_size"] = 5; EXPECT_FALSE(decode(Type::LogChunk,chunk));
    auto window = fixture(Type::LogWindowUpdate); window["grant_bytes"] = 0; window["grant_chunks"] = 0; EXPECT_FALSE(decode(Type::LogWindowUpdate,window));
    window["grant_chunks"] = 1; EXPECT_TRUE(decode(Type::LogWindowUpdate,window));
    window["grant_bytes"] = -1; EXPECT_FALSE(decode(Type::LogWindowUpdate,window));
}
TEST(Messages, PureSequenceCreditAndFinalCounterChecks) {
    auto chunk = std::get<LogChunk>(decode(Type::LogChunk,fixture(Type::LogChunk)).value());
    LogContext context{{},{},12,2};
    EXPECT_FALSE(check_log(chunk,{{},{},5,2})); EXPECT_FALSE(check_log(chunk,{{},{},100,0}));
    auto next = check_log(chunk,context); ASSERT_TRUE(next); EXPECT_EQ(next.value().out.bytes,6U); EXPECT_EQ(next.value().credit_chunks,1U);
    EXPECT_FALSE(check_log(chunk,next.value())); chunk.sequence = 2; EXPECT_FALSE(check_log(chunk,next.value()));
    chunk.sequence = 0; chunk.stream = Stream::Stderr; auto both = check_log(chunk,next.value()); ASSERT_TRUE(both);
    EXPECT_EQ(both.value().err.next_sequence,1U); EXPECT_EQ(both.value().credit_bytes,0U);
    auto finished = std::get<TaskFinished>(decode(Type::TaskFinished,fixture(Type::TaskFinished)).value());
    EXPECT_TRUE(check_finish(finished,next.value())); EXPECT_FALSE(check_finish(finished,both.value()));
    chunk.stream = Stream::Stdout; chunk.sequence = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(check_log(chunk,{{chunk.sequence,0},{},100,1}));
}
TEST(Messages, NullableFieldsAndAllResultTokens) {
    auto task = fixture(Type::GetTaskResponse);
    for (const auto& key : {"worker_id","session_id","assigned_at","started_at","finished_at","exit_code","term_signal","reason"}) task["attempts"][0][key] = nullptr;
    task["task"]["latest_attempt_id"] = nullptr; task["task"]["cwd"] = nullptr; task["task"]["timeout_ms"] = nullptr;
    EXPECT_TRUE(decode(Type::GetTaskResponse,task));
    auto logs = fixture(Type::GetLogs); logs["attempt_no"] = nullptr; EXPECT_TRUE(decode(Type::GetLogs,logs));
    auto finish = fixture(Type::TaskFinished);
    for (auto outcome : {"EXITED","SIGNALED","TIMED_OUT","CANCELLED","LAUNCH_FAILED"}) {
        finish["outcome"] = outcome; finish["exit_code"] = nullptr; finish["term_signal"] = nullptr;
        if (std::string_view(outcome) == "EXITED") finish["exit_code"] = 0;
        if (std::string_view(outcome) == "SIGNALED") finish["term_signal"] = 15;
        EXPECT_TRUE(decode(Type::TaskFinished,finish));
    }
    finish["outcome"] = "EXITED"; finish["exit_code"] = nullptr; EXPECT_FALSE(decode(Type::TaskFinished,finish));
    finish["outcome"] = "SIGNALED"; finish["term_signal"] = nullptr; EXPECT_FALSE(decode(Type::TaskFinished,finish));
    finish["outcome"] = "LOST"; EXPECT_FALSE(decode(Type::TaskFinished,finish));
    auto cancel = fixture(Type::CancelTaskResponse);
    for (auto status : {"accepted","completed","already_cancelled"}) { cancel["status"] = status; EXPECT_TRUE(decode(Type::CancelTaskResponse,cancel)); }
    cancel["status"] = "success"; EXPECT_FALSE(decode(Type::CancelTaskResponse,cancel));
}
TEST(Messages, DeepDiagnosticDetailsAreBoundedWithoutRecursiveCopyOrDump) {
    std::string nested;
    for (int i = 0; i < 2000; ++i) nested += "{\"x\":";
    nested += "{}"; nested += std::string(2000,'}');
    const auto payload = std::string(R"({"code":"INTERNAL","message":"deep","retryable":false,"details":)") + nested + "}";
    const auto decoded = decode_message(frame(Type::ErrorResponse,payload),Direction::ControllerToClient);
    ASSERT_TRUE(decoded); const auto encoded = encode_message(decoded.value(),7,Direction::ControllerToClient); ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded.value().payload.size(),payload.size());
    EXPECT_TRUE(decode_message(encoded.value(),Direction::ControllerToClient));
}
} // namespace
} // namespace ts::protocol
