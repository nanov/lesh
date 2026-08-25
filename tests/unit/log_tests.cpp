#include "substrate/log.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace lesh;

// The logger (#109, #120): level times category, off by default, one branch when
// off, a file rather than the terminal, and a structured sink the replay harness
// reads.
//
// Every test here restores the process to "logging off" when it finishes, because
// the gate is one process-wide word and a test that armed it and walked away
// would write the next test's session into its own log file. The fixture's
// TearDown is that guarantee rather than a courtesy.

namespace {

class LogTest : public ::testing::Test {
protected:
	lesh::testing::temp_path scratch;

	void SetUp() override { log::shutdown(); }
	void TearDown() override { log::shutdown(); }

	static std::string contents_of(const std::string& path) {
		std::ifstream in{path};
		std::ostringstream all;
		all << in.rdbuf();
		return all.str();
	}

	static std::vector<std::string> lines_of(const std::string& path) {
		std::vector<std::string> out;
		std::ifstream in{path};
		for (std::string line; std::getline(in, line);)
			out.push_back(line);
		return out;
	}

	// Settings for a text sink at `path` and nothing else, with `LESH_LOG` set to
	// `request` - the shape almost every test below wants.
	log::settings text_only(std::string_view request, const std::string& path) const {
		return log::settings_from_env(std::string{request}.c_str(), path.c_str(), nullptr,
		                              nullptr, nullptr);
	}
};

} // namespace

// ---------------------------------------------------------------------------
// The axes, and their names.
// ---------------------------------------------------------------------------

TEST_F(LogTest, EveryLevelAndCategoryRoundTripsThroughItsName) {
	for (int one = 0; one < static_cast<int>(log::level::count_); ++one) {
		const auto as_level = static_cast<log::level>(one);
		log::level back = log::level::off;
		ASSERT_TRUE(log::level_from_name(log::name_of(as_level), back)) << log::name_of(as_level);
		EXPECT_EQ(back, as_level);
	}
	for (int one = 0; one < static_cast<int>(log::category::count_); ++one) {
		const auto as_category = static_cast<log::category>(one);
		log::category back = log::category::loop;
		ASSERT_TRUE(log::category_from_name(log::name_of(as_category), back));
		EXPECT_EQ(back, as_category);
	}
}

TEST_F(LogTest, AnUnknownNameIsRefusedRatherThanGuessedAt) {
	// The out-parameter is left alone, so `LESH_LOG=debgu` cannot silently mean
	// `debug` - or, worse, `error`, which is what a defaulted answer would give.
	log::level untouched = log::level::info;
	EXPECT_FALSE(log::level_from_name("debgu", untouched));
	EXPECT_EQ(untouched, log::level::info);

	log::category also_untouched = log::category::render;
	EXPECT_FALSE(log::category_from_name("reactorr", also_untouched));
	EXPECT_EQ(also_untouched, log::category::render);
}

TEST_F(LogTest, TheGridFitsInOneWordWithTheRecordingBitClear) {
	// The cost rule's foundation: 55 bits of grid below bit 63. If a category is
	// added past what fits, this is what says so before the gate starts aliasing
	// two subsystems onto one bit.
	for (int one = 0; one < static_cast<int>(log::category::count_); ++one) {
		for (int of = 1; of < static_cast<int>(log::level::count_); ++of) {
			const int bit = log::gate_bit(static_cast<log::level>(of),
			                              static_cast<log::category>(one));
			EXPECT_GE(bit, 0);
			EXPECT_LT(bit, log::kRecordingBit);
		}
	}
}

// ---------------------------------------------------------------------------
// Parsing LESH_LOG.
// ---------------------------------------------------------------------------

TEST_F(LogTest, NothingIsEnabledUntilSomebodyAsks) {
	// Off by default in both shells (#109 decision 3). Naming a file is not
	// asking for output either.
	EXPECT_EQ(log::settings_from_env(nullptr, nullptr, nullptr, nullptr, nullptr).enabled, 0u);
	EXPECT_EQ(log::settings_from_env("", nullptr, nullptr, nullptr, nullptr).enabled, 0u);
	EXPECT_EQ(log::settings_from_env(nullptr, "/tmp/x", nullptr, nullptr, nullptr).enabled, 0u);
	EXPECT_EQ(log::settings_from_env("off", nullptr, nullptr, nullptr, nullptr).enabled, 0u);
}

