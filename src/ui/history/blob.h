#pragma once

// TIER 1 OF THE TWO-TIER HISTORY: `history.data`, written and read (#191,
// ADR-0010 §Tier 1).
//
// Two halves and one type between them. `blob_writer` turns a range of records
// into one finished FlatBuffer; `mapped_blob` opens such a file, `mmap`s it,
// runs the Verifier ONCE, checks the identifier, and hands the records back as
// borrowed spans. `record` is what goes in and what comes out - one struct, not
// two, because a record on the way in and a record on the way out have the same
// five fields and the same borrowed-bytes discipline, and a second spelling of
// it would be the thing the two sides could disagree about.
//
// NOTHING IS COPIED ON THE READ PATH. `records()` is a view over the mapping:
// `cmd` and `cwd` are `std::span`s pointing INTO it, and the walk that #193
// runs on every keystroke is pointer arithmetic and nothing else. The price is
// the lifetime rule below, paid once here rather than per caller.
//
// WHY BYTES AND NOT STRINGS. A command line is whatever the user typed; a shell
// that cannot recall a command because it was not valid UTF-8 is a shell with a
// bug. The schema says `[ubyte]` (`history.fbs`) and this file says
// `std::span<const std::byte>` for the same reason, all the way up.
//
// WHAT IS NOT HERE, AND WHERE IT IS. The append log and its torn-tail resync are
// Tier 2 (#192). The in-memory deque, the merge walk and the `history_source`
// implementation are #193. The vacuum that PRODUCES a `history.data` - the LRU
// dedup, the 256 Ki cap, the temp-and-`rename` dance - is #194; this file is the
// serializer it will call and knows nothing about where the records came from or
// what order they deserve. Staleness - `file_id_t`, the directory watch, the
// remote-filesystem heap fallback - is #195. In particular there is NO locking
// here: `mapped_blob::open` takes no `flock`, because a writer never truncates
// or modifies `history.data` in place (it renames a new file over it), so the
// worst a concurrent vacuum can do to a mapping is make it stale, which is a
// state #195 detects and never one that faults.
//
// ADR-0007: `mapped_blob`'s destructor `munmap`s and closes. Every other member
// of both types is a self-freeing standard container or a `unique_ptr`, so the
// leak gate's expected count for this file is zero.
//
// FLATBUFFERS DOES NOT APPEAR IN THIS HEADER, and that is deliberate. The
// vendored runtime (`third_party/flatbuffers/`) and the committed
// `history_generated.h` are `blob.cpp`'s business; a consumer above this line
// includes `<span>` and gets bytes. `blob_writer` pays one heap allocation for
// the hidden builder, on the vacuum path, where it is free.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace lesh::ui::history {

// ---------------------------------------------------------------------------
// What a record is
// ---------------------------------------------------------------------------

// One history entry, with its bytes BORROWED.
//
// On the way in to `blob_writer::build`, `cmd` and `cwd` point at whatever the
// caller has and must outlive the call. On the way out of `mapped_blob`, they
// point into the mapping and are valid exactly as long as it is. Callers copy
// what they intend to keep - the same rule `history_search::match` already
// imposes, and for the same reason: a copy per record on the autosuggest path
// is an allocation per keystroke.
//
// `cmd` is REQUIRED by the schema and is therefore never empty in a record that
// came out of a verified blob. `cwd` is optional and comes back empty when the
// writer had none.
struct record {
	// The command line, raw. Newlines and all (F-34); never NUL-terminated.
	std::span<const std::byte> cmd;
	// Unix seconds at add time.
	std::uint64_t when = 0;
	// The LOGICAL `$PWD` bytes at add time. Empty when unknown.
	std::span<const std::byte> cwd;
	// The command's exit status, as `resolve_pending` saw it (#193).
	std::int32_t exit_code = 0;
	// Low 64 bits of the session's uuidv7. Exists only for the dedup
	// tie-break in #193's merge walk; it is not an identity anybody looks up.
	std::uint64_t session_id = 0;
};

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// Serializes records into one finished `history.data` buffer.
//
// NEWEST FIRST IS THE CALLER'S JOB, not this class's. `build` writes the range
// in the order it is given and does not sort, dedup, cap or timestamp anything:
// the order IS the format (ADR-0010 - `records[0]` is the most recent), and the
// side that knows which record is newest is the vacuum that merged them (#194).
// A writer that quietly re-sorted would be a second opinion about the format,
// held somewhere the vacuum's tests do not look.
//
// REUSED ACROSS BUILDS. The builder is a member, so the vacuum's periodic
// rewrite - at the 256 Ki cap, ~25 MB - grows its buffer once and then keeps it,
// instead of allocating a fresh one every 25 appends.
class blob_writer {
public:
	blob_writer();
	~blob_writer();

