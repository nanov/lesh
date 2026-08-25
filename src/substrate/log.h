#pragma once

// ONE RECORDER, TWO SINKS (#109, architecture spec §6.6).
//
// Every message carries a LEVEL - how noisy the user asked for - and a CATEGORY
// - which subsystem is speaking. neovim's axis and fish's `FLOG` axis, both,
// because they answer different questions and a shell needs both answers: a user
// debugging a hang sets `LESH_LOG=debug`, a user chasing the highlighter sets
// `LESH_LOG=trace:reactor,provider`, and the dashboard #94 wants someday reads
// categories without caring what level the user picked.
//
// THE COST RULE IS THE WHOLE DESIGN. N-1 gives the keystroke path a millisecond,
// and a logger that costs anything when off has spent some of it:
//
//   - a DISABLED log is ONE RELAXED ATOMIC LOAD and one predictable branch. The
//     entire enabled set is 55 bits of a single `uint64_t`, so the check is a
//     load, a shift and a test - no per-category array, no cache line per
//     subsystem, no lock.
//   - ARGUMENTS EVALUATE ONLY PAST THE CHECK. The macro puts them inside the
//     `if`, so `LESH_LOG(debug, exec, "%s", expensive())` does not call
//     `expensive()` when debug is off. This is the half a plain function call
//     cannot give you, and the reason logging is a macro here at all.
//   - FORMATTING USES A FIXED THREAD-LOCAL BUFFER AND TRUNCATES. It never
//     allocates - a log line that reallocates is an allocation-gate suspect, and
//     tests/unit/allocation_tests.cpp asserts the off path adds no heap at all.
//   - `trace` IS COMPILED OUT of release entirely (LESH_ENABLE_TRACE_LOGGING,
//     set by the debug and bench presets and not by release). N-1 never sees a
//     trace call site in the binary it ships.
//
// WHERE IT GOES. `$LESH_LOG_FILE`, defaulting to
// `${XDG_STATE_HOME:-~/.local/state}/lesh/log`, directory created. NEVER STDERR
// WHILE LESHPER OWNS THE TERMINAL (#98): a stray write corrupts the screen, and
// there is no flag that turns that back on. stderr is reachable only by a
// NON-INTERACTIVE shell that set a level and named no file. Off by default in
// both shells - this is opt-in diagnostics, not a background cost.
//
// THE REPLAY FILE IS THE SECOND SINK, not a second serialization. `LESH_REPLAY_FILE`
// opens a structured sink (jsonl, one object per line) that the `event` category
// writes every loop input to; feeding that file back through the editor must
// reproduce an equal state, which is N-3. A test harness with its own event
// format would be the bug this arrangement prevents.
//
// REDACTION. Buffer contents and key codepoints appear at `trace` ONLY. `debug`
// logs lengths and action names and is therefore safe to paste into a bug
// report. The replay file necessarily contains full input; that is what it is
// for, and it is opt-in and documented as such.
//
// WHY SUBSTRATE. Shell core and leshper both log, so the logger has to sit below
// both, and it depends on nothing above itself - POSIX, <atomic> and <cstdio>.

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace lesh::log {

// HOW NOISY. neovim's axis, in order, and the order is load-bearing: asking for
// `debug` enables everything at or above it in importance, which is the
// comparison two lines down and the reason `off` is zero.
enum class level : uint8_t {
	off,
	error,
	warn,
	info,
	debug,
	trace,
	count_,   // must stay last
};

// WHICH SUBSYSTEM. fish's `FLOG` axis, fixed by #109. A new category is a
// deliberate addition here plus a row in `kCategoryNames`, which the
// static_assert below makes mandatory rather than optional.
enum class category : uint8_t {
	loop,       // one turn of the event loop, and the effects it carried out
	dispatch,   // key sequence to action; mode and keymap changes
	reactor,    // subscriptions and the results they deliver
	provider,   // request lifecycle: issued, superseded, killed, completed
	spawn,      // children: pid, argv summary, exit
	worker,     // the pool: park, wake, arena reset
	render,     // frames, at trace only
	history,    // store operations, NEVER their contents
	exec,       // the shell's own execution path
	parse,      // the lexer and the parser
	event,      // every loop input; the replay file's whole content
	count_,     // must stay last
};

inline constexpr const char* kLevelNames[] = {
	"off", "error", "warn", "info", "debug", "trace",
};
inline constexpr const char* kCategoryNames[] = {
	"loop", "dispatch", "reactor", "provider", "spawn", "worker",
	"render", "history", "exec", "parse", "event",
};

static_assert(std::size(kLevelNames) == static_cast<size_t>(level::count_),
              "every level needs a name");
static_assert(std::size(kCategoryNames) == static_cast<size_t>(category::count_),
              "every category needs a name");

[[nodiscard]] constexpr const char* name_of(level of) noexcept {
	return kLevelNames[static_cast<size_t>(of)];
}
[[nodiscard]] constexpr const char* name_of(category of) noexcept {
	return kCategoryNames[static_cast<size_t>(of)];
}

// A name back to its enumerator. False leaves the out-parameter untouched, which
// is what lets `LESH_LOG=nonsense` be reported rather than silently mean `error`.
[[nodiscard]] bool level_from_name(std::string_view name, level& into) noexcept;
[[nodiscard]] bool category_from_name(std::string_view name, category& into) noexcept;

// ---------------------------------------------------------------------------
// The gate: one word, one relaxed load.
// ---------------------------------------------------------------------------

// Five real levels (`off` is the absence of one) times eleven categories is 55
// bits, and bit 63 says the structured sink is open. Both fit in one `uint64_t`,
// which is the entire reason the check is a load and a test instead of an
// indexed read out of a table that may not be in cache.
inline constexpr int kEnabledLevels = static_cast<int>(level::count_) - 1;
inline constexpr int kRecordingBit = 63;
static_assert(kEnabledLevels * static_cast<int>(category::count_) <= kRecordingBit,
              "the level-by-category grid no longer fits below the recording bit");

[[nodiscard]] constexpr int gate_bit(level of, category in) noexcept {
	return static_cast<int>(in) * kEnabledLevels + static_cast<int>(of) - 1;
}

// The one piece of mutable state on the hot path. Zero means every check below
// is false, which is the state the process starts in and the state it returns to
// after `shutdown()`.
namespace detail {
inline std::atomic<uint64_t> gate{0};
}

// IS THIS MESSAGE WANTED? One relaxed load, one shift, one test.
//
// Relaxed is the right order and not a shortcut: the only thing being published
// is "somebody turned logging on", a thread that misses it by a few nanoseconds
// loses one line of diagnostics, and an acquire here would put a fence on the
// keystroke path to buy that line.
[[nodiscard]] inline bool enabled(level of, category in) noexcept {
	return (detail::gate.load(std::memory_order_relaxed) >> gate_bit(of, in)) & 1u;
}

// IS THE REPLAY FILE OPEN? Same word, same load.
[[nodiscard]] inline bool recording() noexcept {
	return (detail::gate.load(std::memory_order_relaxed) >> kRecordingBit) & 1u;
}

// Both questions off ONE load, for the caller that has to ask both - the editor's
// event hook, which is on the keystroke path and would otherwise pay two.
[[nodiscard]] inline bool enabled_or_recording(level of, category in) noexcept {
	const uint64_t bits = detail::gate.load(std::memory_order_relaxed);
	return ((bits >> gate_bit(of, in)) | (bits >> kRecordingBit)) & 1u;
}

// ---------------------------------------------------------------------------
// Writing.
// ---------------------------------------------------------------------------

// The fixed thread-local line buffer. A message longer than this is TRUNCATED,
// never grown: growing is an allocation, and an allocation on the logging path
// is the thing the cost rule forbids. 1 KiB is past the length at which a log
// line stops being readable anyway.
inline constexpr size_t kLineCapacity = 1024;

// Formats and writes ONE line. Never call this directly - `LESH_LOG` does the
// gate check that keeps the arguments unevaluated when the level is off.
//
// printf-shaped, and checked by the compiler as such: a format-string mismatch is
// a build error rather than a garbage line in a diagnostic file, and `vsnprintf`
// truncates into a fixed buffer by definition where a `std::format` into a
// `std::string` would allocate.
void write(level of, category in, const char* format, ...) noexcept
	__attribute__((format(printf, 3, 4)));

void write_v(level of, category in, const char* format, va_list args) noexcept;

// THE ONE LOGGING STATEMENT.
//
// The `if` is the deferral: everything in `__VA_ARGS__` sits inside it, so a
// disabled log evaluates none of it. `[[unlikely]]` tells the branch predictor
// what is true in every shipping run.
#define LESH_LOG(level_, category_, ...)                                       \
	do {                                                                       \
		if (::lesh::log::enabled((level_), (category_))) [[unlikely]]          \
			::lesh::log::write((level_), (category_), __VA_ARGS__);            \
	} while (0)

// TRACE, WHICH DOES NOT EXIST IN RELEASE.
//
// Separate from `LESH_LOG` because the guarantee is different in kind: the other
// four levels are runtime-gated and their call sites are in the binary, while a
// trace call site is not compiled at all unless LESH_ENABLE_TRACE_LOGGING is
// defined - which the debug and bench presets do and the release preset does
// not. That is what "N-1 never sees logging in release" means literally.
//
// This is also where the redaction rule lands: buffer contents and key
// codepoints go through here and nowhere else.
#ifdef LESH_ENABLE_TRACE_LOGGING
#define LESH_LOG_TRACE(category_, ...)                                         \
	LESH_LOG(::lesh::log::level::trace, (category_), __VA_ARGS__)
inline constexpr bool kTraceCompiledIn = true;
#else
#define LESH_LOG_TRACE(category_, ...) ((void)0)
inline constexpr bool kTraceCompiledIn = false;
#endif

// ---------------------------------------------------------------------------
// The structured sink: one jsonl object per record.
// ---------------------------------------------------------------------------

// A record's fixed buffer. Larger than a text line by a wide margin because a
// paste event carries its whole payload, and a replay file that drops the tail
// of a paste replays something the user never typed.
inline constexpr size_t kRecordCapacity = 32 * 1024;

// ONE STRUCTURED RECORD, built in place and written by `commit()`.
//
// Deliberately not a JSON library: a flat object of string, integer and boolean
// fields is the whole format, writing it needs no dependency, and the harness on
// the other side reads it with a reader of the same size. jsonl rather than one
// big array so a crashed session still leaves a file that parses line by line,
// and so a dashboard can tail it.
//
// Overflow TRUNCATES AND SAYS SO: the object is still closed as valid JSON and
// carries `"truncated":true`, because a half-written line the reader chokes on
// is a worse failure than a visible short one. Nothing here allocates.
//
// ONE AT A TIME PER THREAD. The buffer is thread-local and a record writes into
// it directly, so two live records on one thread would overwrite each other.
// That is what the fixed-buffer rule costs, and it costs nothing in practice: a
// record is built and committed inside one statement at one call site.
class record {
public:
	explicit record(category in) noexcept;

	record& text(std::string_view key, std::string_view value) noexcept;
	record& number(std::string_view key, int64_t value) noexcept;
	record& number(std::string_view key, uint64_t value) noexcept;
	record& flag(std::string_view key, bool value) noexcept;

	// Closes the object and writes the line to the structured sink. A no-op when
	// no structured sink is open, so a caller that forgot `recording()` is slow
	// rather than wrong.
	void commit() noexcept;

	// The finished line, for the tests that assert the format without a file.
	// Closes the object if `commit()` has not already.
	[[nodiscard]] std::string_view line() noexcept;

private:
	// Room for `bytes` more, with the closing quote, the truncation marker, the
	// brace and the newline still reserved. Every append asks first, and asks
	// about a WHOLE unit - a field, an escape sequence - so a line is never cut
	// through the middle of one and left unparseable.
	[[nodiscard]] bool fits(size_t bytes) const noexcept;
	void raw(char c) noexcept;                        // room already established
	void raw(std::string_view bytes) noexcept;        // room already established
	[[nodiscard]] bool put(std::string_view bytes) noexcept;
	void put_escaped(std::string_view value) noexcept;
	[[nodiscard]] bool begin_field(std::string_view key) noexcept;
	record& put_scalar(std::string_view key, std::string_view value) noexcept;
	void close() noexcept;

	char* _buffer;
	size_t _length = 0;
	bool _truncated = false;
	bool _closed = false;
};

// ---------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------

// The version the startup line reports. The only version string in the tree; a
// release process replaces this with something generated, and `options.version`
// is the seam that makes that a one-line change.
inline constexpr const char* kVersion = "0.1.0-dev";

// What the environment asked for, PARSED AND NOTHING ELSE.
//
// Pure, taking the strings rather than calling `getenv`, so a test can ask it
// about an environment the machine has not got - `terminal_capabilities::from_env`
// (#112) is the precedent and the reason is the same one.
struct settings {
	// The level-by-category grid the gate word is built from. Never has the
	// recording bit set; opening the structured sink is `configure`'s business.
	uint64_t enabled = 0;
	// Where the text sink goes when it goes to a file. Always resolved - the
	// explicit `$LESH_LOG_FILE` or the XDG default - so a caller never has to
	// rebuild the default itself.
	std::string log_path;
	// Whether `log_path` came from `$LESH_LOG_FILE` rather than from the default.
	// The one thing that distinguishes "write it here" from "write it wherever
	// lesh keeps its state", and therefore the field that decides whether a
	// NON-INTERACTIVE shell talks to stderr instead (#98 forbids that outright
	// while leshper owns the terminal, whatever this says).
	bool log_path_explicit = false;
	// Where the structured sink goes. Empty means no replay file.
	std::string replay_path;
	// `LESH_LOG` named something that is not a level or not a category. Carried
	// rather than printed, because this layer has no business writing to a
	// terminal it may not own.
	bool malformed = false;

	friend bool operator==(const settings&, const settings&) noexcept = default;
};

// `LESH_LOG=<level>[:<cat>,<cat>]`, and the two file variables.
//
// NAMES ONLY, NEVER NUMBERS. neovim takes a numeric level and it is the part of
// its interface nobody remembers; `LESH_LOG=3` and `LESH_LOG=debug` would also
// be two spellings of one thing, and the second is the one a bug report can be
// read back from. It has the pleasant second effect that this parser reads no
// digits at all, so there is no numeric operand here for substrate/numeric.h to
// own.
//
// A level with no category list enables that level for EVERY category, which is
// what `LESH_LOG=debug` should obviously mean. An empty or absent `LESH_LOG`
// enables nothing, whatever the file variables say: naming a file is not asking
// for output.
[[nodiscard]] settings settings_from_env(const char* lesh_log,
                                         const char* lesh_log_file,
                                         const char* lesh_replay_file,
                                         const char* xdg_state_home,
                                         const char* home);

// What the process knows that the environment does not.
struct options {
	// Whether leshper owns the terminal. TRUE FORBIDS STDERR outright (#98) -
	// there is no override, because the failure mode is a corrupted screen and
	// the user cannot see the message that corrupted it.
	bool interactive = false;
	// Reported by the startup line. `\0`-terminated or null.
	const char* tty = nullptr;
	const char* version = kVersion;
	// THE FLOOR-DETECTION SLOT, deliberately empty (#97 has not landed). The
	// startup line prints `floor=` with this after it, so the field is already in
	// every log file and #97 fills a value in rather than changing a format that
	// tools have started to read.
	const char* floor = "";
};

// Opens the sinks the settings ask for, arms the gate, and writes the startup
// line. Idempotent in the sense that calling it again reconfigures from scratch:
// the previous sinks are closed first.
//
// Returns false when a sink was asked for and could not be opened, or when
// `LESH_LOG` named something that does not exist. The gate is armed only for
// whatever did open, so a missing log directory costs diagnostics rather than
// the session - and the caller, which is the layer that knows whether it may
// speak to the terminal, decides whether to say anything about it.
bool configure(const settings& from, const options& with);

// The convenience the two `main`s want: read the real environment.
bool configure_from_environment(const options& with);

// ADR-0007: everything the logger owns is released here, and the gate goes back
// to zero so a call site that runs afterwards is off rather than writing to a
// closed descriptor. Idempotent, and safe to call having never configured.
void shutdown() noexcept;

// The descriptors the sinks hold, for the tests that assert opening and closing.
// -1 when the sink is not open.
[[nodiscard]] int text_sink_fd() noexcept;
[[nodiscard]] int structured_sink_fd() noexcept;

} // namespace lesh::log
