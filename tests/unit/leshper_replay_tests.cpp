#include "leshper/editor.h"
#include "leshper/event.h"
#include "leshper/state.h"
#include "substrate/log.h"
#include "substrate/numeric.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace lesh::leshper;
namespace log = lesh::log;

// THE REPLAY HARNESS (N-3, #109 decision 4, #120).
//
// The property: a recorded event sequence, fed back through the editor, produces
// an EQUAL state. Not a similar one - equal by the operator `state` carries over
// every field, which exists for exactly this and for nothing else.
//
// What makes this harness worth having rather than a second copy of
// LeshperReplay's in-memory test is where the events come from: a FILE that a
// running editor wrote through the logger's structured sink. That is the seam
// #109 was arguing about. If the recorder and the replayer had separate ideas of
// what an event is, this file is where they would disagree, and a harness with
// its own event format could never notice.
//
// The reader below is deliberately small and deliberately here rather than in
// the substrate: the shell does not read replay files, the harness does. The
// writer's half lives in `log_event` in editor.cpp, and the two are meant to be
// read together.

namespace {

// ---------------------------------------------------------------------------
// A reader for the flat jsonl the structured sink writes.
//
// Not a JSON library and not trying to be: the format is one object per line
// whose values are strings, integers and booleans, which is all `log::record`
// can emit. Anything richer arriving here means the writer grew a shape the
// harness was never told about, and the reader says so by refusing the line
// instead of guessing.
// ---------------------------------------------------------------------------

struct field {
	std::string key;
	std::string text;      // the unescaped string, when the value was a string
	int64_t number = 0;    // the value, when it was an integer
	bool flag = false;     // the value, when it was a boolean
	bool is_text = false;
};

using object = std::vector<field>;

void skip_blanks(std::string_view line, size_t& at) {
	while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
		++at;
}

// One JSON string, unescaped in place. `at` points at the opening quote and
// lands past the closing one.
bool read_string(std::string_view line, size_t& at, std::string& into) {
	if (at >= line.size() || line[at] != '"')
		return false;
	++at;
	into.clear();
	while (at < line.size() && line[at] != '"') {
		if (line[at] != '\\') {
			into.push_back(line[at++]);
			continue;
		}
		if (++at >= line.size())
			return false;
		const char escape = line[at++];
		switch (escape) {
			case '"':  into.push_back('"');  break;
			case '\\': into.push_back('\\'); break;
			case '/':  into.push_back('/');  break;
			case 'n':  into.push_back('\n'); break;
			case 'r':  into.push_back('\r'); break;
			case 't':  into.push_back('\t'); break;
			case 'b':  into.push_back('\b'); break;
			case 'f':  into.push_back('\f'); break;
			case 'u': {
				// The writer only ever `\u`-escapes C0 controls, so four hex
				// digits below 0x20 is the whole of what can arrive. The digits
				// go through substrate/numeric.h like every other number lesh
				// reads - see the header there for why there is only one parser.
				if (at + 4 > line.size())
					return false;
				const lesh::digit_run run = lesh::scan_digits(line.substr(at, 4), 16, 0x10FFFF);
				if (run.consumed != 4 || run.value > 0x7F)
					return false;
				into.push_back(static_cast<char>(run.value));
				at += 4;
				break;
			}
			default:
				return false;
		}
	}
	if (at >= line.size())
		return false;
	++at;   // the closing quote
	return true;
}

// One integer. A leading `-` belongs to the operand here - a pid, a status and a
// signal number are all signed - and the digits themselves go through
// `scan_digits` rather than being accumulated by hand.
bool read_number(std::string_view line, size_t& at, int64_t& into) {
	const bool negative = at < line.size() && line[at] == '-';
	if (negative)
		++at;
	const lesh::digit_run run = lesh::scan_digits(line.substr(at), 10,
	                                              static_cast<uint64_t>(INT64_MAX));
	if (run.consumed == 0 || run.overflowed)
		return false;
	at += run.consumed;
	into = negative ? -static_cast<int64_t>(run.value) : static_cast<int64_t>(run.value);
	return true;
}

std::optional<object> read_object(std::string_view line) {
	object parsed;
	size_t at = 0;
	skip_blanks(line, at);
	if (at >= line.size() || line[at++] != '{')
		return std::nullopt;

	skip_blanks(line, at);
	if (at < line.size() && line[at] == '}')
		return parsed;

	while (true) {
		skip_blanks(line, at);
		field one;
		if (!read_string(line, at, one.key))
			return std::nullopt;
		skip_blanks(line, at);
		if (at >= line.size() || line[at++] != ':')
			return std::nullopt;
		skip_blanks(line, at);
		if (at >= line.size())
			return std::nullopt;

		if (line[at] == '"') {
			one.is_text = true;
			if (!read_string(line, at, one.text))
				return std::nullopt;
		} else if (line.compare(at, 4, "true") == 0) {
			one.flag = true;
			at += 4;
		} else if (line.compare(at, 5, "false") == 0) {
			one.flag = false;
			at += 5;
		} else if (!read_number(line, at, one.number)) {
			return std::nullopt;
		}
		parsed.push_back(std::move(one));

		skip_blanks(line, at);
		if (at < line.size() && line[at] == ',') {
			++at;
			continue;
		}
		if (at < line.size() && line[at] == '}')
			return parsed;
		return std::nullopt;
	}
}

const field* find(const object& in, std::string_view key) {
	for (const field& one : in)
		if (one.key == key)
			return &one;
	return nullptr;
}

std::string_view text_of(const object& in, std::string_view key) {
	const field* one = find(in, key);
	return one != nullptr && one->is_text ? std::string_view{one->text} : std::string_view{};
}

int64_t number_of(const object& in, std::string_view key) {
	const field* one = find(in, key);
	return one != nullptr ? one->number : 0;
}

bool flag_of(const object& in, std::string_view key) {
	const field* one = find(in, key);
	return one != nullptr && one->flag;
}

// One record back to the event that produced it. `std::nullopt` for a line that
// is not an `event` record at all - a dashboard tailing the same file would
// write its own, and a replay must skip them rather than choke.
std::optional<event> event_from(const object& in) {
	if (text_of(in, "cat") != "event")
		return std::nullopt;
	const std::string_view kind = text_of(in, "kind");

	if (kind == "key") {
		key_event key;
		key.codepoint = static_cast<char32_t>(number_of(in, "cp"));
		key.named = flag_of(in, "named");
		key.key = static_cast<named_key>(number_of(in, "key"));
		key.modifiers.shift = flag_of(in, "shift");
		key.modifiers.alt = flag_of(in, "alt");
		key.modifiers.ctrl = flag_of(in, "ctrl");
		return event{key};
	}
	if (kind == "resize")
		return event{resize_event{static_cast<uint16_t>(number_of(in, "columns")),
		                          static_cast<uint16_t>(number_of(in, "rows"))}};
	if (kind == "worker_result") {
		// `generation` bumps rather than assigns, which is what keeps a result
		// from naming a number nothing produced (N-4). Replaying one therefore
		// counts up to it - the only way in, and cheap at the sizes a session
		// reaches.
		generation gen;
		for (int64_t i = 0; i < number_of(in, "gen"); ++i)
			gen.bump();
		return event{worker_result{gen}};
	}
	if (kind == "job")
		return event{job_notice{static_cast<int>(number_of(in, "pid")),
		                        static_cast<int>(number_of(in, "status"))}};
	if (kind == "injected")
		return event{injected_input{std::string{text_of(in, "text")}}};
	if (kind == "signal")
		return event{signal_event{static_cast<int>(number_of(in, "signal"))}};
	if (kind == "paste")
		return event{paste_event{std::string{text_of(in, "text")}}};
	return std::nullopt;
}

// THE REPLAY READER: a replay file back to the sequence that wrote it.
std::vector<event> read_replay(const std::string& path) {
	std::vector<event> recovered;
	std::ifstream in{path};
	for (std::string line; std::getline(in, line);) {
		if (line.empty())
			continue;
		const std::optional<object> parsed = read_object(line);
		if (!parsed.has_value())
			continue;
		if (std::optional<event> one = event_from(*parsed))
			recovered.push_back(std::move(*one));
	}
	return recovered;
}

// A session worth replaying: every alternative of the variant at least once, and
// enough editing that state equality is not vacuous.
std::vector<event> recorded_session() {
	return {
		key_event::of(U'e'),
		key_event::of(U'c'),
		key_event::of(U'h'),
		key_event::of(U'o'),
		key_event::of(U' '),
		resize_event{100, 30},
		key_event::of(U'é'),                       // multi-byte, decoded
		key_event::of(named_key::left),
		key_event::of(named_key::right, key_modifiers{false, false, true}),
		key_event::of(U'o'),
		key_event::of(U'n'),
		key_event::of(U'e'),
		worker_result{generation{}},                    // stale by now: dropped
		key_event::of(0x17),                            // Ctrl-W
		injected_input{"two"},
		key_event::of(0x7F),                            // backspace
		key_event::of(0x01),                            // Ctrl-A
		key_event::of(0x05),                            // Ctrl-E
		key_event::of(0x1F),                            // undo
		signal_event{28},
		job_notice{7, 0},
	};
}

state play(const std::vector<event>& sequence) {
	state s;
	for (const event& one : sequence)
		step(s, one);
	return s;
}

class LeshperReplayFileTest : public ::testing::Test {
protected:
	lesh::testing::temp_path scratch;

