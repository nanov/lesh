#include "ui/history/history.h"

#include "ui/history/locking.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lesh::ui::history {

namespace {

// ---------------------------------------------------------------------------
// Bytes, both ways
// ---------------------------------------------------------------------------

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) noexcept {
	return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] std::string_view as_text(std::span<const std::byte> bytes) noexcept {
	return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// ---------------------------------------------------------------------------
// The session id (ADR-0010 §Tier 1: "low 64 bits of the session's uuidv7")
// ---------------------------------------------------------------------------

// Random bytes, from the kernel.
//
// `arc4random_buf` on Apple, `getentropy` elsewhere - both are in libc on the
// two platforms this builds for and neither needs a descriptor, which matters
// because this runs during startup and a shell that opened `/dev/urandom` would
// have to remember to close it. A failure is not fatal: a session id is a
// tie-break, so a degenerate one costs a dedup decision and never correctness.
void random_bytes(void* into, std::size_t count) noexcept {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	::arc4random_buf(into, count);
#else
	if (::getentropy(into, count) == 0)
		return;
	// The fallback nobody should ever reach: the clock and the pid, which are
	// not secret and are not meant to be. This is not a key.
	struct ::timespec now {};
	::clock_gettime(CLOCK_REALTIME, &now);
	const std::uint64_t mixed = static_cast<std::uint64_t>(now.tv_nsec) * 0x9E3779B97F4A7C15ull
	                            ^ static_cast<std::uint64_t>(::getpid());
	std::memset(into, 0, count);
	std::memcpy(into, &mixed, count < sizeof(mixed) ? count : sizeof(mixed));
#endif
}

// A uuidv7, both halves, written out in full even though only one is kept.
//
// RFC 9562 §5.7: 48 bits of unix milliseconds, then version 7, then 12 random
// bits (the high half); then variant 0b10 and 62 random bits (the low half).
// The timestamp is therefore ENTIRELY in the half this file discards, and
// saying so with the code rather than in a comment is why the whole value is
// built: the id we keep is 62 bits of entropy under two fixed variant bits, and
// a reader who assumed it was time-ordered would be wrong.
struct uuidv7 {
	std::uint64_t high = 0;
	std::uint64_t low = 0;
};

[[nodiscard]] uuidv7 generate_uuidv7() noexcept {
	std::uint64_t entropy[2] = {0, 0};
	random_bytes(entropy, sizeof(entropy));

	struct ::timespec now {};
	::clock_gettime(CLOCK_REALTIME, &now);
	const std::uint64_t millis =
		(static_cast<std::uint64_t>(now.tv_sec) * 1000ull
		 + static_cast<std::uint64_t>(now.tv_nsec) / 1000000ull)
		& 0x0000FFFFFFFFFFFFull;

	uuidv7 out;
	out.high = (millis << 16)                       // unix_ts_ms
	           | (std::uint64_t{0x7} << 12)         // ver
	           | (entropy[0] & 0x0FFFull);          // rand_a
	out.low = (entropy[1] & ~(std::uint64_t{0x3} << 62)) | (std::uint64_t{0x2} << 62);
	return out;
}

[[nodiscard]] std::uint64_t unix_now() noexcept {
	return static_cast<std::uint64_t>(std::time(nullptr));
}

// ---------------------------------------------------------------------------
// The data directory
// ---------------------------------------------------------------------------

// `mkdir -p`, with the leaf at `leaf_mode` and every parent at 0755.
//
// THE LEAF IS 0700 AND THE PARENTS ARE NOT, and that is deliberate rather than
// sloppy: `~/.local/share` is a directory the whole desktop shares and creating
// it 0700 on a machine that did not have one would be this shell deciding
// something for every other program. `~/.local/share/lesh` is ours alone, holds
// every command the user has typed, and is 0700 (ADR-0010 §Placement).
[[nodiscard]] bool make_directories(const std::string& path, ::mode_t leaf_mode) noexcept {
	if (path.empty())
		return false;

	// A trailing slash would otherwise make the LEAF a parent - the loop below
	// would never see a component whose end is the end of the string - and the
	// directory holding every command the user has typed would come out 0755.
	std::string_view trimmed{path};
	while (trimmed.size() > 1 && trimmed.back() == '/')
		trimmed.remove_suffix(1);

	std::string built;
	built.reserve(trimmed.size());
	std::size_t at = 0;
	while (at < trimmed.size()) {
		const std::size_t slash = trimmed.find('/', at);
		const std::size_t end = slash == std::string_view::npos ? trimmed.size() : slash;
		built.assign(trimmed, 0, end);
		at = end + 1;
		// A leading '/' makes the first component empty; so does a doubled
		// slash. Neither is a directory to create.
		if (built.empty() || built == "/")
			continue;
		const bool leaf = end == trimmed.size();
		if (::mkdir(built.c_str(), leaf ? leaf_mode : 0755) != 0 && errno != EEXIST)
			return false;
	}

	// EEXIST above says something is there; this says it is a directory we can
	// use. A regular file called `lesh` where the data directory belongs is the
	// case that would otherwise be discovered by `open` failing three times.
	struct ::stat info {};
	return ::stat(built.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Reads a whole file. Empty on any failure, which is the same answer as an
// empty log and wants the same handling.
[[nodiscard]] std::vector<std::byte> read_whole_file(const std::string& path) {
	std::vector<std::byte> out;
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return out;

	struct ::stat info {};
	if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
		::close(fd);
		return out;
	}

	out.resize(static_cast<std::size_t>(info.st_size));
	std::size_t filled = 0;
	while (filled < out.size()) {
		const ::ssize_t got = ::read(fd, out.data() + filled, out.size() - filled);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (got == 0)
			break;
		filled += static_cast<std::size_t>(got);
	}
	::close(fd);
	// A file that shrank between the `fstat` and the reads is a file somebody
	// truncated under us. Keep what arrived; the framing layer's torn-tail rule
	// covers the rest.
	out.resize(filled);
	return out;
}

// ---------------------------------------------------------------------------
// The per-walk dedup set
// ---------------------------------------------------------------------------

// How many times a `seen_commands` had to grow, process-wide. See
// `history::scratch_growths` for why this exists.
std::atomic<std::size_t> g_scratch_growths{0};

[[nodiscard]] std::uint64_t hash_bytes(std::string_view bytes) noexcept {
	// FNV-1a, 64-bit. Command lines are short and this is a dedup key, not a
	// checksum: what it has to be is cheap and allocation-free.
	std::uint64_t hash = 0xCBF29CE484222325ull;
	for (const char byte : bytes) {
		hash ^= static_cast<unsigned char>(byte);
		hash *= 0x100000001B3ull;
	}
	return hash;
}

// The command lines a walk has already yielded.
//
// GENERATION-STAMPED, WHICH IS THE WHOLE TRICK. Clearing a hash table between
// walks would be a memset of the whole table on a path that runs per keystroke;
// bumping an integer is not. A slot whose stamp is not the current generation
// is empty, whatever it holds - including the dangling `string_view` a previous
// walk left in it, which is never read because the stamp says the slot is free.
//
// OPEN ADDRESSING with linear probing and a power-of-two mask, kept at or below
// half load. It grows on demand and never shrinks, which is what makes the
// SECOND walk on a thread allocate nothing at all.
class seen_commands {
public:
	// Starts a walk. O(1), and the reason this class exists.
	void begin() noexcept {
		++_generation;
		_live = 0;
	}

	// True when `cmd` had not been seen in this walk - i.e. when the caller
	// should yield it. Borrows `cmd` for the rest of the walk.
	[[nodiscard]] bool insert(std::string_view cmd) {
		if (_slots.empty() || (_live + 1) * 2 > _slots.size())
			grow();

		const std::uint64_t hash = hash_bytes(cmd);
		const std::size_t mask = _slots.size() - 1;
		std::size_t at = static_cast<std::size_t>(hash) & mask;
		while (true) {
			slot& one = _slots[at];
			if (one.generation != _generation) {
				one = slot{_generation, hash, cmd};
				++_live;
				return true;
			}
			if (one.hash == hash && one.cmd == cmd)
				return false;
			at = (at + 1) & mask;
		}
	}

private:
	struct slot {
		std::uint64_t generation = 0;
		std::uint64_t hash = 0;
		std::string_view cmd;
	};

	void grow() {
		// 256 slots is 128 entries before the first growth, which covers every
		// walk the autosuggester makes (it stops at the first strict extension)
		// and most of what a search does. A history in the tens of thousands
		// pays a handful of doublings ONCE per thread and then never again.
		const std::size_t wanted = _slots.empty() ? 256 : _slots.size() * 2;
		std::vector<slot> fresh(wanted);
		const std::size_t mask = wanted - 1;
		for (const slot& one : _slots) {
			if (one.generation != _generation)
				continue;
			std::size_t at = static_cast<std::size_t>(one.hash) & mask;
			while (fresh[at].generation == _generation)
				at = (at + 1) & mask;
			fresh[at] = one;
		}
		_slots.swap(fresh);
		g_scratch_growths.fetch_add(1, std::memory_order_relaxed);
	}

	std::vector<slot> _slots;
	std::size_t _live = 0;
	// Starts at 1 so that a default-constructed slot (generation 0) is free.
	std::uint64_t _generation = 0;
};

// One dedup table per thread, reused across walks.
//
// THREAD-LOCAL AND NOT A MEMBER, because two workers can walk the same history
// at the same time and a member would be shared scratch on a lock-free read
// path. Not a stack array either, because a fixed capacity would mean a long
// history silently stops deduplicating - and "the newest wins" would become
// "the newest usually wins", which is not a rule.
//
// LSan sees this as reachable from thread-local storage, which it scans as a
// root, so ADR-0007's count-at-exit is unaffected.
thread_local seen_commands t_seen;
thread_local bool t_seen_in_use = false;

// Borrows the thread's table, or brings its own if a walk is already using it.
//
// A NESTED WALK IS NOT A THING THAT HAPPENS TODAY - the searcher's sink does
// not walk history - but the failure if it ever did would be silent and would
// look like a dedup bug three layers away. Five lines here instead.
class seen_scope {
public:
	seen_scope() {
		if (!t_seen_in_use) {
			t_seen_in_use = true;
			_owns_thread_local = true;
			_set = &t_seen;
		} else {
			_own.emplace();
			_set = &*_own;
		}
		_set->begin();
	}

	~seen_scope() {
		if (_owns_thread_local)
			t_seen_in_use = false;
	}

	seen_scope(const seen_scope&) = delete;
	seen_scope& operator=(const seen_scope&) = delete;

	[[nodiscard]] seen_commands& operator*() const noexcept { return *_set; }

private:
	seen_commands* _set = nullptr;
	std::optional<seen_commands> _own;
	bool _owns_thread_local = false;
};

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

history::history() : _session_id(generate_uuidv7().low) {
	// A VIEW BEFORE ANY CALLER CAN ASK FOR ONE. `for_each_newest_first` is
	// callable the instant this object exists - a worker could be handed the
	// source before `open` is called - so the empty view is published here
	// rather than left null for the walk to check.
	publish();
}

history::~history() {
	// ADR-0007. The mapping goes with the last view holding it; this is the
	// descriptor the appender writes through, which is ours alone.
	if (_log_fd >= 0)
		::close(_log_fd);
}

std::size_t history::scratch_growths() noexcept {
	return g_scratch_growths.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

std::optional<std::string> history::default_data_directory() {
	if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] == '/')
		return std::string{xdg} + "/lesh";
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
		return std::string{home} + "/.local/share/lesh";
	return std::nullopt;
}

open_report history::open(const std::string& directory) {
	open_report out;

	// A SECOND `open` REPLACES THE FIRST, descriptor and all. Nothing calls it
	// twice today; #195's reload will, and a leaked fd per reload is the kind
	// of thing ADR-0007's count-at-exit would find long after the change that
	// caused it.
	if (_log_fd >= 0) {
		::close(_log_fd);
		_log_fd = -1;
	}
	// A reload owed to the file the PREVIOUS `open` mapped is not owed to this
	// one: `map_tier1` below re-establishes both the mapping and its id.
	_reload_needed = false;

	if (!make_directories(directory, 0700)) {
		out.directory_unusable = true;
		return out;
	}

	_data_path = directory + "/history.data";
	_log_path = directory + "/history.new.log";

	// --- Remoteness, BEFORE anything opens or locks (#195) -------------------
	//
	// ADR-0010: "Never lock and never mmap when the data dir is remote." Both
	// halves are decided here, once, because a directory does not change
	// filesystems under a running shell - and because the lock verbs take a
	// descriptor and no path, so the answer has to be somewhere they can see it.
	_remote = is_remote(directory);
	set_data_directory_remote(_remote);
	out.directory_remote = _remote;

	// --- Tier 1 -------------------------------------------------------------
	out.tier1_mapped = map_tier1() == blob_status::ok;
	out.tier1_untouchable = _tier1_untouchable;
	out.tier1_corrupt = _tier1_corrupt;

	// --- Tier 2, read ---------------------------------------------------------
	load_log();
	out.log_frames = _log_frames;
	out.log_discarded_bytes = _log_discarded_bytes;

	// --- Tier 2, write --------------------------------------------------------
	_log_fd = ::open(_log_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	out.log_writable = _log_fd >= 0;

	// --- The directory watch (#195, fish #3565) -------------------------------
	//
	// LAST, and after the log has been created, so that this session's own two
	// files are already there and the first notification is somebody else's.
	// A failure is a degradation `open_report` names and nothing more.
	out.watching = _watch.open(directory);

	publish();
	return out;
}

// ---------------------------------------------------------------------------
// Tier 1's mapping
// ---------------------------------------------------------------------------

// Tier 1, opened and verified, with the two policy flags latched and the id
// cached.
//
// SHARED BY `open`, BY THE VACUUM'S REMAP AND BY THE WATCH'S RELOAD, which is
// the whole reason it is a function: a rewrite has to map its own output, a
// sibling's rewrite has to be noticed, and mapping through a second copy of
// these four cases is how the three would drift.
blob_status history::map_tier1() {
	// THE ID FIRST. See `_data_id`'s declaration: taking it before the open is
	// what makes a race here cost a wasted remap rather than a permanently stale
	// mapping.
	_data_id = file_id_of_path(_data_path);

	auto mapped = std::make_shared<mapped_blob>();
	// THE ONE BRANCH IN THE WHOLE SUBSYSTEM ON WHERE THE FILE LIVES (fish PR
	// #5097). Everything downstream - the view, the merge walk, the searcher -
	// reads `records()` and cannot tell which of these ran.
	const blob_status status =
		_remote ? mapped->open_copied(_data_path) : mapped->open(_data_path);

	// BOTH POLICY FLAGS ARE RE-DERIVED HERE AND NOWHERE ELSE, and they are
	// cleared before the switch rather than accumulated. `reload_tier1` runs
	// this again over DIFFERENT BYTES (#195), and a flag left standing from the
	// file we used to have would have `may_rewrite_tier1()` answering about a
	// file that no longer exists - which is exactly the case #194's rebuild
	// policy turns on.
	_tier1_untouchable = false;
	_tier1_corrupt = false;
	_blob.reset();
	switch (status) {
	case blob_status::ok:
		// `const` from here on: a view hands this to workers, and nothing above
		// this line will ever call a non-const member on it again.
		_blob = std::move(mapped);
		break;
	case blob_status::io_error:
		// A missing file is a first run and not a problem (`blob.h` says so in
		// as many words). Any other errno costs Tier 1 and nothing else: the
		// session runs on the log plus memory, exactly as it does before the
		// first vacuum has ever written a blob.
		break;
	case blob_status::unknown_identifier:
		// ADR-0010: NEVER DESTROY IT. A future lesh's file, or somebody else's.
		_tier1_untouchable = true;
		warn_once("is not a lesh history file",
		          "this session's history is the append log and these commands "
		          "only, and the file will not be rewritten");
		break;
	case blob_status::corrupt:
		// Ours, and the Verifier refused it. #194's decision (`vacuum.h`): the
		// next vacuum moves it aside and rebuilds, so this is a delay and not
		// the permanent loss of Tier 1 that refusing forever would be. NOT
		// untouchable - that flag is the unknown identifier alone.
		_tier1_corrupt = true;
		warn_once("did not verify",
		          "it will be renamed aside and rebuilt at the next vacuum, and "
		          "until then this session's history is the append log and "
		          "these commands only");
		break;
	}
	return status;
}

// READ WHOLE AND ONCE (at open, and again after this session's own vacuum). The
// log holds the frames since the last rewrite - ~`k_vacuum_frequency` of them -
// so this is a few kilobytes, and the alternative (re-reading per request) is
// exactly the per-keystroke file I/O ADR-0010 exists to remove.
void history::load_log() {
	const std::vector<std::byte> bytes = read_whole_file(_log_path);
	auto loaded = std::make_shared<std::vector<item>>();
	const log_scan scan = for_each(bytes, [&loaded](const record& one) {
		loaded->push_back(item{
			.cmd = std::string{as_text(one.cmd)},
			.cwd = std::string{as_text(one.cwd)},
			.when = one.when,
			.exit_code = one.exit_code,
			.session_id = one.session_id,
			// A frame on disk is a finished command by construction:
			// `resolve_pending` is the only thing that writes one.
			.mode = persist_mode::disk,
			.pending = false,
		});
	});
	// The log is APPEND ORDER, oldest first; the walk is newest first.
	std::reverse(loaded->begin(), loaded->end());
	_log_frames = scan.frames;
	_log_discarded_bytes = scan.discarded_bytes;
	_logged = std::move(loaded);
}

void history::reload_tier1() {
	// CLEARED FIRST, unconditionally. A reload that failed - the file is gone
	// again, or the new one does not verify - has still incorporated everything
	// there was to incorporate, and leaving the flag up would re-run it on every
	// `publish` for the rest of the session.
	_reload_needed = false;
	if (_data_path.empty())
		return;
	++_reloads;
	map_tier1();
	// THE LOG IS NOT RE-READ, and that is ADR-0010's wording rather than an
	// omission: "the next view build re-maps, re-verifies, re-caches the id".
	// The frames this session read at `open` are the same commands the vacuum
	// just folded into the blob, and the merge walk's dedup puts the log's copy
	// first, so the result is identical either way. Re-reading would also pull
	// in a sibling's UNVACUUMED frames, which is a different feature. OUR OWN
	// vacuum is not this path: it truncated the log it merged and re-reads it
	// itself, which is bookkeeping about a file this process just wrote.
}

void history::note_tier1_identity() {
	// PLAIN `==` AND NOT `file_id_equal`, and the difference is the point. The
	// helper refuses to call two failed `stat`s a match, which is what a TOCTOU
	// check needs; here two failures mean "there was no `history.data` before
	// and there is none now", which is a first run and is emphatically not a
	// change to reload for.
	if (file_id_of_path(_data_path) != _data_id)
		_reload_needed = true;
}

void history::incorporate_external_changes() {
	if (!_reload_needed)
		return;
	// `publish` is what consumes the flag - see its declaration.
	publish();
}

void history::drain_watch() {
	// CONSUME FIRST, whatever we decide afterwards. An undrained notification
	// leaves the descriptor readable and turns the loop's `poll` into a spin.
	if (!_watch.drain())
		return;
	note_tier1_identity();
	// STRAIGHT AWAY, on this thread, rather than at the next mutation. A session
	// that only reads has no next mutation - that IS fish #3565 - so the whole
	// point of the watch is that the new view is up before the next keystroke
	// asks for one.
	incorporate_external_changes();
}

void history::warn_once(const char* what, const char* consequence) {
	if (_warned)
		return;
	_warned = true;
	++_warnings;
	// STDERR, AND ONLY BEFORE THE EDITOR HAS THE TERMINAL. #98 forbids a
	// diagnostic written over a live edit line; `open` is called from `main`
	// before the session starts, which is what makes this one legal. Once,
	// because the condition does not change for the life of the process and a
	// shell that said it every prompt would be a shell nobody could use.
	std::fprintf(stderr, "lesh: %s %s; %s\n", _data_path.c_str(), what, consequence);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

add_status history::add(std::string_view cmd, std::string_view cwd, bool pending) {
	// fish #6032. A blank line is not history in any shell, and the rule lives
	// here rather than at the call site so that the second call site cannot get
	// it wrong.
	if (cmd.find_first_not_of(" \t\n\v\f\r") == std::string_view::npos)
		return add_status::rejected;

	// THE PREVIOUS EPHEMERAL ITEM, GONE. "Retrievable until the next add"
	// (ADR-0010) is exactly this line: a leading-space command can be recalled
	// with the up-arrow right after it runs, and stops existing the moment
	// another command is entered. A REJECTED add is not an add, which is why
	// this is below the check above.
	while (!_new_items.empty() && _new_items.back()->mode == persist_mode::ephemeral) {
		_new_items.pop_back();
		if (_first_unwritten > _new_items.size())
			_first_unwritten = _new_items.size();
	}

	// fish's rule, unconditionally, and it is a LITERAL SPACE - not any
	// whitespace. A tab-indented command line is history; the space is the
	// gesture users have been taught means "do not remember this".
	const persist_mode mode =
		cmd.front() == ' ' ? persist_mode::ephemeral : persist_mode::disk;

	// ADJACENT DUPLICATE (fish `history_item_t::merge`): same text, same persist
	// mode, so the item already at the back takes the newer timestamp instead of
	// a second item being appended.
	if (!_new_items.empty()) {
		const item& back = *_new_items.back();
		if (back.cmd == cmd && back.mode == mode) {
			auto merged = std::make_shared<item>(back);
			merged->when = std::max(back.when, unix_now());
			// THE NEWER CWD, because the merged item is the newer run: it is
			// about to be written again with the newer timestamp, and a record
			// whose `when` and `cwd` came from different runs would be a record
			// that never happened.
			merged->cwd.assign(cwd);
			merged->pending = pending;
			_new_items.back() = std::move(merged);
			// AND THE CURSOR REWINDS TO IT (fish `history_impl_t::add`), so the
			// newer timestamp actually reaches the log. Without this the merge
			// would be invisible on disk and the "keep the max timestamp" rule
			// would be a statement about memory only.
			if (_first_unwritten > _new_items.size() - 1)
				_first_unwritten = _new_items.size() - 1;
			publish();
			return add_status::merged;
		}
	}

	auto fresh = std::make_shared<item>(item{
		.cmd = std::string{cmd},
		.cwd = std::string{cwd},
		.when = unix_now(),
		.exit_code = 0,
		.session_id = _session_id,
		.mode = mode,
		.pending = pending,
	});
	_new_items.push_back(std::move(fresh));
	publish();
	return add_status::added;
}

void history::resolve_pending(std::int32_t exit_code) {
	// The pending item is always the back: `add` puts it there and nothing adds
	// behind it. Nothing pending is not an error - a cancelled line never
	// reached `add`, and a second call has nothing left to do.
	if (_new_items.empty() || !_new_items.back()->pending)
		return;

	// COPY-ON-WRITE. Views hold `shared_ptr<const item>`, so a walk that is in
	// flight on another thread is reading the item as it was; the resolution
	// replaces the pointer and only the NEXT view sees it.
	auto resolved = std::make_shared<item>(*_new_items.back());
	resolved->pending = false;
	resolved->exit_code = exit_code;
	_new_items.back() = std::move(resolved);

	// THIS IS THE POINT THAT APPENDS THE FRAME (ADR-0010 §Recording). Before the
	// wait there was no exit code to write, and after the process dies there is
	// nobody to write it - so a command reaches the disk exactly once, here.
	const bool appended = _new_items.back()->mode == persist_mode::disk;
	(void)flush();
	publish();

	// AND THIS IS THE POINT THAT COUNTS IT (ADR-0010 §Vacuum: "every 25
	// appends"). Only a `disk` item is an append - an ephemeral one never
	// reaches the file, and counting it would make the cadence depend on how
	// many secrets the user typed. After `publish`, so that a vacuum that
	// crashes leaves a view describing the command that was just recorded.
	if (appended)
		maybe_vacuum();
}

int history::open_locked_log() {
	if (_log_path.empty())
		return -1;

	// ADR-0010's loop, and fish's `save_internal_via_appending`: open the PATH,
	// lock what we got, and then ask whether the thing we locked is still the
	// thing at that path. A vacuum between the `open` and the `flock` would
	// otherwise have us appending, under a lock nobody else respects, to an
	// inode that has already been unlinked.
	//
	// 1024 TRIES, which is fish's `max_save_tries` order of magnitude and is a
	// bound rather than an expectation: the loop terminates on the first attempt
	// in every run that is not racing a vacuum in the same millisecond.
	constexpr int k_max_tries = 1024;
	for (int attempt = 0; attempt < k_max_tries; ++attempt) {
		// O_CREAT, unlike fish, because a vacuum here may have unlinked the log
		// after folding it into the blob - and a shell that then wrote through
		// its session-long descriptor would be writing into an orphan.
		const int fd =
			::open(_log_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
		if (fd < 0)
			return -1;

		// FALSE IS NOT A FAILURE (`locking.h`): the process may have given up on
		// locking, or the directory may be remote. The append proceeds either
		// way - the frame is one `writev` and POSIX makes that atomic against
		// other appenders, which is why the lock is an optimisation against
		// interleaved ORDER and not a correctness requirement.
		(void)lock_exclusive(fd);

		if (file_id_equal(file_id_of_path(_log_path), file_id_of_fd(fd)))
			return fd;

		unlock(fd);
		::close(fd);
	}
	return -1;
}

bool history::flush() {
	// NOTHING TO WRITE MEANS NO SYSCALLS. `save()` and `resolve_pending` both
	// call this unconditionally, and without the early-out an `exit` typed at a
	// prompt would open and lock the log to write nothing.
	// The cursor's own item decides it: the loop below stops at the first
	// pending one, so if the item AT the cursor is pending (or there is none),
	// there is nothing behind it either.
	if (_first_unwritten >= _new_items.size() || _new_items[_first_unwritten]->pending)
		return true;

	// THE APPEND-PATH IDENTITY CHECK (ADR-0010: "If the fd's id differs from the
	// cached id, another session vacuumed -> `reload_needed`"). One `stat` per
	// command line, on the path where a syscall is free - never per read, which
	// is the rule the whole two-tier design exists to keep.
	note_tier1_identity();

	// A FRESH, LOCKED DESCRIPTOR PER FLUSH, released by the close below. The
	// session's own `_log_fd` stays open as the answer to "is the log writable
	// at all" (`open_report::log_writable`, and the `_unwritable` accounting
	// below), and is the fallback when the dance could not be completed.
	const int locked_fd = open_locked_log();
	const int write_fd = locked_fd >= 0 ? locked_fd : _log_fd;

	bool complete = true;
	while (_first_unwritten < _new_items.size()) {
		const item& one = *_new_items[_first_unwritten];
		// A pending item is not ours to write, and neither is anything behind
		// it: the cursor stops here and picks up at the next `resolve_pending`.
		if (one.pending)
			break;

		if (one.mode == persist_mode::disk) {
			if (write_fd < 0) {
				// NOWHERE TO WRITE, and the cursor advances anyway. Holding it
				// back would make every later command retry a write that cannot
				// succeed - there is no path in this milestone that opens a log
				// after `open` failed to - and `save()` would then walk the
				// whole session on the way out for nothing.
				++_unwritable;
				complete = false;
			} else {
				const record framed{
					.cmd = as_bytes(one.cmd),
					.when = one.when,
					.cwd = as_bytes(one.cwd),
					.exit_code = one.exit_code,
					.session_id = one.session_id,
				};
				if (_appender.append(write_fd, framed) != append_status::ok) {
					// A full disk, or a frame that will not fit a u32 length.
					// Counted and stepped over for the reason above; `log.h` is
					// explicit that a short write must NOT be finished with a
					// second one, and a retry next command would be exactly
					// that with a sibling shell's frame possibly in between.
					++_unwritable;
					complete = false;
				}
			}
		}
		// `memory` and `ephemeral` items step the cursor without being written,
		// which is the whole of what those modes mean on this path.
		++_first_unwritten;
	}

	if (locked_fd >= 0) {
		// The close would release the lock on its own; the explicit `unlock`
		// says so at the point a reader is looking, and costs one syscall on a
		// path that just did four.
		unlock(locked_fd);
		::close(locked_fd);
	}
	return complete;
}

bool history::save() {
	// THE ANSWER IS ABOUT THE SESSION AND NOT ABOUT THIS CALL. `flush` reports
	// what it could not write NOW, but an item dropped three commands ago is
	// just as absent from the file, and "did the history get saved" is the only
	// question a caller on the way out is actually asking.
	//
	// AND NO VACUUM. ADR-0010: "`save()` on interactive exit flushes unwritten
	// items to the log and does not vacuum." The rewrite - the LRU dedup, the
	// 256 Ki cap, the temp-and-`rename` - is #194, and `may_rewrite_tier1()` is
	// the one question this milestone answers on its behalf.
	(void)flush();
	return _unwritable == 0;
}

// ---------------------------------------------------------------------------
// The vacuum (ADR-0010 §Vacuum)
// ---------------------------------------------------------------------------

void history::set_vacuum_hook(std::function<void(vacuum_step)> hook) {
	_vacuum_hook = std::move(hook);
}

void history::maybe_vacuum() {
	// No directory, no files, nothing to compact. A memory-only history - what
	// `vared` and most of the suite get - never reaches the rest of this, and
	// neither does one whose owner turned the periodic rewrite off.
	if (_data_path.empty() || !_automatic_vacuum)
		return;

	// fish `save_unless_disabled`: THE FIRST COUNTDOWN IS RANDOM in
	// `[0, k_vacuum_frequency)` and every later one is the full period. A fixed
	// start would mean a shell used for twenty commands and closed never
	// vacuums at all - which is most shells - and the log would grow forever on
	// exactly the machines nobody notices.
	if (_vacuum_countdown < 0) {
		std::uint64_t seed = 0;
		random_bytes(&seed, sizeof(seed));
		_vacuum_countdown =
			static_cast<int>(seed % static_cast<std::uint64_t>(k_vacuum_frequency));
	}
	if (_vacuum_countdown > 0) {
		--_vacuum_countdown;
		return;
	}

	// One less than the period, because this call IS the first of the next
	// twenty-five. fish's `countdown = kVacuumFrequency` followed by its
	// unconditional `countdown--` says the same thing in two statements.
	_vacuum_countdown = k_vacuum_frequency - 1;
	(void)vacuum_now();
}

vacuum_result history::vacuum_now() {
	++_vacuums;
	if (_data_path.empty())
		return vacuum_result{.status = vacuum_status::refused};

	// EVERY RESOLVED, WRITABLE ITEM - see `vacuum_request::session` for why the
	// unwritten ones alone are not enough. The spans borrow `_new_items`, which
	// nothing touches until `vacuum` returns.
	std::vector<record> session;
	session.reserve(_new_items.size());
	for (const std::shared_ptr<const item>& one : _new_items) {
		if (one->pending || one->mode != persist_mode::disk)
			continue;
		session.push_back(record{
			.cmd = as_bytes(one->cmd),
			.when = one->when,
			.cwd = as_bytes(one->cwd),
			.exit_code = one->exit_code,
			.session_id = one->session_id,
		});
	}

	const vacuum_result done = vacuum(vacuum_request{
		.data_path = _data_path,
		.log_path = _log_path,
		.session = session,
		.policy = may_rewrite_tier1() ? tier1_policy::rewritable
		                              : tier1_policy::untouchable,
		.on_step = _vacuum_hook,
	});

	switch (done.status) {
	case vacuum_status::refused:
		// Not ours. Nothing was touched and nothing is owed.
		return done;
	case vacuum_status::gave_up:
		// ADR-0010 step 3: "on give-up, do not drop data: fall back to plain
		// append". IN A TWO-TIER DESIGN THAT IS THE TIER 2 APPEND, and `flush`
		// is it - so the fallback is to make sure everything unwritten is in
		// the log and to try the rewrite again in another period. There is
		// nothing to append to `history.data`; it is a blob, not a log.
		(void)flush();
		return done;
	case vacuum_status::renamed:
		break;
	}

	// --- Everything below happens ONLY after a successful rename -------------

	// THE LOG, AND ONLY IF IT IS STILL WHAT THE VACUUM MERGED. A sibling shell
	// that appended a frame after the vacuum read the log would have that frame
	// truncated away without it ever having reached the blob - the one path in
	// this design that could lose a resolved command. Leaving the log alone
	// instead costs duplicates, which the merge walk hides and the next vacuum
	// removes. `vacuum_result::log_bytes_merged` carries the length.
	if (_log_fd >= 0) {
		const file_id_t now = file_id_of_path(_log_path);
		if (now != k_invalid_file_id && now.size == done.log_bytes_merged)
			(void)::ftruncate(_log_fd, 0);
	}

	// THE CURSOR AND THE ITEMS (ADR-0010 as amended by #193): a successful
	// vacuum clears `new_items` in the same `publish()` that maps the new blob,
	// which is legal because `session_id` and not `new_items` tells own items
	// from foreign ones. Without it `publish()` would be O(items this session)
	// per command line for the whole life of the shell.
	//
	// A FILTER AND NOT A TRUNCATION, though, because "written" is not the same
	// as "in the blob": a `memory` item advanced the cursor without being
	// written anywhere, and dropping it here would be forgetting it. A pending
	// item is behind the cursor by construction and is kept for the same
	// reason.
	std::deque<std::shared_ptr<const item>> keeping;
	std::size_t cursor = 0;
	for (std::size_t at = 0; at < _new_items.size(); ++at) {
		const std::shared_ptr<const item>& one = _new_items[at];
		const bool below_cursor = at < _first_unwritten;
		if (below_cursor && !one->pending && one->mode == persist_mode::disk)
			continue;
		keeping.push_back(one);
		if (below_cursor)
			++cursor;
	}
	_new_items = std::move(keeping);
	_first_unwritten = cursor;

	// THE REMAP. #195 owns the flag - its directory watch is the other thing
	// that sets it - and here the reload happens in the same call, because a
	// writer that did not map its own output would keep serving the history it
	// replaced.
	_reload_needed = true;
	(void)map_tier1();
	load_log();
	_reload_needed = false;

	publish();
	if (_vacuum_hook)
		_vacuum_hook(vacuum_step::published);
	return done;
}

// ---------------------------------------------------------------------------
// The snapshot view
// ---------------------------------------------------------------------------

void history::publish() {
	// THE RELOAD, CONSUMED HERE AND NOWHERE ELSE (#195). Every route to a stale
	// mapping - the watch's drain, the append path's id check - sets one flag,
	// and the one place that builds a view is the one place that clears it.
	if (_reload_needed)
		reload_tier1();

	auto fresh = std::make_shared<view>();
	fresh->own.reserve(_new_items.size());
	// NEWEST FIRST, PENDING EXCLUDED. Both are the walk's contract, and doing
	// them here means the walk itself is a straight loop with no predicate -
	// which is what keeps the per-keystroke path as short as it is.
	for (auto it = _new_items.rbegin(); it != _new_items.rend(); ++it) {
		if (!(*it)->pending)
			fresh->own.push_back(*it);
	}
	fresh->logged = _logged;
	fresh->blob = _blob;
	std::atomic_store(&_view, std::shared_ptr<const view>{std::move(fresh)});
}

// A TRAILING RETURN TYPE, because `view` is private: written the other way
// round the return type is looked up at namespace scope, before the declarator
// says which class this member belongs to.
auto history::snapshot() const noexcept -> std::shared_ptr<const view> {
	return std::atomic_load(&_view);
}

// ---------------------------------------------------------------------------
// The merge walk
// ---------------------------------------------------------------------------

void history::for_each_merged_newest_first(
	const std::function<bool(const merged_entry&)>& fn) const {
	// ONCE, AT THE TOP. Everything below reads a graph that nothing will modify
	// - a mutation on the loop thread builds a new view and swaps the pointer,
	// and this one keeps its mapping alive by refcount until the walk returns.
	const std::shared_ptr<const view> taken = snapshot();
	if (!taken)
		return;

	const seen_scope seen;
	merged_entry yielding;

	// 1. THIS SESSION, newest first. First because of the tie-break: "on equal
	//    `when`, own `session_id` wins" (ADR-0010 §Read path) is not a
	//    comparison anywhere in this function, it is this ordering. An own item
	//    is seen before any copy of it, so the dedup keeps the own one.
	yielding.from = merged_entry::origin::session;
	for (const std::shared_ptr<const item>& one : taken->own) {
		const std::string_view cmd{one->cmd};
		if (!(*seen).insert(cmd))
			continue;
		yielding.what = record{
			.cmd = as_bytes(cmd),
			.when = one->when,
			.cwd = as_bytes(one->cwd),
			.exit_code = one->exit_code,
			.session_id = one->session_id,
		};
		if (!fn(yielding))
			return;
	}

	// 2. THE LOG, newest first. Every frame in it was written before this
	//    session started - the log is read once, at `open`, and a reload is
	//    #195 - so it is uniformly older than anything above and uniformly
	//    newer than anything below, which is why a concatenation and not a
	//    merge sort is the right shape here.
	if (taken->logged) {
		yielding.from = merged_entry::origin::log;
		for (const item& one : *taken->logged) {
			const std::string_view cmd{one.cmd};
			if (!(*seen).insert(cmd))
				continue;
			yielding.what = record{
				.cmd = as_bytes(cmd),
				.when = one.when,
				.cwd = as_bytes(one.cwd),
				.exit_code = one.exit_code,
				.session_id = one.session_id,
			};
			if (!fn(yielding))
				return;
		}
	}

	// 3. THE MAPPING, which is already newest first because the format says so.
	//    Pointer arithmetic and no copies - `record_range` yields spans into the
	//    pages, and this loop adds a hash and a compare per entry.
	if (taken->blob) {
		yielding.from = merged_entry::origin::blob;
		for (const record& one : taken->blob->records()) {
			if (!(*seen).insert(as_text(one.cmd)))
				continue;
			yielding.what = one;
			if (!fn(yielding))
				return;
		}
	}
}

void history::for_each_newest_first(const std::function<bool(std::string_view)>& fn) const {
	for_each_merged_newest_first([&fn](const merged_entry& one) {
		return fn(as_text(one.what.cmd));
	});
}

} // namespace lesh::ui::history
