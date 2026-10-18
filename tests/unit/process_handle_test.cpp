#include "ts/runtime/process_handle.h"

#include <csignal>

#include <sys/wait.h>

#include <gtest/gtest.h>

namespace ts::runtime {
namespace {

TEST(ProcessHandle, FirstTerminationCauseWinsAndNaturalOutcomeCannotBeRewritten) {
    ProcessHandle terminated(10, 10, true);
    EXPECT_TRUE(terminated.install_termination_cause(TerminationCause::TIMEOUT));
    EXPECT_FALSE(terminated.install_termination_cause(TerminationCause::CANCELLED));
    EXPECT_TRUE(terminated.record_reap_status(0));
    EXPECT_EQ(terminated.termination_cause(), TerminationCause::TIMEOUT);
    EXPECT_FALSE(terminated.natural_outcome_frozen());

    ProcessHandle natural(11, 11, true);
    EXPECT_TRUE(natural.record_reap_status(7 << 8));
    ASSERT_TRUE(natural.leader_outcome().has_value());
    EXPECT_EQ(natural.leader_outcome()->exit_code, 7);
    EXPECT_TRUE(natural.natural_outcome_frozen());
    EXPECT_FALSE(natural.install_termination_cause(TerminationCause::CANCELLED));
    EXPECT_EQ(natural.termination_cause(), TerminationCause::NONE);
}

TEST(ProcessHandle, SignalOutcomeAndCleanupPhaseAreExplicit) {
    ProcessHandle handle(12, 12, false);
    EXPECT_FALSE(handle.group_established());
    handle.confirm_group_established();
    EXPECT_TRUE(handle.group_established());
    EXPECT_TRUE(handle.record_reap_status(SIGTERM & 0x7f));
    ASSERT_TRUE(handle.leader_outcome().has_value());
    EXPECT_EQ(handle.leader_outcome()->term_signal, SIGTERM);
    handle.set_cleanup_phase(GroupCleanupPhase::TERM_SENT);
    EXPECT_EQ(handle.cleanup_phase(), GroupCleanupPhase::TERM_SENT);
}

}  // namespace
}  // namespace ts::runtime
