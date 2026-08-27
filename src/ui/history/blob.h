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
// WHAT IS NOT HERE, AND WHERE IT IS. The append log's framing, its CRC and its
// torn-tail resync are Tier 2 (`log.h`, #192); what this file lends Tier 2 is
// `record_writer` and `record_reader` below, the two halves of one record as a
// standalone buffer, because that is the payload and payloads are FlatBuffers'
// business rather than framing's. The in-memory deque, the merge walk and the
// `history_source` implementation are #193. The vacuum that PRODUCES a `history.data` - the LRU
// dedup, the 256 Ki cap, the temp-and-`rename` dance - is #194; this file is the
// serializer it will call and knows nothing about where the records came from or
// what order they deserve. Staleness - `file_id_t`, the directory watch, the
// remote-filesystem heap fallback - is #195, and it left exactly two marks on
// this file: `open_copied` below, and one `LOCK_SH` inside `open`.
//
// THAT LOCK IS THE SMALLEST ONE IN THE SUBSYSTEM and it is worth saying what it
// is not for. It is held across the `fstat` and the `mmap`/`read` and no longer
// (ADR-0010: "only long enough to get a consistent size"), and it is refused
// outright on a remote directory or once the process has given up locking - all
// of which is `locking.h`'s business, not this file's. What it does NOT protect
// against is a stale mapping, because nothing has to: a writer never truncates
// or modifies `history.data` in place (it renames a new file over it), so the
// worst a concurrent vacuum can do to a mapping is make it out of date, which
// #195 detects elsewhere and which never faults.
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
#include <functional>
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
// Writing ONE record - the Tier 2 payload
// ---------------------------------------------------------------------------

// Serializes one record as a STANDALONE finished FlatBuffer.
//
// This is what a Tier 2 log frame carries (`log.h`, #192; ADR-0010 §Tier 2 says
// it in one line: "payload = one Record as a standalone finished FlatBuffer").
// It lives HERE and not in `log.h` for the same reason the pimpl above exists:
// `log.h` would otherwise have to see flatbuffers in order to say what a
// payload is, and then so would everything that includes it. `log.cpp` includes
// this header and no generated one, and the framing layer never learns what is
// inside the bytes it frames.
//
// NOT `blob_writer` WITH A RANGE OF ONE. A `history.data` is rooted at a
// `HistoryFile` and carries the "SHH1" identifier; a frame payload is rooted at
// a bare `Record` and carries no identifier at all, because the frame header
// already said how long the bytes are and checksummed them, and four more per
// append buys nothing.
//
// WHICH MEANS THE TWO BUFFERS ARE NOT SELF-DESCRIBING APART, and it is worth
// being exact about that rather than comfortable. FlatBuffers tables are
// structurally permissive: hand a whole `history.data` to `record_reader` and
// it VERIFIES - the vector of records reads as a vector of `ubyte`, the absent
// fields read as their defaults - and you get a nonsense record rather than a
// refusal. Nothing bounds-checks its way out of that; what keeps it from ever
// happening is that the only way into `record_reader` is through a frame whose
// length and CRC already matched, and a blob is not a frame. The identifier is
// for FILES, which somebody can hand us by accident; a payload is reached by
// exactly one path.
//
// REUSED ACROSS BUILDS, like `blob_writer`, because an append happens once per
// command line: the builder grows its buffer once instead of per command.
class record_writer {
public:
	record_writer();
	~record_writer();

	record_writer(const record_writer&) = delete;
	record_writer& operator=(const record_writer&) = delete;
	record_writer(record_writer&&) noexcept;
	record_writer& operator=(record_writer&&) noexcept;

	// Serializes `one` and returns the finished buffer. The span is BORROWED
	// from this writer and is invalidated by the next `build` and by
	// destruction - and `one`'s own spans need only outlive this call.
	std::span<const std::byte> build(const record& one);

	// The buffer the last `build` produced; empty before the first one.
	[[nodiscard]] std::span<const std::byte> bytes() const noexcept;

private:
	struct impl;
	std::unique_ptr<impl> _impl;
};

// ---------------------------------------------------------------------------
// Reading ONE record - the Tier 2 payload
// ---------------------------------------------------------------------------

// Verifies a standalone finished `Record` and unpacks it, leaving its bytes
// where they lie.
//
// FALSE MEANS "DO NOT TRUST THESE BYTES", and it is the whole of what stands
// between a log frame whose CRC agrees by coincidence and a read off the end of
// the caller's buffer. #192's reader runs this on every frame it is about to
// yield and treats false exactly as it treats a CRC mismatch: the frame never
// happened, resync. A Verifier pass PER PAYLOAD here, unlike Tier 1's one pass
// per mapping, because a frame is a few hundred bytes and there is no mapping
// to amortise the pass over.
//
// A CLASS, AND THE REASON IS ALIGNMENT. FlatBuffers requires the buffer's first
// byte to be aligned to the widest scalar in the schema - `uint64`, so eight -
// and a Tier 2 frame payload starts NINE bytes into a file, which is aligned to
// nothing. The vendored runtime is half-prepared for that (`ReadScalar` carries
// `no_sanitize("alignment")` precisely so an odd address is survivable) and
// half not: `GetMutableRoot` and `Vector::length_` are plain dereferences, and
// UBSan says so on the first frame. So `read` copies the payload into an
// eight-aligned scratch, parses THERE, and then rebases the two borrowed spans
// back onto the caller's bytes by their offset - which is exact, because the
// scratch is a byte-for-byte copy. The scratch is a member so a walk over a log
// grows one buffer instead of one per frame.
//
// THE OUTPUT STILL BORROWS THE CALLER'S BYTES, not the scratch. That is the
// point of the rebase and it is what lets #193 and #194 walk a log without
// materialising a string per frame; a record read here outlives the next `read`
// exactly as long as the buffer it was read from does.
class record_reader {
public:
	record_reader();
	~record_reader();

