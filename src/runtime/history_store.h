#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace lesh::runtime {

// The v1 HistoryStore (#94, #113): a dumb, append-only log file behind the
// `HistoryStore` override point A-13 names. It is storage only - append and
// newest-first snapshot iteration, nothing else. Search (F-32/F-33) is
// leshper's, built as filters over one iteration using C-6's lexer for token
// mode; this class does not know what a search is and must not grow one.
//
// Interactive-only (#101): whether to build one AT ALL is the caller's
// decision, made once at startup - a non-interactive shell (`-c`, a script,
// stdin) must touch no rc and no history file, and this class has no way to
// know which kind of shell it was constructed by. It does not check for
// itself; the caller must not construct one for a non-interactive shell.
//
// File location: `default_path()` mirrors the `~/.leshrc` decision (#101) -
// one dotfile directly in `$HOME`, no XDG lookup yet.
//
// Concurrent shells, the v1 answer: append-only `O_APPEND` writes, last-wins
// reads. Two shells appending at once interleave safely - see `append()` for
// why - but there is no merge, no lock file, no dedup. A snapshot iteration
// simply reads whatever is on disk at the moment it is called; an append a
// sibling shell makes after that moment is invisible to it and always will
// be - that is "last wins", not a promise that concurrent shells ever agree.
// The atuin-direction (rich metadata, sync, dedup) is future work arriving
// BEHIND this same interface (owner's noted trajectory on #94); this class
// must not be shaped in anticipation of it.
//
// Newlines (F-34): a recalled multiline entry must reconstruct with its
// newlines intact, not flattened. The file format is therefore one PHYSICAL
// line per entry: an embedded newline is escaped to the two bytes `\` `n`,
// and a literal backslash is escaped to `\` `\`, before the entry is
// written. See `history_store.cpp` for the exact grammar and what a
// malformed (hand-edited, truncated, or foreign) line decodes to.
//
// ADR-0007: this class holds no file descriptor and no arena allocation
// across calls - only `std::string` members, which free themselves on
// destruction - so the leak gate's expected count of this store is zero
// without any explicit cleanup.
class history_store {
public:
	// Does not touch the filesystem: opening (and creating, if necessary)
	// happens per call, in `append()` and `for_each_newest_first()`.
	explicit history_store(std::string path) : _path{std::move(path)} {}

	history_store(const history_store&) = delete;
	history_store& operator=(const history_store&) = delete;

	// Appends one entry, newlines and all. A failed open or a failed write is
	// reported back rather than thrown or asserted - a full disk, a missing
	// parent directory, or a permissions problem must not take the shell
	// down. Returns false on any failure; the entry is simply not recorded.
	bool append(std::string_view entry);

	// Calls `fn(entry)` once per stored entry, newest first, where `entry` is
	// the original (unescaped, newlines restored) text. Re-reads the file
	// fresh on every call - a concurrent sibling shell's append becomes
	// visible on the NEXT call, never mid-walk. A missing file iterates zero
	// entries, not an error.
	void for_each_newest_first(const std::function<void(std::string_view)>& fn) const;

	// The v1 default location: `~/.lesh_history`. Returns `nullopt` when
	// `$HOME` is unset or empty - exactly the condition under which
	// `~/.leshrc` also cannot be located.
	[[nodiscard]] static std::optional<std::string> default_path();

private:
	std::string _path;
	// Reused across `append()` calls so a steady stream of appends - one per
	// accepted command line - settles into zero further heap allocation
	// rather than allocating an encode buffer fresh each time.
	std::string _scratch;
};

} // namespace lesh::runtime
