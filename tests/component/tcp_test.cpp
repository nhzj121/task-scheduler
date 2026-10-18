#include "ts/net/connection_manager.h"
#include "ts/protocol/messages.h"
#include "ts/io/timer_fd.h"
#include "../helpers/protocol_fixtures.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ts::net {
namespace {
using Clock = std::chrono::steady_clock;
template <typename Predicate> void until(io::EventLoop& loop, Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!predicate() && Clock::now() < deadline) {
        const auto result = loop.poll_once(10); ASSERT_TRUE(result) << (result ? "" : result.error().message);
    }
    ASSERT_TRUE(predicate()) << "bounded event-loop deadline";
}
std::size_t fd_count() {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"),std::filesystem::directory_iterator{}));
}
std::uint64_t resident_bytes() {
    std::ifstream statm("/proc/self/statm"); std::uint64_t virtual_pages = 0, resident = 0;
    statm >> virtual_pages >> resident;
    if (!statm) throw std::runtime_error("cannot read memory diagnostic");
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) throw std::runtime_error("invalid page size");
    return resident * static_cast<std::uint64_t>(page_size);
}
TransportLimits small_limits() {
    TransportLimits limits; limits.maximum_frame_bytes = 16; limits.input_bytes = 64;
    limits.output = {{128,8},{128,8}}; return limits;
}
struct Harness {
    std::unique_ptr<io::EventLoop> loop{std::move(io::EventLoop::create()).value()};
    std::unique_ptr<ConnectionManager> manager;
    std::vector<ConnectionId> connected;
    std::vector<CloseEvent> closed;
    std::map<std::uint64_t,std::string> received;
    explicit Harness(TransportLimits limits = {}, SocketIo operations = {}) {
        manager = std::move(ConnectionManager::create(*loop,{
            [&](ConnectionId id) { connected.push_back(id); },
            [&](ConnectionId id,std::string_view bytes) { received[id.value].append(bytes); return bytes.size(); },
            [&](CloseEvent event) { closed.push_back(event); }
        },limits,std::move(operations))).value();
    }
    ConnectionId pair() {
        const auto listener = manager->listen({"127.0.0.1",0}).value();
        const auto client = manager->connect(manager->endpoint(listener).value()).value();
        until(*loop,[&] { return connected.size() == 2; });
        EXPECT_TRUE(manager->close_listener(listener)); return client;
    }
    ConnectionId other(ConnectionId id) const { for (auto item : connected) if (!(item == id)) return item; throw std::logic_error("no peer"); }
};
TEST(Tcp, NonblockingCloexecListenConnectAcceptAndInterest) {
    Harness harness; const auto listener = harness.manager->listen({"127.0.0.1",0}).value();
    const auto endpoint = harness.manager->endpoint(listener).value(); ASSERT_NE(endpoint.port,0);
    std::vector<ConnectionId> clients;
    for (int i = 0; i < 5; ++i) clients.push_back(harness.manager->connect(endpoint).value());
    until(*harness.loop,[&] { return harness.connected.size() == 10; });
    for (auto id : harness.connected) {
        const auto state = harness.manager->inspect(id); ASSERT_TRUE(state);
        EXPECT_NE(::fcntl(state->fd,F_GETFL) & O_NONBLOCK,0); EXPECT_NE(::fcntl(state->fd,F_GETFD) & FD_CLOEXEC,0);
        EXPECT_FALSE(state->connecting); EXPECT_EQ(state->interest & io::kWritable,0U);
    }
    ASSERT_TRUE(harness.manager->send(clients[0],{"hello"}));
    EXPECT_NE(harness.manager->inspect(clients[0])->interest & io::kWritable,0U);
    until(*harness.loop,[&] { return harness.manager->inspect(clients[0])->control_bytes == 0; });
    EXPECT_EQ(harness.manager->inspect(clients[0])->interest & io::kWritable,0U);
    until(*harness.loop,[&] { return !harness.received.empty(); });
    EXPECT_TRUE(harness.manager->close_listener(listener));
    EXPECT_FALSE(harness.manager->connect({"localhost",1}));
    EXPECT_FALSE(harness.manager->listen({"bad",0}));
}
TEST(Tcp, NumericIpv6LoopbackAndRegistrationFailureCleanup) {
    const auto before = fd_count();
    {
        Harness harness(small_limits()); auto listener = harness.manager->listen({"::1",0}); ASSERT_TRUE(listener);
        const auto endpoint = harness.manager->endpoint(listener.value()).value(); EXPECT_EQ(endpoint.address,"::1");
        const auto client = harness.manager->connect(endpoint).value();
        until(*harness.loop,[&] { return harness.connected.size() == 2; });
        const auto peer = harness.other(client); ASSERT_TRUE(harness.manager->send(client,{"ipv6"}));
        until(*harness.loop,[&] { return harness.received[peer.value] == "ipv6"; });
        const auto registrations = harness.loop->registration_count();
        EXPECT_FALSE(harness.manager->adopt(common::UniqueFd(::open("/dev/null",O_RDWR | O_CLOEXEC))));
        EXPECT_EQ(harness.loop->registration_count(),registrations); EXPECT_EQ(harness.manager->connection_count(),2U);
        auto invalid = small_limits(); invalid.input_bytes = 1; EXPECT_FALSE(ConnectionManager::create(*harness.loop,{},invalid));
    }
    EXPECT_EQ(fd_count(),before);
}
TEST(Tcp, DeterministicShortWritesEintrEagainAndFinishWireOrder) {
    std::string wire; unsigned calls = 0;
    SocketIo operations;
    operations.send = [&](int fd,const void* bytes,std::size_t size,int flags) -> ssize_t {
        ++calls; EXPECT_NE(flags & MSG_NOSIGNAL,0);
        if (calls % 4 == 1) { errno = EINTR; return -1; }
        if (calls % 4 == 2) { errno = EAGAIN; return -1; }
        const auto count = std::min(size,std::size_t{1});
        auto result = ::send(fd,bytes,count,flags);
        if (result > 0) wire.append(static_cast<const char*>(bytes),static_cast<std::size_t>(result));
        return result;
    };
    Harness harness({},operations); auto client = harness.pair(); auto peer = harness.other(client);
    ASSERT_TRUE(harness.manager->send(client,{"stdout",QueueClass::Log,OrderPoint{1,1},std::nullopt}));
    until(*harness.loop,[&] { return !wire.empty(); }); // Partial current log is now pinned.
    ASSERT_TRUE(harness.manager->send(client,{"stderr",QueueClass::Log,OrderPoint{1,2},std::nullopt}));
    ASSERT_TRUE(harness.manager->send(client,{"FINISH",QueueClass::Control,std::nullopt,OrderPoint{1,2}}));
    ASSERT_TRUE(harness.manager->send(client,{"ACK"}));
    until(*harness.loop,[&] { return harness.received[peer.value].size() == 21; });
    EXPECT_EQ(wire,"stdoutACKstderrFINISH"); EXPECT_EQ(harness.received[peer.value],wire);
}
TEST(Tcp, ReceiveEintrEagainBoundedAndPauseWithinCallback) {
    unsigned calls = 0; SocketIo operations;
    operations.recv = [&](int fd,void* bytes,std::size_t size,int flags) -> ssize_t {
        ++calls; if (calls % 3 == 1) { errno = EINTR; return -1; }
        if (calls % 3 == 2) { errno = EAGAIN; return -1; }
        return ::recv(fd,bytes,std::min(size,std::size_t{1}),flags);
    };
    Harness harness({},operations); auto client = harness.pair(); auto peer = harness.other(client);
    ASSERT_TRUE(harness.manager->set_read_enabled(peer,false)); ASSERT_TRUE(harness.manager->send(client,{"abcdef"}));
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(harness.loop->poll_once(0));
    EXPECT_TRUE(harness.received[peer.value].empty());
    ASSERT_TRUE(harness.manager->set_read_enabled(peer,true));
    until(*harness.loop,[&] { return harness.received[peer.value] == "abcdef"; });
}
TEST(Tcp, PausedPeerFinDoesNotSpinAndResumeDrainsBeforeEof) {
    Harness harness; const auto client = harness.pair(); const auto peer = harness.other(client);
    ASSERT_TRUE(harness.manager->set_read_enabled(peer,false));
    EXPECT_EQ(harness.manager->inspect(peer)->interest & (io::kReadable | io::kReadHangup),0U);
    ASSERT_TRUE(harness.manager->send(client,{"tail-before-fin"}));
    until(*harness.loop,[&] { return harness.manager->inspect(client)->control_bytes == 0; });
    ASSERT_TRUE(harness.manager->close(client));
    for (int i = 0; i < 10; ++i) { auto result = harness.loop->poll_once(0); ASSERT_TRUE(result); EXPECT_EQ(result.value(),0U); }
    EXPECT_TRUE(harness.received[peer.value].empty()); ASSERT_TRUE(harness.manager->set_read_enabled(peer,true));
    until(*harness.loop,[&] { return harness.closed.size() == 2; });
    EXPECT_EQ(harness.received[peer.value],"tail-before-fin"); EXPECT_EQ(harness.closed.back().cause,CloseCause::PeerEof);
}
TEST(Tcp, RealBackpressureBoundsAndNoFdLeak) {
    const auto before = fd_count();
    {
        Harness harness; const auto client = harness.pair(); const auto peer = harness.other(client);
        const int size = 4096; const int fd = harness.manager->inspect(client)->fd;
        ASSERT_EQ(::setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&size,sizeof(size)),0);
        ASSERT_TRUE(harness.manager->set_read_enabled(peer,false));
        const std::string payload(4U * 1024U * 1024U,'p');
        ASSERT_TRUE(harness.manager->send(client,{payload,QueueClass::Log,std::nullopt,std::nullopt}));
        for (int i = 0; i < 50; ++i) ASSERT_TRUE(harness.loop->poll_once(0));
        auto state = harness.manager->inspect(client); ASSERT_TRUE(state); EXPECT_EQ(state->log_bytes,payload.size()); EXPECT_NE(state->interest & io::kWritable,0U);
        ASSERT_TRUE(harness.manager->set_read_enabled(peer,true));
        until(*harness.loop,[&] { return harness.received[peer.value].size() == payload.size(); });
        EXPECT_EQ(harness.received[peer.value],payload); EXPECT_EQ(harness.manager->inspect(client)->log_bytes,0U);
    }
    EXPECT_EQ(fd_count(),before);
}
TEST(Tcp, OutputOverflowExplicitlyClosesAndDiagnosesOwnedBuffers) {
    Harness harness(small_limits()); auto client = harness.pair();
    ASSERT_TRUE(harness.manager->send(client,{std::string(128,'x'),QueueClass::Log,std::nullopt,std::nullopt}));
    EXPECT_FALSE(harness.manager->send(client,{"x",QueueClass::Log,std::nullopt,std::nullopt}));
    ASSERT_EQ(harness.closed.size(),1U); EXPECT_EQ(harness.closed[0].cause,CloseCause::OutputOverflow); EXPECT_EQ(harness.closed[0].discarded_log_bytes,128U);
    EXPECT_TRUE(harness.manager->inspect(client)->closing);
    ASSERT_TRUE(harness.loop->poll_once(0)); EXPECT_FALSE(harness.manager->inspect(client));
}
TEST(Tcp, ClosingPartialFrameDiagnosesOnlyUnsentBytes) {
    unsigned calls = 0; SocketIo operations;
    operations.send = [&](int fd,const void* bytes,std::size_t,int flags) -> ssize_t {
        if (++calls > 1) { errno = EAGAIN; return -1; }
        return ::send(fd,bytes,1,flags);
    };
    Harness harness({},operations); const auto client = harness.pair();
    ASSERT_TRUE(harness.manager->send(client,{"abcdef"})); until(*harness.loop,[&] { return calls >= 2; });
    EXPECT_EQ(harness.manager->inspect(client)->control_bytes,6U);
    ASSERT_TRUE(harness.manager->close(client)); ASSERT_EQ(harness.closed.size(),1U);
    EXPECT_EQ(harness.closed[0].discarded_control_bytes,5U);
}
TEST(Tcp, PeerFinAndResetDuringSendSuppressSigpipe) {
    Harness harness; const auto client = harness.pair(); const auto peer = harness.other(client);
    linger reset{1,0}; ASSERT_EQ(::setsockopt(harness.manager->inspect(peer)->fd,SOL_SOCKET,SO_LINGER,&reset,sizeof(reset)),0);
    ASSERT_TRUE(harness.manager->close(peer));
    ASSERT_TRUE(harness.manager->send(client,{std::string(1024,'x')}));
    until(*harness.loop,[&] { return harness.closed.size() == 2; });
    EXPECT_TRUE(harness.closed.back().cause == CloseCause::ReceiveError || harness.closed.back().cause == CloseCause::SendError || harness.closed.back().cause == CloseCause::PeerEof);
    SocketIo failed; failed.send = [](int,const void*,std::size_t,int flags) -> ssize_t { EXPECT_NE(flags & MSG_NOSIGNAL,0); errno = EPIPE; return -1; };
    Harness deterministic({},failed); auto id = deterministic.pair(); ASSERT_TRUE(deterministic.manager->send(id,{"x"}));
    until(*deterministic.loop,[&] { return !deterministic.closed.empty(); });
    EXPECT_EQ(deterministic.closed[0].cause,CloseCause::SendError); EXPECT_EQ(deterministic.closed[0].system_error,EPIPE);
}
TEST(Tcp, ConnectRefusalReportedBySoError) {
    Harness harness; auto listener = harness.manager->listen({"127.0.0.1",0}).value();
    const auto endpoint = harness.manager->endpoint(listener).value(); ASSERT_TRUE(harness.manager->close_listener(listener));
    auto connected = harness.manager->connect(endpoint);
    if (connected) { until(*harness.loop,[&] { return !harness.closed.empty(); }); EXPECT_EQ(harness.closed[0].cause,CloseCause::ConnectError); EXPECT_NE(harness.closed[0].system_error,0); }
    else EXPECT_FALSE(connected.error().message.empty());
}
TEST(Tcp, ChurnGenerationFdReuseAndDeferredOwnerDestruction) {
    const auto before = fd_count(); std::set<std::uint64_t> identities;
    {
        Harness harness(small_limits());
        const auto listener = harness.manager->listen({"127.0.0.1",0}).value(); const auto endpoint = harness.manager->endpoint(listener).value();
        std::optional<io::RegistrationToken> previous; int reused = 0; std::uint64_t warmed_resident = 0;
        for (int i = 0; i < 150; ++i) {
            harness.connected.clear(); auto client = harness.manager->connect(endpoint).value();
            until(*harness.loop,[&] { return harness.connected.size() == 2; });
            auto state = harness.manager->inspect(client); ASSERT_TRUE(state);
            if (previous && previous->fd == state->fd) { ++reused; EXPECT_NE(previous->key,state->registration.key); EXPECT_FALSE(harness.loop->is_current(*previous)); }
            previous = state->registration; EXPECT_TRUE(identities.insert(client.value).second);
            for (auto id : harness.connected) ASSERT_TRUE(harness.manager->close(id));
            EXPECT_EQ(harness.manager->connection_count(),2U); // Owners retained to batch safe point.
            ASSERT_TRUE(harness.loop->poll_once(0)); EXPECT_EQ(harness.manager->connection_count(),0U);
            if (i == 49) warmed_resident = resident_bytes();
        }
        EXPECT_GT(reused,0);
        EXPECT_LE(resident_bytes(),warmed_resident + 2U * 1024U * 1024U);
    }
    EXPECT_EQ(fd_count(),before);
}
TEST(Tcp, InputBoundAndCloseCallbackPrecedesUnregisterDeferredErase) {
    auto loop = std::move(io::EventLoop::create()).value();
    std::unique_ptr<ConnectionManager> manager; std::vector<ConnectionId> connected;
    std::optional<CloseEvent> closed; bool observed_owner = false;
    manager = std::move(ConnectionManager::create(*loop,{
        [&](ConnectionId id) { connected.push_back(id); },
        [](ConnectionId,std::string_view) { return std::size_t{0}; },
        [&](CloseEvent event) {
            closed = event; const auto state = manager->inspect(event.id);
            ASSERT_TRUE(state); observed_owner = state->closing && loop->is_current(state->registration) && ::fcntl(state->fd,F_GETFD) >= 0;
        }
    },small_limits())).value();
    const auto listener = manager->listen({"127.0.0.1",0}).value();
    const auto client = manager->connect(manager->endpoint(listener).value()).value();
    until(*loop,[&] { return connected.size() == 2; });
    ASSERT_TRUE(manager->send(client,{std::string(128,'x')}));
    until(*loop,[&] { return closed.has_value(); });
    EXPECT_TRUE(observed_owner); EXPECT_EQ(closed->cause,CloseCause::InputOverflow);
    EXPECT_FALSE(manager->inspect(closed->id));
}
TEST(Tcp, SelfCrossCloseAndSameBatchStaleEvents) {
    auto loop = std::move(io::EventLoop::create()).value();
    std::unique_ptr<ConnectionManager> manager; std::vector<ConnectionId> connected;
    std::size_t callbacks = 0; std::size_t close_events = 0;
    manager = std::move(ConnectionManager::create(*loop,{
        [&](ConnectionId id) { connected.push_back(id); },
        [&](ConnectionId id,std::string_view bytes) {
            ++callbacks;
            for (auto target : connected) EXPECT_TRUE(manager->close(target));
            EXPECT_TRUE(manager->inspect(id)->closing); EXPECT_EQ(manager->connection_count(),4U);
            return bytes.size();
        },
        [&](CloseEvent) { ++close_events; }
    },small_limits())).value();
    auto listener = manager->listen({"127.0.0.1",0}).value(); auto endpoint = manager->endpoint(listener).value();
    ASSERT_TRUE(manager->connect(endpoint)); ASSERT_TRUE(manager->connect(endpoint));
    until(*loop,[&] { return connected.size() == 4; });
    for (auto id : connected) ASSERT_TRUE(manager->send(id,{"x"}));
    until(*loop,[&] { return close_events == 4; });
    EXPECT_EQ(callbacks,1U); EXPECT_EQ(manager->connection_count(),0U);
}
TEST(Tcp, ThrowingCallbackStillUnregistersAndDefersOwnerSafely) {
    auto loop = std::move(io::EventLoop::create()).value(); std::vector<ConnectionId> connected;
    std::size_t closed = 0;
    auto manager = std::move(ConnectionManager::create(*loop,{
        [&](ConnectionId id) { connected.push_back(id); },
        [](ConnectionId,std::string_view) -> std::size_t { throw std::runtime_error("consumer failed"); },
        [&](CloseEvent event) { if (event.cause == CloseCause::CallbackError) ++closed; }
    },small_limits())).value();
    auto listener = manager->listen({"127.0.0.1",0}).value();
    auto client = manager->connect(manager->endpoint(listener).value()).value();
    until(*loop,[&] { return connected.size() == 2; }); ASSERT_TRUE(manager->send(client,{"x"}));
    bool failed = false;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (closed == 0 && Clock::now() < deadline) { if (!loop->poll_once(10)) failed = true; }
    EXPECT_EQ(closed,1U); EXPECT_TRUE(failed);
    manager.reset(); EXPECT_EQ(loop->registration_count(),0U); EXPECT_TRUE(loop->poll_once(0));
}
TEST(Tcp, DestructionWithQueuedRetireActionsAndOwnerThreadGuard) {
    Harness harness(small_limits()); const auto client = harness.pair();
    bool rejected = false;
    std::thread thread([&] { rejected = !harness.manager->send(client,{"x"}); }); thread.join();
    EXPECT_TRUE(rejected); ASSERT_TRUE(harness.manager->close(client));
    harness.manager.reset(); EXPECT_EQ(harness.loop->registration_count(),0U);
    EXPECT_TRUE(harness.loop->poll_once(0)); // Expired sentinel prevents deferred manager access.
}
TEST(Tcp, BoundedDispatchDoesNotStarveTimerAndPauseInsideConsumer) {
    auto loop = std::move(io::EventLoop::create()).value(); std::unique_ptr<ConnectionManager> manager;
    std::vector<ConnectionId> connected; std::size_t delivered = 0; bool paused = false;
    auto limits = small_limits(); limits.read_budget = 1; limits.write_budget = 64;
    manager = std::move(ConnectionManager::create(*loop,{
        [&](ConnectionId id) { connected.push_back(id); },
        [&](ConnectionId id,std::string_view bytes) {
            delivered += bytes.size(); if (!paused) { paused = true; EXPECT_TRUE(manager->set_read_enabled(id,false)); }
            return bytes.size();
        },{}
    },limits)).value();
    auto listener = manager->listen({"127.0.0.1",0}).value();
    auto client = manager->connect(manager->endpoint(listener).value()).value();
    until(*loop,[&] { return connected.size() == 2; });
    const auto peer = connected[0] == client ? connected[1] : connected[0];
    ASSERT_TRUE(manager->send(client,{std::string(128,'x')}));
    until(*loop,[&] { return paused; }); EXPECT_EQ(delivered,1U);
    for (int i = 0; i < 10; ++i) { ASSERT_TRUE(loop->poll_once(0)); }
    EXPECT_EQ(delivered,1U);
    auto timer = std::move(io::TimerFd::create()).value(); bool ticked = false;
    auto token = loop->add(timer.fd(),io::kReadable,[&](const io::RegistrationToken&,io::EventMask) { EXPECT_TRUE(timer.drain()); ticked = true; }).value();
    ASSERT_TRUE(timer.arm_once(std::chrono::nanoseconds(1))); ASSERT_TRUE(manager->set_read_enabled(peer,true));
    until(*loop,[&] { return ticked; }); EXPECT_LT(delivered,128U);
    until(*loop,[&] { return delivered == 128; }); EXPECT_TRUE(loop->remove(token));
}
TEST(TcpProtocol, RealLoopbackEverySplitMergeAndOutstandingCorrelation) {
    using namespace protocol;
    auto loop = std::move(io::EventLoop::create()).value();
    FrameParser parser(Direction::ClientToController); std::size_t total_bytes = 0, frames = 0;
    std::vector<ConnectionId> accepted;
    auto server = std::move(ConnectionManager::create(*loop,{
        [&](ConnectionId id) { accepted.push_back(id); },
        [&](ConnectionId,std::string_view bytes) {
            total_bytes += bytes.size();
            auto parsed = parser.feed(bytes,[&](Frame&& incoming) { EXPECT_TRUE(decode_message(incoming,Direction::ClientToController)); ++frames; return true; });
            EXPECT_TRUE(parsed); return bytes.size();
        },{}
    })).value();
    auto client = std::move(ConnectionManager::create(*loop,{})).value();
    auto listener = server->listen({"127.0.0.1",0}).value();
    const auto id = client->connect(server->endpoint(listener).value()).value();
    until(*loop,[&] { return accepted.size() == 1 && !client->inspect(id)->connecting; });
    auto fixture = test::fixtures[3]; auto payload = std::string(fixture.json);
    const auto bytes = encode_frame({{Type::GetTask,77,static_cast<std::uint32_t>(payload.size())},payload},Direction::ClientToController).value();
    for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
        const auto initial = total_bytes;
        if (cut > 0) { ASSERT_TRUE(client->send(id,{bytes.substr(0,cut)})); until(*loop,[&] { return total_bytes == initial + cut; }); }
        if (cut < bytes.size()) { ASSERT_TRUE(client->send(id,{bytes.substr(cut)})); }
        until(*loop,[&] { return total_bytes == initial + bytes.size(); });
    }
    EXPECT_EQ(frames,bytes.size() + 1);
    ASSERT_TRUE(client->send(id,{bytes + bytes + bytes.substr(0,21)}));
    until(*loop,[&] { return frames == bytes.size() + 3; }); EXPECT_EQ(parser.buffered_bytes(),21U);
    ASSERT_TRUE(client->send(id,{bytes.substr(21)})); until(*loop,[&] { return frames == bytes.size() + 4; });
    // Test-only outstanding request state: IDs are scoped to this connection, not idempotency keys.
    std::map<std::uint64_t,Type> outstanding;
    EXPECT_TRUE(outstanding.emplace(77,Type::GetTaskResponse).second); EXPECT_FALSE(outstanding.emplace(77,Type::GetTaskResponse).second);
    auto response_fixture = test::fixtures[4]; auto response_payload = std::string(response_fixture.json);
    Frame response{{response_fixture.type,77,static_cast<std::uint32_t>(response_payload.size())},response_payload};
    ASSERT_TRUE(decode_message(response,Direction::ControllerToClient));
    EXPECT_EQ(response_type(Type::GetTask),outstanding.at(response.header.request_id)); EXPECT_EQ(outstanding.erase(response.header.request_id),1U);
}
TEST(TcpProtocol, FinDuringHeaderPayloadAndAfterCompleteFrame) {
    using namespace protocol;
    const auto payload = std::string(test::fixtures[3].json);
    const auto wire = encode_frame({{Type::GetTask,1,static_cast<std::uint32_t>(payload.size())},payload},Direction::ClientToController).value();
    for (const auto count : {std::size_t{10},wire.size() - 1,wire.size()}) {
        auto loop = std::move(io::EventLoop::create()).value(); FrameParser parser(Direction::ClientToController);
        std::size_t delivered = 0, frames = 0; bool eof = false; bool accepted = false;
        auto server = std::move(ConnectionManager::create(*loop,{
            [&](ConnectionId) { accepted = true; },
            [&](ConnectionId,std::string_view bytes) { delivered += bytes.size(); EXPECT_TRUE(parser.feed(bytes,[&](Frame&&) { ++frames; return true; })); return bytes.size(); },
            [&](CloseEvent event) { EXPECT_EQ(event.cause,CloseCause::PeerEof); eof = true; }
        })).value();
        auto client = std::move(ConnectionManager::create(*loop,{})).value(); auto listener = server->listen({"127.0.0.1",0}).value();
        auto id = client->connect(server->endpoint(listener).value()).value(); until(*loop,[&] { return accepted; });
        ASSERT_TRUE(client->send(id,{wire.substr(0,count)})); until(*loop,[&] { return delivered == count; });
        ASSERT_TRUE(client->close(id)); until(*loop,[&] { return eof; });
        EXPECT_EQ(frames,count == wire.size() ? 1U : 0U); EXPECT_EQ(parser.buffered_bytes(),count == wire.size() ? 0U : count);
    }
}
TEST(TcpProtocol, CallerAdapterClassifiesSchemaVersusSyntaxAndWorkerViolations) {
    using namespace protocol;
    for (auto expected : {Direction::ClientToController,Direction::WorkerToController}) {
        const auto type = expected == Direction::ClientToController ? Type::ListTasks : Type::Heartbeat;
        auto semantic = decode_message({{type,is_event(type) ? 0U : 9U,2},"{}"},expected);
        ASSERT_FALSE(semantic); EXPECT_EQ(semantic.error().kind,ErrorKind::Schema);
        const bool client_invalid_argument = expected == Direction::ClientToController && semantic.error().kind == ErrorKind::Schema;
        EXPECT_EQ(client_invalid_argument,expected == Direction::ClientToController);
        auto syntax = decode_message({{type,is_event(type) ? 0U : 9U,1},"{"},expected);
        ASSERT_FALSE(syntax); EXPECT_EQ(syntax.error().kind,ErrorKind::Syntax);
    }
    const auto input = std::string(test::fixtures[3].json);
    auto wrong_role = decode_message({{Type::GetTask,9,static_cast<std::uint32_t>(input.size())},input},Direction::WorkerToController);
    ASSERT_FALSE(wrong_role); EXPECT_EQ(wrong_role.error().kind,ErrorKind::Unsupported);
}
TEST(TcpProtocol, ClientSemanticErrorReturnsCorrelatedTypedErrorOverLoopback) {
    using namespace protocol;
    auto loop = std::move(io::EventLoop::create()).value(); std::unique_ptr<ConnectionManager> server;
    FrameParser incoming(Direction::ClientToController), responses(Direction::ControllerToClient);
    bool accepted = false, got_error = false;
    server = std::move(ConnectionManager::create(*loop,{
        [&](ConnectionId) { accepted = true; },
        [&](ConnectionId id,std::string_view bytes) {
            EXPECT_TRUE(incoming.feed(bytes,[&](Frame&& request) {
                const auto decoded = decode_message(request,Direction::ClientToController);
                if (!decoded && decoded.error().kind == ErrorKind::Schema) {
                    Message error = ErrorResponse{ErrorCode::InvalidArgument,decoded.error().message,false,nlohmann::json::object()};
                    const auto reply = encode_message(error,request.header.request_id,Direction::ControllerToClient).value();
                    EXPECT_TRUE(server->send(id,{encode_frame(reply,Direction::ControllerToClient).value()}));
                } else ADD_FAILURE() << "expected client semantic error";
                return true;
            }));
            return bytes.size();
        },{}
    })).value();
    auto client = std::move(ConnectionManager::create(*loop,{{},
        [&](ConnectionId,std::string_view bytes) {
            EXPECT_TRUE(responses.feed(bytes,[&](Frame&& response) {
                EXPECT_EQ(response.header.request_id,987U);
                const auto decoded = decode_message(response,Direction::ControllerToClient); EXPECT_TRUE(decoded);
                if (decoded) { EXPECT_EQ(std::get<ErrorResponse>(decoded.value()).code,ErrorCode::InvalidArgument); }
                got_error = true; return true;
            })); return bytes.size();
        },{}
    })).value();
    const auto listener = server->listen({"127.0.0.1",0}).value();
    const auto id = client->connect(server->endpoint(listener).value()).value(); until(*loop,[&] { return accepted; });
    const std::string payload = R"({"page_size":0,"page_token":null})";
    ASSERT_TRUE(client->send(id,{encode_frame({{Type::ListTasks,987,static_cast<std::uint32_t>(payload.size())},payload},Direction::ClientToController).value()}));
    until(*loop,[&] { return got_error; });
}
TEST(TcpProtocol, MaximumWireFrameAcrossBoundedTcpAndParserBuffers) {
    using namespace protocol;
    auto loop = std::move(io::EventLoop::create()).value(); FrameParser parser(Direction::ClientToController);
    bool accepted = false, complete = false;
    auto server = std::move(ConnectionManager::create(*loop,{
        [&](ConnectionId) { accepted = true; },
        [&](ConnectionId,std::string_view bytes) {
            const auto result = parser.feed(bytes,[&](Frame&& incoming) {
                EXPECT_EQ(incoming.payload.size(),kMaxPayload); EXPECT_TRUE(decode_message(incoming,Direction::ClientToController));
                complete = true; return true;
            }); EXPECT_TRUE(result); EXPECT_LE(parser.buffered_bytes(),kHeaderSize + kMaxPayload); return bytes.size();
        },{}
    })).value();
    auto client = std::move(ConnectionManager::create(*loop,{})).value(); auto listener = server->listen({"127.0.0.1",0}).value();
    const auto id = client->connect(server->endpoint(listener).value()).value(); until(*loop,[&] { return accepted; });
    auto payload = std::string(kMaxPayload - 2,' '); payload += "{}";
    auto wire = encode_frame({{Type::ListWorkers,11,kMaxPayload},std::move(payload)},Direction::ClientToController).value();
    ASSERT_TRUE(client->send(id,{std::move(wire)})); until(*loop,[&] { return complete; });
    EXPECT_EQ(client->inspect(id)->control_bytes,0U); EXPECT_EQ(parser.buffered_bytes(),0U);
}
TEST(TcpProtocol, EncodedLogFramesFinishBarrierAcrossShortSocketWrites) {
    using namespace protocol;
    using Json = nlohmann::json;
    auto loop = std::move(io::EventLoop::create()).value(); FrameParser parser(Direction::WorkerToController);
    std::vector<Type> delivered; std::unique_ptr<ConnectionManager> receiver;
    bool accepted = false;
    LogContext context{{},{},12,2};
    receiver = std::move(ConnectionManager::create(*loop,{
        [&](ConnectionId) { accepted = true; },
        [&](ConnectionId,std::string_view bytes) {
            EXPECT_TRUE(parser.feed(bytes,[&](Frame&& incoming) {
                const auto decoded = decode_message(incoming,Direction::WorkerToController); EXPECT_TRUE(decoded);
                if (!decoded) return false;
                delivered.push_back(incoming.header.type);
                if (incoming.header.type == Type::LogChunk) {
                    auto checked = check_log(std::get<LogChunk>(decoded.value()),context); EXPECT_TRUE(checked);
                    if (checked) context = checked.value();
                } else if (incoming.header.type == Type::TaskFinished) { EXPECT_TRUE(check_finish(std::get<TaskFinished>(decoded.value()),context)); }
                return true;
            })); return bytes.size();
        },{}
    })).value();
    std::size_t send_calls = 0, sent = 0; SocketIo operations;
    operations.send = [&](int fd,const void* bytes,std::size_t count,int flags) -> ssize_t {
        ++send_calls;
        if (send_calls % 5 == 0) { errno = EAGAIN; return -1; }
        auto result = ::send(fd,bytes,std::min(count,std::size_t{7}),flags);
        if (result > 0) sent += static_cast<std::size_t>(result);
        return result;
    };
    auto sender = std::move(ConnectionManager::create(*loop,{}, {},operations)).value(); auto listener = receiver->listen({"127.0.0.1",0}).value();
    const auto id = sender->connect(receiver->endpoint(listener).value()).value(); until(*loop,[&] { return accepted; });
    const auto bytes_for = [](Type type, Json json) {
        auto payload = json.dump(); Frame frame{{type,0,static_cast<std::uint32_t>(payload.size())},std::move(payload)};
        EXPECT_TRUE(decode_message(frame,Direction::WorkerToController));
        return encode_frame(frame,Direction::WorkerToController).value();
    };
    const auto log_json = Json::parse(test::fixtures[21].json); auto stderr_json = log_json; stderr_json["stream"] = "stderr";
    auto finished_json = Json::parse(test::fixtures[23].json); finished_json["stderr_next_sequence"] = 1; finished_json["stderr_bytes"] = 6;
    ASSERT_TRUE(sender->send(id,{bytes_for(Type::LogChunk,log_json),QueueClass::Log,OrderPoint{123,1},std::nullopt}));
    until(*loop,[&] { return sent > 0; }); ASSERT_TRUE(delivered.empty()); // First frame is genuinely partial.
    ASSERT_TRUE(sender->send(id,{bytes_for(Type::LogChunk,stderr_json),QueueClass::Log,OrderPoint{123,2},std::nullopt}));
    ASSERT_TRUE(sender->send(id,{bytes_for(Type::TaskFinished,finished_json),QueueClass::Control,std::nullopt,OrderPoint{123,2}}));
    ASSERT_TRUE(sender->send(id,{bytes_for(Type::Heartbeat,Json::parse(test::fixtures[17].json))}));
    until(*loop,[&] { return delivered.size() == 4; });
    EXPECT_EQ(delivered,(std::vector<Type>{Type::LogChunk,Type::Heartbeat,Type::LogChunk,Type::TaskFinished}));
    EXPECT_EQ(context.credit_bytes,0U); EXPECT_EQ(context.credit_chunks,0U);
}
TEST(TcpProtocol, MalformedAndWorkerSchemaFaultsCloseOnlyTheirOwnTransport) {
    using namespace protocol;
    for (int scenario = 0; scenario < 4; ++scenario) {
        const auto expected = scenario == 0 ? Direction::ClientToController : Direction::WorkerToController;
        auto loop = std::move(io::EventLoop::create()).value(); std::unique_ptr<ConnectionManager> server;
        std::map<std::uint64_t,std::unique_ptr<FrameParser>> parsers; std::size_t closed = 0, valid = 0;
        server = std::move(ConnectionManager::create(*loop,{
            [&](ConnectionId id) { parsers.emplace(id.value,std::make_unique<FrameParser>(expected)); },
            [&](ConnectionId id,std::string_view bytes) {
                const auto result = parsers.at(id.value)->feed(bytes,[&](Frame&& incoming) {
                    const auto decoded = decode_message(incoming,expected); if (!decoded) return false;
                    ++valid; return true;
                });
                if (!result) { EXPECT_TRUE(server->close(id)); }
                return bytes.size();
            },[&](CloseEvent) { ++closed; }
        })).value();
        auto client = std::move(ConnectionManager::create(*loop,{})).value(); auto listener = server->listen({"127.0.0.1",0}).value();
        const auto endpoint = server->endpoint(listener).value(); const auto bad = client->connect(endpoint).value(); const auto good = client->connect(endpoint).value();
        until(*loop,[&] { return parsers.size() == 2; });
        const auto type = scenario == 0 ? Type::GetTask : Type::Heartbeat;
        const auto payload = scenario <= 1 ? std::string("{") : std::string("{}");
        auto malformed = encode_frame({{type,type == Type::GetTask ? 7U : 0U,static_cast<std::uint32_t>(payload.size())},payload},direction(type)).value();
        if (scenario == 3) { malformed[6] = 1; malformed[7] = 3; } // Wrong role, not a JSON/schema fault.
        ASSERT_TRUE(client->send(bad,{malformed}));
        auto fixture = scenario == 0 ? test::fixtures[3] : test::fixtures[17];
        auto good_payload = std::string(fixture.json);
        ASSERT_TRUE(client->send(good,{encode_frame({{fixture.type,scenario == 0 ? 8U : 0U,static_cast<std::uint32_t>(good_payload.size())},good_payload},expected).value()}));
        until(*loop,[&] { return closed == 1 && valid == 1; });
        EXPECT_EQ(server->connection_count(),1U);
    }
}
} // namespace
} // namespace ts::net
