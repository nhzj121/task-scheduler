#include "ts/protocol/messages.h"
#include <gtest/gtest.h>
namespace ts::protocol {
TEST(M10Protocol, RepeatedLateFieldRejectionRetainsDiagnosticsAndReleasesDecodedOwners) {
    // Long strings, vector and map are allocated before the final invalid field.
    // The instrumented run must release all of them on every rejection.
    nlohmann::json payload{{"executable", "/bin/" + std::string(300, 'x')},
        {"args", {std::string(600, 'a'), std::string(700, 'b')}},
        {"env", {{"LONG_VALUE", std::string(1000, 'e')}}}, {"cwd", "/tmp"},
        {"resources", {{"cpu_slots", 1}, {"memory_mb", 0}}}, {"timeout_ms", "wrong-type"}};
    const auto wire = payload.dump();
    const Frame input{{Type::SubmitTask, 1, static_cast<std::uint32_t>(wire.size())}, wire};
    for (int index = 0; index < 1000; ++index) {
        const auto result = decode_message(input, Direction::ClientToController);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().kind, ErrorKind::Schema);
        EXPECT_EQ(result.error().message, "timeout_ms: expected integer");
    }
}
} // namespace ts::protocol