TEST_F(LogTest, ALevelWithNoCategoryListMeansEveryCategory) {
	const log::settings parsed = log::settings_from_env("info", nullptr, nullptr, nullptr, nullptr);
	ASSERT_FALSE(parsed.malformed);
	ASSERT_TRUE(log::configure(parsed, {}));

	for (int one = 0; one < static_cast<int>(log::category::count_); ++one) {
		const auto in = static_cast<log::category>(one);
		EXPECT_TRUE(log::enabled(log::level::error, in)) << log::name_of(in);
		EXPECT_TRUE(log::enabled(log::level::info, in)) << log::name_of(in);
		// And the axis is ORDERED: asking for info does not turn on debug.
		EXPECT_FALSE(log::enabled(log::level::debug, in)) << log::name_of(in);
		EXPECT_FALSE(log::enabled(log::level::trace, in)) << log::name_of(in);
	}
}

TEST_F(LogTest, ACategoryListNarrowsToExactlyThoseCategories) {
	const std::string path = scratch.file("log");
	ASSERT_TRUE(log::configure(text_only("debug:reactor,provider", path), {}));

	EXPECT_TRUE(log::enabled(log::level::debug, log::category::reactor));
	EXPECT_TRUE(log::enabled(log::level::debug, log::category::provider));
	EXPECT_TRUE(log::enabled(log::level::error, log::category::reactor));
	EXPECT_FALSE(log::enabled(log::level::error, log::category::exec));
	EXPECT_FALSE(log::enabled(log::level::debug, log::category::event));
}

TEST_F(LogTest, AMalformedRequestIsReportedRatherThanApproximated) {
	// The level is the part that did not parse, so nothing is enabled at all - a
	// typo must not silently give you a different amount of logging than you
	// asked for.
	const log::settings bad_level = log::settings_from_env("debgu", nullptr, nullptr, nullptr, nullptr);
	EXPECT_TRUE(bad_level.malformed);
	EXPECT_EQ(bad_level.enabled, 0u);

	// A bad category among good ones keeps the good ones and still says so.
	const log::settings bad_category =
		log::settings_from_env("debug:reactor,nonsense", nullptr, nullptr, nullptr, nullptr);
	EXPECT_TRUE(bad_category.malformed);
	EXPECT_NE(bad_category.enabled, 0u);

	// And `configure` passes the complaint on, because this layer must not write
	// to a terminal it may not own (#98).
	const std::string path = scratch.file("log");
	EXPECT_FALSE(log::configure(text_only("debgu", path), {}));
}

TEST_F(LogTest, TheDefaultPathIsTheXdgStateOneAndAnExplicitFileWins) {
	const log::settings from_xdg =
		log::settings_from_env("info", nullptr, nullptr, "/state", "/home/u");
	EXPECT_EQ(from_xdg.log_path, "/state/lesh/log");
	EXPECT_FALSE(from_xdg.log_path_explicit);

	const log::settings from_home =
		log::settings_from_env("info", nullptr, nullptr, nullptr, "/home/u");
	EXPECT_EQ(from_home.log_path, "/home/u/.local/state/lesh/log");
	EXPECT_FALSE(from_home.log_path_explicit);

	const log::settings explicit_file =
		log::settings_from_env("info", "/tmp/mine", nullptr, "/state", "/home/u");
	EXPECT_EQ(explicit_file.log_path, "/tmp/mine");
	EXPECT_TRUE(explicit_file.log_path_explicit);
}

TEST_F(LogTest, TheReplayFileIsAskedForOnItsOwn) {
	// `LESH_REPLAY_FILE` does not need `LESH_LOG`: the harness wants every loop
	// input recorded and wants it without the text sink's noise.
	const log::settings parsed =
		log::settings_from_env(nullptr, nullptr, "/tmp/replay", nullptr, nullptr);
	EXPECT_EQ(parsed.replay_path, "/tmp/replay");
	EXPECT_EQ(parsed.enabled, 0u);
}

// ---------------------------------------------------------------------------
// The cost rule.
// ---------------------------------------------------------------------------

