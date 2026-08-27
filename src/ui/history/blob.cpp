#include "ui/history/blob.h"

#include "ui/history/history_generated.h"
#include "ui/history/locking.h"

#include <cerrno>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lesh::ui::history {

namespace {

// The Verifier counts every table it walks and gives up past this many.
//
// ADR-0010 caps `history.data` at `HISTORY_SAVE_MAX` = 256 Ki records, one
// table each, plus the root: 262,145. The flatbuffers default happens to be
// 1,000,000, which covers that with room to spare - but "happens to" is not a
// contract, and a future cap raise that silently started rejecting our own
// valid files would be a bad afternoon. So the limit is stated here, in terms
// of the cap, with slack: four times the cap is still a hard ceiling against a
// hostile file claiming millions of tables, and a file over it is a file we
// could not have written.
constexpr std::uint32_t k_verifier_max_tables = 4u * 256u * 1024u;

// The identifier lives at bytes [4, 8) of a finished buffer, so anything
// shorter cannot be asked the question at all.
constexpr std::size_t k_identifier_end = 8;

// What a finished buffer's first byte must be aligned to: the widest scalar in
// `history.fbs`, which is `uint64`. `mmap` gives a page and is therefore free;
// #195's heap fallback has to ask for it, and this is the number it asks for.
constexpr std::size_t k_blob_alignment = 8;

[[nodiscard]] std::span<const std::byte> bytes_of(
	const ::flatbuffers::Vector<std::uint8_t>* vector) noexcept {
	if (vector == nullptr)
		return {};
	return {reinterpret_cast<const std::byte*>(vector->data()), vector->size()};
}

using record_vector = ::flatbuffers::Vector<::flatbuffers::Offset<fb::Record>>;

// THE SHARED LOCK, held for exactly as long as the size and the bytes are being
// read (#195; ADR-0010: "mapping Tier 1 takes `LOCK_SH` only long enough to get
// a consistent size"; fish takes the same lock around the same window).
//
// RAII because `open_impl` has seven exits and six of them are failures, and a
// history file left locked by an error path would be a shell that hangs the next
// terminal's vacuum. Taking the lock may be REFUSED - the process has given up,
// or the directory is remote - and that is not a failure: the constructor
// records what happened and the destructor undoes only what was done.
class shared_lock_guard {
public:
	explicit shared_lock_guard(int fd) noexcept : _fd(lock_shared(fd) ? fd : -1) {}
	~shared_lock_guard() {
		if (_fd >= 0)
			unlock(_fd);
	}

	shared_lock_guard(const shared_lock_guard&) = delete;
	shared_lock_guard& operator=(const shared_lock_guard&) = delete;

private:
	int _fd = -1;
};

// A descriptor that closes itself, so that the lock above is always released
// while its descriptor is still open.
//
// THAT ORDERING IS THE WHOLE REASON THIS TYPE EXISTS, and it is not a nicety: an
// `flock(LOCK_UN)` on a descriptor that has already been closed is at best
// `EBADF` and at worst an unlock of whatever some other thread opened into the
// same slot in between. Nesting the lock guard inside this one's scope makes
// "unlock, then close" the destruction order rather than a rule each exit path
// has to remember.
class fd_guard {
public:
	explicit fd_guard(int fd) noexcept : _fd(fd) {}
	~fd_guard() {
		if (_fd >= 0)
			::close(_fd);
	}

	fd_guard(const fd_guard&) = delete;
	fd_guard& operator=(const fd_guard&) = delete;

	[[nodiscard]] int get() const noexcept { return _fd; }
	// Hands ownership to the caller; the destructor then does nothing.
	[[nodiscard]] int release() noexcept { return std::exchange(_fd, -1); }

private:
	int _fd = -1;
};

} // namespace

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// The builder and the one scratch vector it needs, kept off `blob.h` so that
// nothing above this file has to see flatbuffers.
struct blob_writer::impl {
	::flatbuffers::FlatBufferBuilder builder;
	// One offset per record, in the order they will appear. A member so the
	// vacuum's rewrite does not allocate it fresh every time.
	std::vector<::flatbuffers::Offset<fb::Record>> offsets;
};

blob_writer::blob_writer() : _impl(std::make_unique<impl>()) {}
blob_writer::~blob_writer() = default;
blob_writer::blob_writer(blob_writer&&) noexcept = default;
blob_writer& blob_writer::operator=(blob_writer&&) noexcept = default;