	void SetUp() override { log::shutdown(); }
	void TearDown() override { log::shutdown(); }

	// Drives the editor with the logger recording to `path`, and answers the
	// state the live run reached.
	state record_to(const std::string& path, const std::vector<event>& sequence) {
		const log::settings asked =
			log::settings_from_env(nullptr, nullptr, path.c_str(), nullptr, nullptr);
		EXPECT_TRUE(log::configure(asked, {}));
		EXPECT_TRUE(log::recording());
		const state reached = play(sequence);
		log::shutdown();
		return reached;
	}
};

} // namespace

// ---------------------------------------------------------------------------
// The property.
// ---------------------------------------------------------------------------

TEST_F(LeshperReplayFileTest, AReplayFileReproducesTheStateThatWroteIt) {
	const std::string path = scratch.file("replay.jsonl");
	const state live = record_to(path, recorded_session());

	// Replay reads the FILE. Nothing from the live run crosses over - not the
	// event vector, not the state - which is what makes this a test of the
	// recording rather than of `step()` being deterministic.
	const std::vector<event> recovered = read_replay(path);
	ASSERT_EQ(recovered.size(), recorded_session().size())
		<< "every loop input is recorded, or the replay is of a different session";

	const state replayed = play(recovered);
	EXPECT_TRUE(live == replayed);

	// And the session did something, so the equality above is not two empty
	// states agreeing.
	EXPECT_FALSE(live.buffer.empty());
	EXPECT_GT(live.gen.value(), 0u);
	EXPECT_EQ(live.columns, 100);
}

