#include "runtime/signals.h"

#include <gtest/gtest.h>

using namespace lesh::runtime;

TEST(Signals, NamesResolveWithAndWithoutPrefix) {
	EXPECT_EQ(signal_state::signal_number("INT"), SIGINT);
	EXPECT_EQ(signal_state::signal_number("SIGINT"), SIGINT);
	EXPECT_EQ(signal_state::signal_number("TERM"), SIGTERM);
	EXPECT_EQ(signal_state::signal_number("EXIT"), kExitTrap);
}

TEST(Signals, NumbersAreAccepted) {
	// `trap - 2` is legal, so a bare number must resolve.
	EXPECT_EQ(signal_state::signal_number("2"), 2);
	EXPECT_EQ(signal_state::signal_number("0"), kExitTrap);
}

TEST(Signals, UnknownNamesAreRejectedRatherThanGuessed) {
	EXPECT_EQ(signal_state::signal_number("NOSUCH"), -1);
	EXPECT_EQ(signal_state::signal_number(""), -1);
	EXPECT_EQ(signal_state::signal_number("12x"), -1);
}

TEST(Signals, DispositionsRoundTrip) {
	signal_state s;
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action);

	s.set_trap(SIGUSR1, "echo hi");
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::handler);
	EXPECT_EQ(s.trap_command(SIGUSR1), "echo hi");

	s.set_ignore(SIGUSR1);
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::ignore);

	s.reset(SIGUSR1);
	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action);
}

TEST(Signals, SubshellResetsHandlersButKeepsIgnores) {
	// POSIX's asymmetry, and the thing several conformance cases test: a subshell
	// resets traps to default EXCEPT those set to ignore, so `trap '' INT`
	// protects a whole subtree while a handler belongs to the shell that set it.
	signal_state s;
	s.set_trap(SIGUSR1, "echo handler");
	s.set_ignore(SIGUSR2);

	s.reset_for_subshell();

	EXPECT_EQ(s.disposition_of(SIGUSR1), disposition::default_action)
		<< "a handler must not survive into a subshell";
	EXPECT_EQ(s.disposition_of(SIGUSR2), disposition::ignore)
		<< "an ignore must survive";
}

TEST(Signals, PendingIsEmptyUntilSomethingArrives) {
	signal_state s;
	int signo = 0;
	EXPECT_FALSE(s.any_pending());
	EXPECT_FALSE(s.take_pending(signo));
}

TEST(Signals, PendingIsConsumedOnce) {
	signal_state s;
	// Simulating what the handler does - the only thing it is allowed to do.
	g_pending[SIGUSR1] = 1;

	EXPECT_TRUE(s.any_pending());
	int signo = 0;
	ASSERT_TRUE(s.take_pending(signo));
	EXPECT_EQ(signo, SIGUSR1);

	EXPECT_FALSE(s.take_pending(signo)) << "taking must clear the flag";
	EXPECT_FALSE(s.any_pending());
}