std::span<const std::byte> blob_writer::build(std::span<const record> newest_first) {
	auto& builder = _impl->builder;
	builder.Clear();

	auto& offsets = _impl->offsets;
	offsets.clear();
	offsets.reserve(newest_first.size());

	// Two passes' worth of work in one loop, and the ORDER inside it matters:
	// FlatBuffers builds bottom-up, so a table's child vectors have to be
	// finished before its own table is started. Creating `cmd` while a Record
	// is open is the classic way to get an assertion out of the builder.
	for (const record& one : newest_first) {
		const auto cmd = builder.CreateVector(
			reinterpret_cast<const std::uint8_t*>(one.cmd.data()), one.cmd.size());
		// `cwd` is optional: a zero offset leaves the field out of the vtable
		// entirely, which is a byte cheaper than an empty vector and reads
		// back as an empty span either way.
		const auto cwd = one.cwd.empty()
			? ::flatbuffers::Offset<::flatbuffers::Vector<std::uint8_t>>()
			: builder.CreateVector(
				reinterpret_cast<const std::uint8_t*>(one.cwd.data()), one.cwd.size());

		offsets.push_back(fb::CreateRecord(builder, cmd, one.when, cwd,
		                                   one.exit_code, one.session_id));
	}

	const auto vector = builder.CreateVector(offsets);
	// `Finish` with the identifier, not the plain one: the four bytes are what
	// makes this file recognisably ours, and a blob written without them would
	// come back from `mapped_blob::open` as somebody else's.
	fb::FinishHistoryFileBuffer(builder, fb::CreateHistoryFile(builder, vector));

	return bytes();
}

std::span<const std::byte> blob_writer::bytes() const noexcept {
	const auto& builder = _impl->builder;
	if (builder.GetSize() == 0)
		return {};
	return {reinterpret_cast<const std::byte*>(builder.GetBufferPointer()),
	        builder.GetSize()};
}

// ---------------------------------------------------------------------------
// Writing ONE record - the Tier 2 payload
// ---------------------------------------------------------------------------

// One builder and nothing else. A single record has no vector of offsets to
// stage, so unlike `blob_writer::impl` there is no scratch beside it.
struct record_writer::impl {
	::flatbuffers::FlatBufferBuilder builder;
};

record_writer::record_writer() : _impl(std::make_unique<impl>()) {}
record_writer::~record_writer() = default;
record_writer::record_writer(record_writer&&) noexcept = default;
record_writer& record_writer::operator=(record_writer&&) noexcept = default;

std::span<const std::byte> record_writer::build(const record& one) {
	auto& builder = _impl->builder;
	builder.Clear();

	// Children before the table, as in `blob_writer::build`: FlatBuffers builds
	// bottom-up and creating a vector while a table is open is an assertion.
	const auto cmd = builder.CreateVector(
		reinterpret_cast<const std::uint8_t*>(one.cmd.data()), one.cmd.size());
	const auto cwd = one.cwd.empty()
		? ::flatbuffers::Offset<::flatbuffers::Vector<std::uint8_t>>()
		: builder.CreateVector(
			reinterpret_cast<const std::uint8_t*>(one.cwd.data()), one.cwd.size());

	// PLAIN `Finish`, deliberately: no file identifier. "SHH1" answers "is this
	// file ours" for `history.data`, which is a file somebody may hand us by
	// accident; a frame payload is never met on its own - it is reached only
	// through a frame header whose length and CRC already vouched for it - and
	// four bytes per append to re-answer a question nobody asks is four bytes
	// per append. `record_reader` therefore verifies with a null identifier, and
	// the ROOT TYPE is what keeps the two buffers from being confused.
	builder.Finish(fb::CreateRecord(builder, cmd, one.when, cwd, one.exit_code,
	                                one.session_id));
	return bytes();
}

std::span<const std::byte> record_writer::bytes() const noexcept {
	const auto& builder = _impl->builder;
	if (builder.GetSize() == 0)
		return {};
	return {reinterpret_cast<const std::byte*>(builder.GetBufferPointer()),
	        builder.GetSize()};
}

// ---------------------------------------------------------------------------
// Reading ONE record - the Tier 2 payload
// ---------------------------------------------------------------------------

struct record_reader::impl {
	// EIGHT-ALIGNED BY CONSTRUCTION. `std::allocator<std::byte>` goes through
	// plain `operator new`, whose result is aligned for every type with
	// fundamental alignment - sixteen bytes on both platforms we build for, and
	// never less than the eight `uint64` needs. `resize` inside the existing
	// capacity keeps the same allocation, so the guarantee survives reuse.
	std::vector<std::byte> aligned;
};