TEST_F(LeshperReplayFileTest, ARecordedRunAndAnUnrecordedRunReachTheSameState) {
	// Logging must not be observable in editor state. If recording changed what
	// the editor did, the replay file would be a record of a session nobody had.
	const std::string path = scratch.file("replay.jsonl");
	const state recorded = record_to(path, recorded_session());
	const state silent = play(recorded_session());
	EXPECT_TRUE(recorded == silent);
}

TEST_F(LeshperReplayFileTest, EveryAlternativeOfTheEventVariantSurvivesTheRoundTrip) {
	// Field by field rather than through state equality, because six of the seven
	// alternatives change no editor state at all and a recorder that dropped
	// their fields would still pass the test above.
	const std::string path = scratch.file("replay.jsonl");
	const std::vector<event> sequence{
		key_event::of(U'é', key_modifiers{true, false, true}),
		key_event::of(named_key::page_down),
		resize_event{221, 61},
		worker_result{[] { generation g; g.bump(); g.bump(); g.bump(); return g; }()},
		job_notice{4242, -9},
		injected_input{"pushed \"back\"\nthrough the keymap"},
		signal_event{28},
		paste_event{"a\tpasted\nline\\with escapes"},
	};
	(void)record_to(path, sequence);

	const std::vector<event> recovered = read_replay(path);
	ASSERT_EQ(recovered.size(), sequence.size());

	const auto& typed = std::get<key_event>(recovered[0]);
	EXPECT_EQ(typed.codepoint, U'é');
	EXPECT_FALSE(typed.named);
	EXPECT_TRUE(typed.modifiers.shift);
	EXPECT_FALSE(typed.modifiers.alt);
	EXPECT_TRUE(typed.modifiers.ctrl);

	const auto& named = std::get<key_event>(recovered[1]);
	EXPECT_TRUE(named.named);
	EXPECT_EQ(named.key, named_key::page_down);

	EXPECT_EQ(std::get<resize_event>(recovered[2]).columns, 221);
	EXPECT_EQ(std::get<resize_event>(recovered[2]).rows, 61);
	EXPECT_EQ(std::get<worker_result>(recovered[3]).computed_against.value(), 3u);
	EXPECT_EQ(std::get<job_notice>(recovered[4]).pid, 4242);
	EXPECT_EQ(std::get<job_notice>(recovered[4]).status, -9);
	EXPECT_EQ(std::get<injected_input>(recovered[5]).text, "pushed \"back\"\nthrough the keymap");
	EXPECT_EQ(std::get<signal_event>(recovered[6]).signal_number, 28);
	EXPECT_EQ(std::get<paste_event>(recovered[7]).text, "a\tpasted\nline\\with escapes");
}