TEST_F(LogTest, ADisabledLogDoesNotEvaluateItsArguments) {
	// The reason logging is a macro. `expensive()` here stands for the string
	// building, the buffer walk and the syscall a real call site would do to
	// produce its message - none of which may happen when the level is off.
	int calls = 0;
	const auto expensive = [&calls] {
		++calls;
		return 7;
	};

	ASSERT_FALSE(log::enabled(log::level::debug, log::category::exec));
	for (int i = 0; i < 1000; ++i)
		LESH_LOG(log::level::debug, log::category::exec, "value=%d", expensive());
	EXPECT_EQ(calls, 0);

	// And past the check it does evaluate, so the test above is not vacuous.
	const std::string path = scratch.file("log");
	ASSERT_TRUE(log::configure(text_only("debug:exec", path), {}));
	LESH_LOG(log::level::debug, log::category::exec, "value=%d", expensive());
	EXPECT_EQ(calls, 1);
}

TEST_F(LogTest, TraceIsCompiledOutRatherThanGated) {
	// In this build - debug, so the preset defines LESH_ENABLE_TRACE_LOGGING -
	// trace call sites exist. In release they are not compiled at all, which is
	// what `kTraceCompiledIn` reports and what the release build asserts by
	// compiling this file with the constant false.
	const std::string path = scratch.file("log");
	ASSERT_TRUE(log::configure(text_only("trace:render", path), {}));

	int calls = 0;
	LESH_LOG_TRACE(log::category::render, "frame=%d", (++calls, 1));

	if (log::kTraceCompiledIn) {
		EXPECT_TRUE(log::enabled(log::level::trace, log::category::render));
		EXPECT_EQ(calls, 1);
	} else {
		// The gate refuses to arm a bit whose call sites do not exist, so nobody
		// reading the word concludes trace is on when it cannot be.
		EXPECT_FALSE(log::enabled(log::level::trace, log::category::render));
		EXPECT_EQ(calls, 0);
	}
}

// ---------------------------------------------------------------------------
// The text sink.
// ---------------------------------------------------------------------------

TEST_F(LogTest, TheLineCarriesClockLevelCategoryAndThread) {
	const std::string path = scratch.file("log");
	ASSERT_TRUE(log::configure(text_only("debug:exec", path), {}));
	LESH_LOG(log::level::warn, log::category::exec, "hello %s %d", "world", 3);
	log::shutdown();

	const std::vector<std::string> lines = lines_of(path);
	ASSERT_EQ(lines.size(), 2u) << "the startup line, then ours";

	// `HH:MM:SS.mmm LEVEL category thread: message` (#109 decision 6).
	const std::string& ours = lines[1];
	EXPECT_EQ(ours[2], ':');
	EXPECT_EQ(ours[5], ':');
	EXPECT_EQ(ours[8], '.');
	EXPECT_NE(ours.find(" WARN exec "), std::string::npos) << ours;
	EXPECT_NE(ours.find(": hello world 3"), std::string::npos) << ours;
}

TEST_F(LogTest, TheStartupLineRecordsVersionPidTtyAndAnEmptyFloorSlot) {
	const std::string path = scratch.file("log");
	log::options with;
	with.tty = "/dev/ttys004";
	ASSERT_TRUE(log::configure(text_only("error:loop", path), with));
	log::shutdown();

	const std::vector<std::string> lines = lines_of(path);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find(" INFO loop "), std::string::npos) << lines[0];
	EXPECT_NE(lines[0].find(std::string{"lesh "} + log::kVersion), std::string::npos) << lines[0];
	EXPECT_NE(lines[0].find("pid="), std::string::npos) << lines[0];
	EXPECT_NE(lines[0].find("tty=/dev/ttys004"), std::string::npos) << lines[0];
	// #97's slot: present and empty, so filling it later is a value change and
	// not a format change for whatever has started reading this line.
	EXPECT_TRUE(lines[0].ends_with("floor=")) << lines[0];
}

TEST_F(LogTest, TheStartupLineAppearsEvenWhenItsCategoryIsNotRequested) {
	// It is the file's header rather than a message about the loop: a user who
	// asked only for `provider` still needs to know which build wrote the file.
	const std::string path = scratch.file("log");
	ASSERT_TRUE(log::configure(text_only("error:provider", path), {}));
	log::shutdown();
	EXPECT_NE(contents_of(path).find(" INFO loop "), std::string::npos);
}