record_reader::record_reader() : _impl(std::make_unique<impl>()) {}
record_reader::~record_reader() = default;
record_reader::record_reader(record_reader&&) noexcept = default;
record_reader& record_reader::operator=(record_reader&&) noexcept = default;

bool record_reader::read(std::span<const std::byte> payload, record& out) {
	// Below the smallest buffer FlatBuffers can produce there is nothing the
	// Verifier could accept, and asking first keeps a null `data()` out of the
	// `reinterpret_cast` below. Parenthesised because the macro expands to a
	// sum of `sizeof`s.
	if (payload.size() < (FLATBUFFERS_MIN_BUFFER_SIZE))
		return false;

	// THE COPY, and `blob.h` argues for it at length: a frame payload begins
	// nine bytes into a file, FlatBuffers wants its buffer eight-aligned, and
	// two places in the vendored runtime dereference through a typed pointer
	// without the `no_sanitize("alignment")` that `ReadScalar` carries. This is
	// the read path of a startup and of a vacuum - not of a keystroke, which
	// reads Tier 1's mapping and is page-aligned by `mmap` - so a memcpy of a
	// few hundred bytes per frame buys correctness at a price nothing measures.
	auto& aligned = _impl->aligned;
	aligned.resize(payload.size());
	std::memcpy(aligned.data(), payload.data(), payload.size());

	::flatbuffers::Verifier::Options options;
	options.max_tables = k_verifier_max_tables;
	::flatbuffers::Verifier verifier(
		reinterpret_cast<const std::uint8_t*>(aligned.data()), aligned.size(), options);
	// A NULL IDENTIFIER, and the root type doing the work. A `history.data`
	// handed here fails: its root is a `HistoryFile`, whose only field is a
	// vector, and reading that vtable as a `Record`'s does not survive the pass.
	if (!verifier.VerifyBuffer<fb::Record>(nullptr))
		return false;

	const fb::Record* const one =
		::flatbuffers::GetRoot<fb::Record>(aligned.data());
	if (one == nullptr)
		return false;

	// REBASED, not copied. Every byte of the scratch is the byte at the same
	// offset of `payload`, so a pointer into one is an offset into the other;
	// the spans that go out therefore point at the CALLER's bytes and the
	// scratch is free to be overwritten by the next frame. `cmd` is `(required)`
	// in the schema, so the pass above already refused a buffer without it.
	const auto* const base = aligned.data();
	const auto rebased =
		[base, payload](const ::flatbuffers::Vector<std::uint8_t>* vector)
		-> std::span<const std::byte> {
		if (vector == nullptr)
			return {};
		const auto* const at = reinterpret_cast<const std::byte*>(vector->data());
		return payload.subspan(static_cast<std::size_t>(at - base), vector->size());
	};

	out = record{
		.cmd = rebased(one->cmd()),
		.when = one->when(),
		.cwd = rebased(one->cwd()),
		.exit_code = one->exit_code(),
		.session_id = one->session_id(),
	};
	return true;
}

// ---------------------------------------------------------------------------
// Reading a whole blob
// ---------------------------------------------------------------------------

blob_status read_records(std::span<const std::byte> bytes,
                         const std::function<void(const record&)>& sink) {
	// AN EMPTY IMAGE IS AN EMPTY HISTORY, the same answer `mapped_blob::open`
	// gives a zero-byte file: `O_CREAT` in the vacuum's step 1 makes exactly
	// that on a first run, and calling it damage would make a first vacuum
	// look like a recovery.
	if (bytes.empty())
		return blob_status::ok;
	if (bytes.size() < k_identifier_end)
		return blob_status::unknown_identifier;

	// The two checks in the order `mapped_blob::open` runs them, and for the
	// reason stated there: the identifier and the Verifier have opposite
	// policies attached, so the caller cannot tell them apart unless we do.
	if (!fb::HistoryFileBufferHasIdentifier(bytes.data()))
		return blob_status::unknown_identifier;

	::flatbuffers::Verifier::Options options;
	options.max_tables = k_verifier_max_tables;
	::flatbuffers::Verifier verifier(
		reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), options);
	if (!fb::VerifyHistoryFileBuffer(verifier))
		return blob_status::corrupt;

	const fb::HistoryFile* const root = fb::GetHistoryFile(bytes.data());
	const record_vector* const records = root->records();
	if (records == nullptr)
		return blob_status::ok;

	// Spans into `bytes`, like everywhere else on this read path. The vacuum
	// keys its dedup on them and never copies a command line it is going to
	// evict, which is what keeps a 256 Ki rewrite from also being 256 Ki
	// allocations.
	for (::flatbuffers::uoffset_t at = 0; at < records->size(); ++at) {
		const fb::Record* const one = records->Get(at);
		if (one == nullptr)
			continue;
		const record yielding{
			.cmd = bytes_of(one->cmd()),
			.when = one->when(),
			.cwd = bytes_of(one->cwd()),
			.exit_code = one->exit_code(),
			.session_id = one->session_id(),
		};
		sink(yielding);
	}
	return blob_status::ok;
}