	record_reader(const record_reader&) = delete;
	record_reader& operator=(const record_reader&) = delete;
	record_reader(record_reader&&) noexcept;
	record_reader& operator=(record_reader&&) noexcept;

	// Verifies `payload` and fills `out`, whose `cmd` and `cwd` then point INTO
	// `payload`. `out` is left untouched when this answers false.
	[[nodiscard]] bool read(std::span<const std::byte> payload, record& out);

private:
	struct impl;
	std::unique_ptr<impl> _impl;
};

// ---------------------------------------------------------------------------
// Reading a whole blob
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

// Verifies `bytes` as a whole `history.data` IMAGE and hands its records to
// `sink`, newest first, exactly as a mapping would.
//
// THIS EXISTS FOR THE VACUUM (#194) AND FOR NOTHING ELSE. ADR-0010 §Vacuum
// step 2 says the rewrite re-reads the old blob "via the fd (not the cached
// mmap)", and the reason is the whole protocol: step 1 snapshots the file_id of
// a descriptor it opened, and the records the rewrite merges have to be THAT
// descriptor's bytes, or the file_id it re-checks in step 3 is guarding
// contents it never read. `mapped_blob::open` takes a path and would race that
// snapshot; this takes bytes the caller already pulled off the fd.
//
// THE STATUS IS RECOMPUTED FROM THESE BYTES, which is the other half of what
// the vacuum needs. A `history.data` that verified at startup can have been
// replaced since by a file that does not, and the policy attached to
// `unknown_identifier` - never destroy it - has to be decided from what is
// there NOW, not from what was there when the session began.
//
// `bytes` MUST BE EIGHT-BYTE ALIGNED, which FlatBuffers requires of any root
// and which a `std::vector<std::byte>`'s storage satisfies by construction
// (plain `operator new` is aligned for every fundamental type). A mapping is
// page-aligned and also qualifies. `sink` is called zero or more times, never
// after this returns, and the spans it is handed point INTO `bytes`.
[[nodiscard]] blob_status read_records(std::span<const std::byte> bytes,
                                       const std::function<void(const record&)>& sink);

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

	// THE REMOTE-FILESYSTEM FALLBACK (#195, ADR-0010: "Never lock and never
	// `mmap` when the data dir is remote ... read Tier 1 into a heap buffer
	// instead"; fish PR #5097).
	//
	// Identical to `open` in every way a caller can observe - same statuses,
	// same one Verifier pass, same `records()` view, same borrowed spans, same
	// lifetime rule - except that the bytes come from `read(2)` into a heap
	// buffer rather than from `mmap`. NOTHING ABOVE THIS LINE BRANCHES ON WHICH
	// ONE WAS USED, which is the point: the merge walk, the searcher and the
	// autosuggester are pointer arithmetic over `records()` either way, and the
	// difference is one decision at `open` time instead of a condition on the
	// per-keystroke path.
	//
	// WHY NFS CANNOT BE MAPPED. The local-filesystem argument for `mmap` is that
	// a vacuum renames a new inode over the old one, so the pages a stale mapping
	// points at stay alive until the last reference goes. Across NFS the server
	// has no idea this client holds a mapping: it drops the file, and the next
	// page fault on it is a SIGBUS in a shell that did nothing wrong. A copy
	// cannot fault.
	//
	// THE PRICE IS THE COPY, once per (re)map, of a file capped at 256 Ki
	// records - and it is paid only where the alternative is a crash.
	blob_status open_copied(const std::string& path);

	// Releases the mapping - or the buffer - and the descriptor. Idempotent.
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

	// The whole of `open` and `open_copied`; `copy` chooses `read` over `mmap`
	// and nothing else differs. One body, so the identifier check, the Verifier
	// pass and the empty-file rule cannot drift apart between the two doors.
	blob_status open_impl(const std::string& path, bool copy);

	// Held open for the life of the mapping, not closed after `mmap`. ADR-0007
	// scores leaks by the count at exit and not by descriptor lifetime, and
	// #195 wants this fd to `fstat` for its `file_id_t` without reopening the
	// path and racing a vacuum.
	int _fd = -1;
	// The bytes. `_size` is their length; both are null/zero for an empty file,
	// which is mapped not at all - `mmap` of zero bytes is `EINVAL`.
	//
	// EITHER A MAPPING OR A HEAP BUFFER (#195's remote fallback, `open_copied`
	// above), and `_owns_buffer` is which. Everything that reads them is
	// identical for the two; only `close` cares, because one is `munmap`ped and
	// the other is freed.
	const void* _data = nullptr;
	std::size_t _size = 0;
	bool _owns_buffer = false;
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