TEST_F(LeshperReplayFileTest, NothingIsRecordedUntilAReplayFileIsAskedFor) {
	// Off by default: an unconfigured editor writes no file, and the hook costs
	// one branch to find that out.
	ASSERT_FALSE(log::recording());
	const state reached = play(recorded_session());
	EXPECT_FALSE(reached.buffer.empty());
	EXPECT_EQ(log::structured_sink_fd(), -1);
}

// ---------------------------------------------------------------------------
// The reader itself.
// ---------------------------------------------------------------------------

TEST_F(LeshperReplayFileTest, TheReaderSkipsLinesThatAreNotEventRecords) {
	// A dashboard (#94) is meant to be a third client of these records, so a
	// replay file may one day hold more than the harness cares about. Skipping is
	// the behaviour that keeps it readable; choking would make the format
	// unextendable.
	const std::string path = scratch.file("mixed.jsonl");
	{
		std::ofstream out{path};
		out << "{\"ts\":\"00:00:00.000\",\"cat\":\"provider\",\"kind\":\"issued\"}\n";
		out << "{\"ts\":\"00:00:00.001\",\"cat\":\"event\",\"kind\":\"signal\",\"signal\":2}\n";
		out << "not json at all\n";
		out << "\n";
		out << "{\"ts\":\"00:00:00.002\",\"cat\":\"event\",\"kind\":\"resize\","
		       "\"columns\":80,\"rows\":24}\n";
	}

	const std::vector<event> recovered = read_replay(path);
	ASSERT_EQ(recovered.size(), 2u);
	EXPECT_EQ(std::get<signal_event>(recovered[0]).signal_number, 2);
	EXPECT_EQ(std::get<resize_event>(recovered[1]).columns, 80);
}

TEST_F(LeshperReplayFileTest, ATruncatedRecordIsVisibleRatherThanCorrupting) {
	// A paste past the record buffer is cut and marked. The line still parses -
	// that is the point of closing the object - so a replay proceeds with a
	// visibly short paste rather than dying on the file.
	const std::string path = scratch.file("replay.jsonl");
	const std::string huge(2 * log::kRecordCapacity, 'p');
	(void)record_to(path, {paste_event{huge}});

	std::ifstream in{path};
	std::string line;
	ASSERT_TRUE(std::getline(in, line));
	EXPECT_NE(line.find("\"truncated\":true"), std::string::npos);

	const std::optional<object> parsed = read_object(line);
	ASSERT_TRUE(parsed.has_value()) << "a truncated record must still be valid JSON";
	EXPECT_TRUE(flag_of(*parsed, "truncated"));
	EXPECT_LT(text_of(*parsed, "text").size(), huge.size());
}