TEST_F(LogTest, AnOverlongMessageIsTruncatedAndMarkedRatherThanGrown) {
	const std::string path = scratch.file("log");
	ASSERT_TRUE(log::configure(text_only("info:parse", path), {}));

	const std::string huge(4 * log::kLineCapacity, 'x');
	LESH_LOG(log::level::info, log::category::parse, "%s", huge.c_str());
	log::shutdown();

	const std::vector<std::string> lines = lines_of(path);
	ASSERT_EQ(lines.size(), 2u);
	EXPECT_EQ(lines[1].size(), log::kLineCapacity - 1);
	EXPECT_TRUE(lines[1].ends_with("...")) << "a shortened line must be tellable from a short one";
}

TEST_F(LogTest, TheDirectoryIsCreatedRatherThanRequired) {
	// `${XDG_STATE_HOME}/lesh/` does not exist on a machine that has never run
	// lesh, and "your logs went nowhere because a directory was missing" is not
	// a diagnostic anyone can act on.
	const std::string path = scratch.file("a/b/c/log");
	ASSERT_TRUE(log::configure(text_only("info:loop", path), {}));
	EXPECT_GE(log::text_sink_fd(), 0);
	log::shutdown();
	EXPECT_FALSE(contents_of(path).empty());
}

TEST_F(LogTest, AnUnopenableSinkCostsDiagnosticsRatherThanTheSession) {
	// A path under a file rather than a directory: `mkdir` fails and so does
	// `open`. Nothing is armed, so no call site writes to a descriptor that is
	// not there, and the caller is told.
	const std::string blocker = scratch.file("blocker");
	{ std::ofstream make{blocker}; make << "x"; }

	EXPECT_FALSE(log::configure(text_only("debug", scratch.file("blocker/under/log")), {}));
	EXPECT_EQ(log::text_sink_fd(), -1);
	EXPECT_FALSE(log::enabled(log::level::error, log::category::loop));
}

TEST_F(LogTest, InteractiveNeverReachesStderr) {
	// #98, and there is no flag for it: a stray write to fd 2 corrupts the screen
	// the user is reading the shell on, and they cannot see the message that did
	// it. With no writable file the sink is nothing at all - never fd 2.
	log::options interactive;
	interactive.interactive = true;

	log::settings nowhere = log::settings_from_env("debug", nullptr, nullptr, nullptr, nullptr);
	nowhere.log_path.clear();
	EXPECT_FALSE(log::configure(nowhere, interactive));
	EXPECT_NE(log::text_sink_fd(), STDERR_FILENO);
	EXPECT_EQ(log::text_sink_fd(), -1);

	// Interactive with the default path still writes to the FILE.
	const std::string path = scratch.file("interactive/log");
	log::settings defaulted =
		log::settings_from_env("debug", nullptr, nullptr, scratch.dir().c_str(), nullptr);
	defaulted.log_path = path;
	ASSERT_TRUE(log::configure(defaulted, interactive));
	EXPECT_GT(log::text_sink_fd(), STDERR_FILENO);
}

TEST_F(LogTest, NonInteractiveWithNoNamedFileTalksToStderr) {
	// The other half of #109 decision 3: `LESH_LOG=debug lesh -c ...` obviously
	// wants its output where the user can see it, and no editor owns the screen.
	log::settings parsed = log::settings_from_env("debug", nullptr, nullptr, nullptr, nullptr);
	parsed.log_path = scratch.file("unused");
	parsed.log_path_explicit = false;
	ASSERT_TRUE(log::configure(parsed, {}));
	EXPECT_EQ(log::text_sink_fd(), STDERR_FILENO);

	// And fd 2 is NOT closed on the way out - closing the shell's stderr would be
	// a far worse bug than any this could have diagnosed.
	log::shutdown();
	EXPECT_EQ(::write(STDERR_FILENO, "", 0), 0);
}

TEST_F(LogTest, ShutdownClosesTheSinksAndDisarmsTheGate) {
	// ADR-0007, and the ordering that matters: the gate must be off before the
	// descriptors close, or a call site between the two writes to a closed fd.
	const std::string path = scratch.file("log");
	const std::string replay = scratch.file("replay.jsonl");
	ASSERT_TRUE(log::configure(
		log::settings_from_env("debug", path.c_str(), replay.c_str(), nullptr, nullptr), {}));
	ASSERT_GE(log::text_sink_fd(), 0);
	ASSERT_GE(log::structured_sink_fd(), 0);
	ASSERT_TRUE(log::recording());

	log::shutdown();
	EXPECT_EQ(log::text_sink_fd(), -1);
	EXPECT_EQ(log::structured_sink_fd(), -1);
	EXPECT_FALSE(log::recording());
	EXPECT_FALSE(log::enabled(log::level::error, log::category::loop));

	// Idempotent, and safe having never configured.
	log::shutdown();
	log::shutdown();
}