mapped_blob::~mapped_blob() { close(); }

mapped_blob::mapped_blob(mapped_blob&& other) noexcept
	: _fd(std::exchange(other._fd, -1)),
	  _data(std::exchange(other._data, nullptr)),
	  _size(std::exchange(other._size, 0)),
	  _owns_buffer(std::exchange(other._owns_buffer, false)),
	  _records(std::exchange(other._records, nullptr)),
	  _count(std::exchange(other._count, 0)),
	  _error(std::exchange(other._error, 0)) {}

mapped_blob& mapped_blob::operator=(mapped_blob&& other) noexcept {
	if (this != &other) {
		close();
		_fd = std::exchange(other._fd, -1);
		_data = std::exchange(other._data, nullptr);
		_size = std::exchange(other._size, 0);
		_owns_buffer = std::exchange(other._owns_buffer, false);
		_records = std::exchange(other._records, nullptr);
		_count = std::exchange(other._count, 0);
		_error = std::exchange(other._error, 0);
	}
	return *this;
}

void mapped_blob::close() noexcept {
	if (_data != nullptr) {
		if (_owns_buffer) {
			// #195's remote fallback. Allocated with the matching aligned
			// `operator new` below; the size and the alignment are both part of
			// the sized-delete contract and both have to match.
			::operator delete(const_cast<void*>(_data), _size,
			                  std::align_val_t{k_blob_alignment});
		} else {
			// The mapping is read-only and `munmap` does not modify it; the cast
			// is the POSIX signature's, not a licence to write.
			::munmap(const_cast<void*>(_data), _size);
		}
		_data = nullptr;
	}
	if (_fd >= 0) {
		::close(_fd);
		_fd = -1;
	}
	_size = 0;
	_owns_buffer = false;
	_records = nullptr;
	_count = 0;
}

blob_status mapped_blob::open(const std::string& path) { return open_impl(path, false); }

blob_status mapped_blob::open_copied(const std::string& path) {
	return open_impl(path, true);
}