	blob_writer(const blob_writer&) = delete;
	blob_writer& operator=(const blob_writer&) = delete;
	blob_writer(blob_writer&&) noexcept;
	blob_writer& operator=(blob_writer&&) noexcept;

	// Serializes `newest_first` and returns the finished buffer - identifier
	// and all, ready to be written to a file byte for byte.
	//
	// The span is BORROWED from this writer and is invalidated by the next
	// `build` and by destruction. An empty range is not an error: it produces
	// a valid, verifiable blob with zero records, which is what a vacuum of an
	// empty history has to write.
	std::span<const std::byte> build(std::span<const record> newest_first);

	// The buffer the last `build` produced; empty before the first one.
	[[nodiscard]] std::span<const std::byte> bytes() const noexcept;

private:
	struct impl;
	std::unique_ptr<impl> _impl;
};

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

class mapped_blob;

// A random-access, zero-copy view over a mapping's records, NEWEST FIRST.
//
// Borrowed from the `mapped_blob` it came from: every record it yields points
// into that mapping, and both die when the blob is re-opened or destroyed.
// Range-for works, which is what the merge walk in #193 wants; `operator[]`
// works, which is what a bounded "scan the most recent N" wants.
class record_range {
public:
	class iterator {
	public:
		using difference_type = std::ptrdiff_t;
		using value_type = record;

		iterator() = default;
		iterator(const mapped_blob* blob, std::size_t index) noexcept
			: _blob(blob), _index(index) {}

		[[nodiscard]] record operator*() const noexcept;

		iterator& operator++() noexcept { ++_index; return *this; }
		iterator operator++(int) noexcept { auto copy = *this; ++_index; return copy; }

		friend bool operator==(const iterator&, const iterator&) noexcept = default;

	private:
		const mapped_blob* _blob = nullptr;
		std::size_t _index = 0;
	};

	record_range() = default;
	record_range(const mapped_blob* blob, std::size_t count) noexcept
		: _blob(blob), _count(count) {}

	[[nodiscard]] std::size_t size() const noexcept { return _count; }
	[[nodiscard]] bool empty() const noexcept { return _count == 0; }

	// Undefined for `index >= size()`, exactly as `std::span::operator[]` is.
	[[nodiscard]] record operator[](std::size_t index) const noexcept;

	[[nodiscard]] iterator begin() const noexcept { return {_blob, 0}; }
	[[nodiscard]] iterator end() const noexcept { return {_blob, _count}; }

private:
	const mapped_blob* _blob = nullptr;
	std::size_t _count = 0;
};

// How an `open` ended.
//
// UNKNOWN IDENTIFIER IS ITS OWN VALUE AND NOT A THROW, because it is the one
// failure with a policy attached that this layer does not get to decide: a
// `history.data` written by a future lesh, or by something else entirely, must
// NEVER be destroyed (ADR-0010). The caller in #193 refuses to vacuum, runs the
// session on Tier 2 plus memory, and warns once. Folding it into `corrupt`
// would hand a future format to the vacuum's `rename`, which is data loss; an
// exception would make the ordinary "somebody else's file" case cost a stack
// unwind on the startup path.
enum class blob_status : std::uint8_t {
	// Mapped and verified. `records()` is usable, possibly empty.
	ok,
	// `open`, `fstat` or `mmap` failed. `error()` is the errno. A missing file
	// is this, with `ENOENT` - the caller decides whether that is a first run
	// or a problem, and on a first run it simply is not one.
	io_error,
	// The file is not ours: at least eight bytes long, but the four at offset 4
	// are not "SHH1". A file too short to carry an identifier at all, but not
	// empty, is reported as this too - it cannot be shown to be ours, and the
	// conservative treatment of "not ours" is exactly the treatment it wants.
	unknown_identifier,
	// The identifier is ours and the Verifier rejected the buffer anyway:
	// truncated, garbled, or hand-edited.
	corrupt,
};