// ---------------------------------------------------------------------------
// The structured sink.
// ---------------------------------------------------------------------------

TEST_F(LogTest, ARecordIsOneFlatJsonObjectWithATimestampAndACategory) {
	log::record one{log::category::event};
	one.text("kind", "key").number("cp", uint64_t{97}).flag("named", false);
	const std::string line{one.line()};

	EXPECT_TRUE(line.starts_with("{\"ts\":\"")) << line;
	EXPECT_TRUE(line.ends_with('}')) << line;
	EXPECT_NE(line.find("\"cat\":\"event\""), std::string::npos) << line;
	EXPECT_NE(line.find("\"kind\":\"key\""), std::string::npos) << line;
	EXPECT_NE(line.find("\"cp\":97"), std::string::npos) << line;
	EXPECT_NE(line.find("\"named\":false"), std::string::npos) << line;
	EXPECT_EQ(line.find("\"truncated\""), std::string::npos) << line;
}

TEST_F(LogTest, StringValuesAreEscapedSoTheLineStaysOneLine) {
	log::record one{log::category::event};
	one.text("text", "a\"b\\c\nd\te\x01");
	const std::string line{one.line()};

	EXPECT_NE(line.find("a\\\"b\\\\c\\nd\\te\\u0001"), std::string::npos) << line;
	// jsonl's whole premise: one object per PHYSICAL line.
	EXPECT_EQ(line.find('\n'), std::string::npos);
}

TEST_F(LogTest, Utf8PassesThroughUnescaped) {
	log::record one{log::category::event};
	one.text("text", "héllo → 🌍");
	EXPECT_NE(std::string{one.line()}.find("héllo → 🌍"), std::string::npos);
}

TEST_F(LogTest, AnOverlongRecordStaysValidJsonAndSaysItIsShort) {
	const std::string huge(2 * log::kRecordCapacity, 'z');
	log::record one{log::category::event};
	one.text("kind", "paste").text("text", huge);
	const std::string line{one.line()};

	EXPECT_LE(line.size(), log::kRecordCapacity);
	EXPECT_TRUE(line.ends_with("\"truncated\":true}")) << line.substr(line.size() - 40);
	// Still one object, still balanced: a reader gets a short answer rather than
	// a parse error it cannot recover from.
	EXPECT_TRUE(line.starts_with('{'));
}

TEST_F(LogTest, CommittingWithNoStructuredSinkIsANoOpRatherThanACrash) {
	ASSERT_FALSE(log::recording());
	log::record one{log::category::event};
	one.text("kind", "key");
	one.commit();
}

TEST_F(LogTest, TheStructuredSinkWritesOneLinePerCommittedRecord) {
	const std::string replay = scratch.file("nested/replay.jsonl");
	ASSERT_TRUE(log::configure(
		log::settings_from_env(nullptr, nullptr, replay.c_str(), nullptr, nullptr), {}));
	ASSERT_TRUE(log::recording());

	for (int i = 0; i < 3; ++i)
		log::record{log::category::event}.number("i", static_cast<int64_t>(i)).commit();
	log::shutdown();

	const std::vector<std::string> lines = lines_of(replay);
	ASSERT_EQ(lines.size(), 3u);
	for (int i = 0; i < 3; ++i) {
		EXPECT_TRUE(lines[static_cast<size_t>(i)].starts_with('{'));
		EXPECT_NE(lines[static_cast<size_t>(i)].find("\"i\":" + std::to_string(i)),
		          std::string::npos);
	}
}

TEST_F(LogTest, RecordingIsIndependentOfTheTextSink) {
	// A replay file with no `LESH_LOG` records everything and writes no text at
	// all, which is what the N-3 harness wants and what keeps a replay run from
	// being a diagnostics run.
	const std::string replay = scratch.file("replay.jsonl");
	ASSERT_TRUE(log::configure(
		log::settings_from_env(nullptr, nullptr, replay.c_str(), nullptr, nullptr), {}));
	EXPECT_TRUE(log::recording());
	EXPECT_EQ(log::text_sink_fd(), -1);
	for (int one = 0; one < static_cast<int>(log::category::count_); ++one)
		EXPECT_FALSE(log::enabled(log::level::error, static_cast<log::category>(one)));
}