blob_status mapped_blob::open_impl(const std::string& path, bool copy) {
	// Whatever happens next, the old mapping goes: a caller that re-opens after
	// a vacuum must not be left holding the pre-vacuum file because the new one
	// failed to verify.
	close();
	_error = 0;

	// O_CLOEXEC IS NOT OPTIONAL HERE. This is a shell: it forks and execs on
	// every command line, and a history descriptor inherited by every child is
	// both a leak and a way for a child to read the user's history by accident.
	//
	// OWNED BY A GUARD until the last line of this function, which is the only
	// place the object takes it over. Every failure below is then a plain
	// `return`, and the descriptor and the shared lock go back in the right
	// order without any of them saying so.
	fd_guard owned{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
	if (owned.get() < 0) {
		_error = errno;
		return blob_status::io_error;
	}

	// THE BYTES ARE TAKEN UNDER THE SHARED LOCK and the verification is not
	// (#195; ADR-0010: "only long enough to get a consistent size"). A verifier
	// pass over 25 MB is not a window another shell's vacuum should have to wait
	// out, and it does not need to: once the mapping exists, the inode it points
	// at cannot be modified by anyone - a writer renames a new file over the
	// name, it never writes through the old one.
	{
		const shared_lock_guard reading{owned.get()};

		struct ::stat info {};
		if (::fstat(owned.get(), &info) != 0) {
			_error = errno;
			return blob_status::io_error;
		}
		if (!S_ISREG(info.st_mode)) {
			// A directory or a device where `history.data` should be. Not ours
			// to interpret and certainly not ours to map.
			_error = EINVAL;
			return blob_status::io_error;
		}

		const auto size = static_cast<std::size_t>(info.st_size);

		// AN EMPTY FILE IS AN EMPTY HISTORY. `O_CREAT` in the append path makes
		// a zero-byte `history.data` the ordinary state of a shell that has
		// never vacuumed, and `mmap` of zero bytes is `EINVAL` anyway. Keep the
		// descriptor - #195 uses it for `file_id_t` - and report zero records.
		if (size == 0) {
			_fd = owned.release();
			return blob_status::ok;
		}

		// Too short to carry an identifier at all. Not mapped, not verified, and
		// above all NOT DESTROYED: whatever those bytes are, they are not a blob
		// we wrote, and the conservative treatment of "not ours" is the right
		// one.
		if (size < k_identifier_end)
			return blob_status::unknown_identifier;

		if (copy) {
			// THE REMOTE FALLBACK. Eight-byte alignment is FlatBuffers'
			// requirement on the root (`uint64` is the widest scalar in
			// `history.fbs`), and it is what `mmap` gave for free; a plain
			// `new std::byte[]` would satisfy it on both platforms this builds
			// for and would be relying on `__STDCPP_DEFAULT_NEW_ALIGNMENT__` to
			// keep doing so, where the aligned form says the requirement out
			// loud and UBSan checks it.
			void* const buffer = ::operator new(
				size, std::align_val_t{k_blob_alignment}, std::nothrow);
			if (buffer == nullptr) {
				_error = ENOMEM;
				return blob_status::io_error;
			}
			std::size_t filled = 0;
			while (filled < size) {
				const ::ssize_t got = ::read(
					owned.get(), static_cast<std::byte*>(buffer) + filled,
					size - filled);
				if (got < 0) {
					if (errno == EINTR)
						continue;
					_error = errno;
					::operator delete(buffer, size,
					                  std::align_val_t{k_blob_alignment});
					return blob_status::io_error;
				}
				if (got == 0)
					break;
				filled += static_cast<std::size_t>(got);
			}
			// A FILE THAT SHRANK BETWEEN THE `fstat` AND THE READS is a file
			// somebody replaced under us - which over NFS is the ordinary case
			// this whole path exists for. The short buffer is verified like any
			// other: the Verifier refuses a truncated blob and the caller gets
			// `corrupt`, never a read off the end of uninitialised bytes.
			_data = buffer;
			_size = filled;
			_owns_buffer = true;
			if (filled < k_identifier_end) {
				close();
				return blob_status::unknown_identifier;
			}
		} else {
			void* const mapping =
				::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, owned.get(), 0);
			if (mapping == MAP_FAILED) {
				_error = errno;
				return blob_status::io_error;
			}
			_data = mapping;
			_size = size;
		}
	}

	// THE IDENTIFIER FIRST, and separately from the Verifier. `VerifyBuffer`
	// checks it too, but folds the answer into one boolean - and the two
	// answers have opposite policies attached (ADR-0010): a wrong identifier is
	// a file to leave alone, a failed verification of OUR identifier is a file
	// to rebuild. The caller cannot tell them apart unless we do.
	if (!fb::HistoryFileBufferHasIdentifier(_data)) {
		close();
		return blob_status::unknown_identifier;
	}

	// THE ONE VERIFIER PASS. Everything `record_at` does afterwards is pointer
	// arithmetic this pass licensed; nothing re-checks per read.
	::flatbuffers::Verifier::Options options;
	options.max_tables = k_verifier_max_tables;
	::flatbuffers::Verifier verifier(static_cast<const std::uint8_t*>(_data), _size,
	                                 options);
	if (!fb::VerifyHistoryFileBuffer(verifier)) {
		close();
		return blob_status::corrupt;
	}

	const fb::HistoryFile* const root = fb::GetHistoryFile(_data);
	const record_vector* const records = root->records();
	if (records != nullptr) {
		_records = records;
		_count = records->size();
	}
	// LAST, and only here: everything above this line could still fail, and a
	// descriptor the object had already adopted would be closed by `close()` on
	// one path and by the guard on another.
	_fd = owned.release();
	return blob_status::ok;
}

record mapped_blob::record_at(std::size_t index) const noexcept {
	if (_records == nullptr)
		return {};

	const auto* const vector = static_cast<const record_vector*>(_records);
	const fb::Record* const one =
		vector->Get(static_cast<::flatbuffers::uoffset_t>(index));
	if (one == nullptr)
		return {};

	// Spans INTO the mapping. Nothing is copied but the five-field view itself,
	// which is registers.
	return record{
		.cmd = bytes_of(one->cmd()),
		.when = one->when(),
		.cwd = bytes_of(one->cwd()),
		.exit_code = one->exit_code(),
		.session_id = one->session_id(),
	};
}

} // namespace lesh::ui::history