// `history.data`, open and mapped read-only.
//
// ONE VERIFIER PASS PER MAP, in `open`, over the whole file. Everything after it
// is pointer arithmetic that the pass has already licensed - that is the entire
// argument for the format, and running the Verifier again per read would throw
// it away. The corollary is that a `mapped_blob` is only as trustworthy as the
// bytes it verified: a writer that modified `history.data` IN PLACE could make a
// verified mapping lie. None does, and none may (ADR-0010: writers rename a new
// file over the old one), which is also why a stale mapping cannot SIGBUS on a
// local filesystem - the inode it maps is unlinked, not truncated.
//
// NOT THREAD-SAFE TO MUTATE, entirely safe to READ from anywhere. `open` and
// `close` run on the loop thread; `records()` and everything it yields are
// const and touch no shared state, which is what lets #193 hand a refcounted
// handle to a stateless worker (ADR-0009).
class mapped_blob {
public:
	mapped_blob() = default;
	~mapped_blob();

	mapped_blob(const mapped_blob&) = delete;
	mapped_blob& operator=(const mapped_blob&) = delete;
	mapped_blob(mapped_blob&& other) noexcept;
	mapped_blob& operator=(mapped_blob&& other) noexcept;

	// Opens `path` read-only, maps it, checks the identifier and verifies it.
	// Any previous mapping is released first, whatever the outcome, so a failed
	// re-open leaves an empty blob rather than the stale one.
	//
	// An EMPTY FILE (zero bytes) is `ok` with zero records, not an error: it is
	// the ordinary state of a `history.data` that has been created and not yet
	// vacuumed into, and reporting it as damage would make a first run look
	// like a corruption.
	blob_status open(const std::string& path);

	// Releases the mapping and the descriptor. Idempotent.
	void close() noexcept;

	// The errno from the syscall that failed, when the last `open` answered
	// `io_error`; zero otherwise.
	[[nodiscard]] int error() const noexcept { return _error; }

	// True when a mapping is held - i.e. the last `open` answered `ok`.
	[[nodiscard]] bool is_open() const noexcept { return _fd >= 0; }

	// Bytes of the file. Zero when nothing is open.
	[[nodiscard]] std::size_t size_bytes() const noexcept { return _size; }

	// The records, newest first. Empty - never invalid - when nothing is open.
	[[nodiscard]] record_range records() const noexcept { return {this, _count}; }

private:
	friend class record_range;

	// Out of line because unpacking a record needs the generated accessors, and
	// they are not in this header. Undefined for `index >= _count`.
	[[nodiscard]] record record_at(std::size_t index) const noexcept;

	// Held open for the life of the mapping, not closed after `mmap`. ADR-0007
	// scores leaks by the count at exit and not by descriptor lifetime, and
	// #195 wants this fd to `fstat` for its `file_id_t` without reopening the
	// path and racing a vacuum.
	int _fd = -1;
	// The mapping. `_size` is its length; both are null/zero for an empty file,
	// which is mapped not at all - `mmap` of zero bytes is `EINVAL`.
	// #195's remote-filesystem fallback reads into a heap buffer instead of
	// mapping; when it lands, these two describe that buffer and `close`
	// learns which kind it is holding.
	const void* _data = nullptr;
	std::size_t _size = 0;
	// The verified `HistoryFile`'s record vector, as `blob.cpp` knows it.
	// Null when there are no records; `_count` is the authority.
	const void* _records = nullptr;
	std::size_t _count = 0;
	int _error = 0;
};

inline record record_range::operator[](std::size_t index) const noexcept {
	return _blob->record_at(index);
}

inline record record_range::iterator::operator*() const noexcept {
	return _blob->record_at(_index);
}

} // namespace lesh::ui::history
