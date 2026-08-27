#include "substrate/log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace lesh::log {

namespace {

// ---------------------------------------------------------------------------
// The sinks.
//
// Two descriptors and two ownership bits, and that is the whole of the logger's
// mutable state besides the gate word. Raw descriptors rather than `FILE*`
// because a `FILE*` buffers - which means a crash loses the last thing the log
// said, and the last thing the log said is what a crash report is for - and
// because `write(2)` on an `O_APPEND` descriptor is what history_store already
// established here as the way to append without a lock.
//
// `owned` is false for stderr: closing fd 2 out from under the shell would be a
// far worse bug than any it could have diagnosed.
// ---------------------------------------------------------------------------
struct sink {
	int fd = -1;
	bool owned = false;

	void close() noexcept {
		if (fd >= 0 && owned)
			::close(fd);
		fd = -1;
		owned = false;
	}
};

sink& text_sink() noexcept {
	static sink one;
	return one;
}
sink& structured_sink() noexcept {
	static sink one;
	return one;
}

// ---------------------------------------------------------------------------
// The fixed buffers.
//
// `thread_local` arrays, so they are static storage rather than heap: no
// allocation on the logging path, no free at shutdown, and no sharing between
// threads to lock against. Two of them because a structured record and a text
// line have wildly different size needs and sharing one would size both at the
// larger.
// ---------------------------------------------------------------------------
thread_local char t_line[kLineCapacity];
thread_local char t_record[kRecordCapacity];

// This thread's OS-visible id, resolved once. The kernel's number rather than an
// ordinal of our own, because its whole job is to be the same number a debugger,
// `sample` or `perf` is showing at the same moment.
uint64_t this_thread_id() noexcept {
	thread_local uint64_t cached = [] {
#if defined(__APPLE__)
		uint64_t id = 0;
		::pthread_threadid_np(nullptr, &id);
		return id;
#elif defined(__linux__)
		return static_cast<uint64_t>(::syscall(SYS_gettid));
#else
		return uint64_t{0};
#endif
	}();
	return cached;
}

// `HH:MM:SS.mmm` into `into`, which must hold 13 bytes. Local time rather than
// UTC: a log read by the person whose machine wrote it is the only case there
// is, and a timestamp they have to convert is one they misread.
//
// No date. The startup line marks the session, and a shell log's questions are
// all about ordering within one.
size_t format_clock(char (&into)[16]) noexcept {
	::timespec now{};
	::clock_gettime(CLOCK_REALTIME, &now);
	::tm parts{};
	::localtime_r(&now.tv_sec, &parts);
	const int written = std::snprintf(into, sizeof into, "%02d:%02d:%02d.%03d",
	                                  parts.tm_hour, parts.tm_min, parts.tm_sec,
	                                  static_cast<int>(now.tv_nsec / 1000000));
	return written < 0 ? 0 : static_cast<size_t>(written);
}

// One `write(2)` for the whole line, retried only on EINTR.
//
// PER CALL is the atomicity POSIX gives an `O_APPEND` writer against every other
// appender to the same file, which is exactly what two lesh processes sharing a
// log file need. Splitting a line across two calls would open the window for a
// sibling's line to land inside it; a short write is therefore given up on
// rather than resumed, because resuming reopens that window.
void write_line(const sink& to, const char* bytes, size_t length) noexcept {
	if (to.fd < 0 || length == 0)
		return;
	ssize_t written = -1;
	do {
		written = ::write(to.fd, bytes, length);
	} while (written == -1 && errno == EINTR);
	(void)written;
}

// `mkdir -p` for the directory part of `path`, by hand rather than through
// <filesystem>.
//
// The substrate depends on nothing it does not have to, and <filesystem> is a
// large dependency to acquire for eleven lines. An existing directory is
// success; anything else is left for `open` to report, since a directory that
// cannot be made and a file that cannot be opened are the same failure to the
// caller.
bool make_parent_directories(const std::string& path) noexcept {
	for (size_t at = 1; at < path.size(); ++at) {
		if (path[at] != '/')
			continue;
		const std::string prefix = path.substr(0, at);
		if (::mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST)
			return false;
	}
	return true;
}

bool open_file_sink(sink& into, const std::string& path) noexcept {
	if (!make_parent_directories(path))
		return false;
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	into.fd = fd;
	into.owned = true;
	return true;
}

// The next `,`-separated or `:`-separated piece of `text`, with `at` advanced
// past the separator. Splitting by hand keeps this parser allocation-free and
// keeps the whole of it in view.
std::string_view next_piece(std::string_view text, size_t& at, char separator) noexcept {
	const size_t start = at;
	while (at < text.size() && text[at] != separator)
		++at;
	const std::string_view piece = text.substr(start, at - start);
	if (at < text.size())
		++at;
	return piece;
}

// Every level at or above `ceiling` in importance, for `in`. `error` is always
// included when anything is, which is the point of an ordered axis.
uint64_t bits_for(level ceiling, category in) noexcept {
	uint64_t bits = 0;
	for (int one = 1; one <= static_cast<int>(ceiling); ++one)
		bits |= uint64_t{1} << gate_bit(static_cast<level>(one), in);
	return bits;
}

// `$HOME/.local/state` or `$XDG_STATE_HOME`, then `/lesh/log`. The XDG basedir
// spec's own fallback, spelled out because neither variable is guaranteed set
// and a logger that silently writes nowhere is worse than one that says it
// could not.
std::string default_log_path(const char* xdg_state_home, const char* home) {
	if (xdg_state_home != nullptr && xdg_state_home[0] != '\0')
		return std::string{xdg_state_home} + "/lesh/log";
	if (home != nullptr && home[0] != '\0')
		return std::string{home} + "/.local/state/lesh/log";
	return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Names.
// ---------------------------------------------------------------------------

bool level_from_name(std::string_view name, level& into) noexcept {
	for (size_t one = 0; one < std::size(kLevelNames); ++one) {
		if (name == kLevelNames[one]) {
			into = static_cast<level>(one);
			return true;
		}
	}
	return false;
}

bool category_from_name(std::string_view name, category& into) noexcept {
	for (size_t one = 0; one < std::size(kCategoryNames); ++one) {
		if (name == kCategoryNames[one]) {
			into = static_cast<category>(one);
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// The text sink.
// ---------------------------------------------------------------------------

void write_v(level of, category in, const char* format, va_list args) noexcept {
	const sink& to = text_sink();
	if (to.fd < 0)
		return;

	char clock[16];
	const size_t stamp = format_clock(clock);

	// `HH:MM:SS.mmm LEVEL category thread: message` (#109). The level is upper
	// case because it is the field an eye scans a log for.
	char upper[8] = {};
	const char* level_name = name_of(of);
	size_t at = 0;
	for (; level_name[at] != '\0' && at + 1 < sizeof upper; ++at)
		upper[at] = static_cast<char>(level_name[at] - ('a' - 'A'));
	upper[at] = '\0';

	int head = std::snprintf(t_line, kLineCapacity, "%.*s %s %s %llu: ",
	                         static_cast<int>(stamp), clock, upper, name_of(in),
	                         static_cast<unsigned long long>(this_thread_id()));
	if (head < 0)
		return;
	size_t length = static_cast<size_t>(head);
	if (length >= kLineCapacity)
		length = kLineCapacity - 1;

	const int body = std::vsnprintf(t_line + length, kLineCapacity - length, format, args);
	if (body < 0)
		return;

	// TRUNCATE, NEVER GROW. A line past the buffer is cut and marked, so a reader
	// can tell a short message from a shortened one; growing would allocate, and
	// the cost rule forbids that on this path.
	const bool truncated = static_cast<size_t>(body) >= kLineCapacity - length;
	length = truncated ? kLineCapacity - 1 : length + static_cast<size_t>(body);
	if (truncated) {
		static constexpr char marker[] = "...";
		std::memcpy(t_line + length - (sizeof marker - 1), marker, sizeof marker - 1);
	}
	t_line[length] = '\n';
	write_line(to, t_line, length + 1);
}

void write(level of, category in, const char* format, ...) noexcept {
	va_list args;
	va_start(args, format);
	write_v(of, in, format, args);
	va_end(args);
}

// ---------------------------------------------------------------------------
// The structured sink.
// ---------------------------------------------------------------------------

namespace {

// What a full record still has to be able to say. Reserved out of the capacity
// from the first byte, so `close()` never has to decide whether it can afford to
// finish the object it started - it always can.
//
// A closing quote for a string cut mid-value, the marker, the brace, the
// newline. Getting this wrong is not a slightly short line, it is an unparseable
// one, which is why the arithmetic is named here rather than spelled inline.
constexpr std::string_view kTruncationMarker = ",\"truncated\":true";
constexpr size_t kRecordHeadroom = kTruncationMarker.size() + 4;

} // namespace

record::record(category in) noexcept : _buffer(t_record) {
	char clock[16];
	const size_t stamp = format_clock(clock);
	raw('{');
	if (begin_field("ts")) {
		raw('"');
		(void)put(std::string_view{clock, stamp});
		raw('"');
	}
	if (begin_field("cat")) {
		raw('"');
		(void)put(name_of(in));
		raw('"');
	}
}

bool record::fits(size_t bytes) const noexcept {
	return _length + bytes <= kRecordCapacity - kRecordHeadroom;
}

void record::raw(char c) noexcept { _buffer[_length++] = c; }

void record::raw(std::string_view bytes) noexcept {
	std::memcpy(_buffer + _length, bytes.data(), bytes.size());
	_length += bytes.size();
}

// ALL OF IT OR NONE OF IT. A unit that does not fit is dropped whole and the
// record is marked short; half a unit would be half an escape sequence or half a
// key, and a reader has no way back from either.
bool record::put(std::string_view bytes) noexcept {
	if (!fits(bytes.size())) {
		_truncated = true;
		return false;
	}
	raw(bytes);
	return true;
}

// JSON string escaping, and only what JSON requires: the two structural
// characters and the C0 controls. UTF-8 above 0x7F passes through unchanged,
// because the file is UTF-8 and `\u`-escaping it would only make it unreadable
// to the human the replay file is half written for.
//
// Cutting stops at the first unit that does not fit rather than skipping it and
// carrying on, so a truncated value is a PREFIX of the original - which is what
// makes a shortened paste still recognisably the paste it came from.
void record::put_escaped(std::string_view value) noexcept {
	for (const char one : value) {
		const unsigned char c = static_cast<unsigned char>(one);
		bool placed = true;
		switch (c) {
			case '"':  placed = put("\\\""); break;
			case '\\': placed = put("\\\\"); break;
			case '\n': placed = put("\\n");  break;
			case '\r': placed = put("\\r");  break;
			case '\t': placed = put("\\t");  break;
			default:
				if (c < 0x20) {
					char escape[7];
					std::snprintf(escape, sizeof escape, "\\u%04x", c);
					placed = put(std::string_view{escape, 6});
				} else {
					placed = put(std::string_view{&one, 1});
				}
		}
		if (!placed)
			return;
	}
}

// The separator, the key and the colon, as ONE unit. False means the field could
// not be started at all, and the caller must not write its value - a key with no
// value is the unparseable shape this returns a bool to prevent.
bool record::begin_field(std::string_view key) noexcept {
	// The escaped key plus the quotes, the colon and a possible leading comma.
	// Keys here are identifiers, so this is exact rather than an estimate.
	if (!fits(key.size() + 4)) {
		_truncated = true;
		return false;
	}
	if (_length > 1)
		raw(',');
	raw('"');
	put_escaped(key);
	raw("\":");
	return true;
}

record& record::text(std::string_view key, std::string_view value) noexcept {
	if (!begin_field(key))
		return *this;
	raw('"');
	put_escaped(value);
	// FORCED, and it always fits: the headroom exists so that a value cut short
	// still ends up inside a closed string. Without this the line would end in an
	// open quote and the reader would take the rest of the object as text.
	raw('"');
	return *this;
}

// The scalars go in whole or not at all - there is no useful prefix of a number,
// so a field that will not fit rolls the separator back out again.
record& record::put_scalar(std::string_view key, std::string_view value) noexcept {
	const size_t before = _length;
	if (!begin_field(key))
		return *this;
	if (!put(value))
		_length = before;
	return *this;
}

record& record::number(std::string_view key, int64_t value) noexcept {
	char digits[24];
	const int written = std::snprintf(digits, sizeof digits, "%lld",
	                                  static_cast<long long>(value));
	return written > 0 ? put_scalar(key, {digits, static_cast<size_t>(written)}) : *this;
}

record& record::number(std::string_view key, uint64_t value) noexcept {
	char digits[24];
	const int written = std::snprintf(digits, sizeof digits, "%llu",
	                                  static_cast<unsigned long long>(value));
	return written > 0 ? put_scalar(key, {digits, static_cast<size_t>(written)}) : *this;
}

record& record::flag(std::string_view key, bool value) noexcept {
	return put_scalar(key, value ? "true" : "false");
}

void record::close() noexcept {
	if (_closed)
		return;
	_closed = true;
	// Added LAST, and it always fits, because `fits()` has been holding room for
	// it since the first byte. A reader learns the line is short from the line
	// itself rather than from noticing the payload looks odd.
	if (_truncated)
		raw(_length > 1 ? kTruncationMarker : kTruncationMarker.substr(1));
	raw('}');
}

void record::commit() noexcept {
	close();
	const sink& to = structured_sink();
	if (to.fd < 0)
		return;
	_buffer[_length] = '\n';
	write_line(to, _buffer, _length + 1);
}

std::string_view record::line() noexcept {
	close();
	return {_buffer, _length};
}

// ---------------------------------------------------------------------------
// Configuration.
// ---------------------------------------------------------------------------

settings settings_from_env(const char* lesh_log, const char* lesh_log_file,
                           const char* lesh_replay_file, const char* xdg_state_home,
                           const char* home) {
	settings parsed;

	// A replay file is asked for independently of `LESH_LOG`: recording every
	// loop input is what the N-3 harness wants and it wants it without the text
	// sink's noise, so the two variables do not gate each other.
	if (lesh_replay_file != nullptr && lesh_replay_file[0] != '\0')
		parsed.replay_path = lesh_replay_file;

	if (lesh_log_file != nullptr && lesh_log_file[0] != '\0') {
		parsed.log_path = lesh_log_file;
		parsed.log_path_explicit = true;
	} else {
		parsed.log_path = default_log_path(xdg_state_home, home);
	}

	if (lesh_log == nullptr || lesh_log[0] == '\0')
		return parsed;

	const std::string_view request{lesh_log};
	size_t at = 0;
	const std::string_view level_name = next_piece(request, at, ':');

	level ceiling = level::off;
	if (!level_from_name(level_name, ceiling)) {
		parsed.malformed = true;
		return parsed;
	}
	if (ceiling == level::off)
		return parsed;

	if (at >= request.size()) {
		// No category list: every category, which is what `LESH_LOG=debug` plainly
		// means and what a user reaching for it first will expect.
		for (int one = 0; one < static_cast<int>(category::count_); ++one)
			parsed.enabled |= bits_for(ceiling, static_cast<category>(one));
		return parsed;
	}

	while (at < request.size()) {
		const std::string_view name = next_piece(request, at, ',');
		category in = category::loop;
		if (name.empty())
			continue;
		if (!category_from_name(name, in)) {
			parsed.malformed = true;
			continue;
		}
		parsed.enabled |= bits_for(ceiling, in);
	}
	return parsed;
}

bool configure(const settings& from, const options& with) {
	shutdown();

	bool complete = true;
	uint64_t bits = from.enabled;

	if (bits != 0) {
		// #98, and there is no flag for it: while the host owns the terminal the
		// text sink is a file or it is nothing, because a stray write to fd 2
		// corrupts the screen the user is reading the shell on. A non-interactive
		// shell that set a level and named no file gets stderr, which is what
		// `LESH_LOG=debug lesh -c ...` obviously wants.
		if (!with.interactive && !from.log_path_explicit) {
			text_sink().fd = STDERR_FILENO;
			text_sink().owned = false;
		} else if (from.log_path.empty() || !open_file_sink(text_sink(), from.log_path)) {
			complete = false;
			bits = 0;
		}
	}

	if (!from.replay_path.empty()) {
		if (open_file_sink(structured_sink(), from.replay_path))
			bits |= uint64_t{1} << kRecordingBit;
		else
			complete = false;
	}

	// TRACE NEVER ARMS IN A BUILD THAT COMPILED IT OUT. The call sites are gone,
	// so an armed bit could only mislead a reader of this word - and the release
	// preset is exactly where somebody would go looking for why `LESH_LOG=trace`
	// says nothing.
	if constexpr (!kTraceCompiledIn) {
		for (int one = 0; one < static_cast<int>(category::count_); ++one)
			bits &= ~(uint64_t{1} << gate_bit(level::trace, static_cast<category>(one)));
	}

	// RELEASE, not relaxed: this is the store that publishes the sinks the loads
	// on the other side will use. The loads are relaxed by design - see log.h -
	// so the release here is what makes a thread that observes an armed bit
	// certain to see the descriptor that goes with it.
	detail::gate.store(bits, std::memory_order_release);

	// THE STARTUP LINE, written whenever a text sink opened rather than gated on
	// a category. It is the header of the file, not a message about the loop: the
	// first thing a "leshper refused to start" report needs is the version, the
	// pid and what terminal it thought it had, and a user who set `LESH_LOG=error`
	// still needs to know which build produced the errors.
	//
	// `floor=` is the empty slot #97 fills. It is in the format now so that
	// filling it is a value change rather than a format change.
	if (text_sink().fd >= 0) {
		char line[kLineCapacity];
		char clock[16];
		const size_t stamp = format_clock(clock);
		const int written = std::snprintf(
			line, sizeof line, "%.*s INFO loop %llu: lesh %s pid=%d tty=%s floor=%s\n",
			static_cast<int>(stamp), clock,
			static_cast<unsigned long long>(this_thread_id()),
			with.version != nullptr ? with.version : kVersion,
			static_cast<int>(::getpid()),
			with.tty != nullptr ? with.tty : "-",
			with.floor != nullptr ? with.floor : "");
		if (written > 0)
			write_line(text_sink(), line, static_cast<size_t>(written));
	}

	// A MALFORMED `LESH_LOG` IS A FAILURE TO CONFIGURE, reported by the return
	// value rather than by a message. There is often nowhere for a message to go:
	// when the LEVEL is the part that did not parse no sink opens at all, and
	// interactively there is no stderr to fall back to (#98). So the complaint
	// goes in the log when there is a log, and the caller - which knows whether
	// it may speak to the terminal - is told either way.
	if (from.malformed) {
		LESH_LOG(level::error, category::loop,
		         "LESH_LOG named a level or category that does not exist");
		complete = false;
	}

	return complete;
}

bool configure_from_environment(const options& with) {
	return configure(settings_from_env(::getenv("LESH_LOG"), ::getenv("LESH_LOG_FILE"),
	                                   ::getenv("LESH_REPLAY_FILE"),
	                                   ::getenv("XDG_STATE_HOME"), ::getenv("HOME")),
	                 with);
}

void shutdown() noexcept {
	// The gate goes to zero FIRST. A call site that runs between these two lines
	// must see "off" rather than an open bit pointing at a descriptor that is
	// about to close - which is the one ordering in this file that is a
	// correctness question rather than a cost one.
	detail::gate.store(0, std::memory_order_release);
	text_sink().close();
	structured_sink().close();
}

int text_sink_fd() noexcept { return text_sink().fd; }
int structured_sink_fd() noexcept { return structured_sink().fd; }

// ---------------------------------------------------------------------------
// LOG_SAFE (#129): between fork and exec, and in a signal handler.
// ---------------------------------------------------------------------------

namespace safe {

int sink_fd() noexcept {
	// One plain int read out of a file-scope struct. No lock, no allocation, no
	// atomic - which is what makes this callable from a forked child whose
	// sibling threads may have been holding any lock in the process when the
	// address space was copied.
	return text_sink().fd;
}

void put(int fd, const char* bytes, size_t length) noexcept {
	if (fd < 0 || bytes == nullptr)
		return;
	size_t written = 0;
	while (written < length) {
		const ssize_t n = ::write(fd, bytes + written, length - written);
		if (n > 0) {
			written += static_cast<size_t>(n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		// Anything else and there is nothing a child between fork and exec can
		// do about it. Silence beats a second failing write.
		return;
	}
}

void put(int fd, long long value) noexcept {
	// 20 digits spans every int64_t, plus a sign. Fixed, on the stack, filled
	// backwards so no division-by-ten table and no reverse pass are needed.
	char digits[21];
	size_t at = sizeof(digits);
	const bool negative = value < 0;
	// Negated as unsigned so LLONG_MIN does not overflow on the way in.
	unsigned long long magnitude =
		negative ? 0ULL - static_cast<unsigned long long>(value)
		         : static_cast<unsigned long long>(value);
	do {
		digits[--at] = static_cast<char>('0' + (magnitude % 10));
		magnitude /= 10;
	} while (magnitude != 0 && at > 0);
	if (negative && at > 0)
		digits[--at] = '-';
	put(fd, digits + at, sizeof(digits) - at);
}

} // namespace safe

} // namespace lesh::log
