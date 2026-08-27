#include "ui/history/blob.h"

#include "ui/history/history_generated.h"

#include <cerrno>
#include <cstring>
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

[[nodiscard]] std::span<const std::byte> bytes_of(
	const ::flatbuffers::Vector<std::uint8_t>* vector) noexcept {
	if (vector == nullptr)
		return {};
	return {reinterpret_cast<const std::byte*>(vector->data()), vector->size()};
}

using record_vector = ::flatbuffers::Vector<::flatbuffers::Offset<fb::Record>>;

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
// Reading
// ---------------------------------------------------------------------------

mapped_blob::~mapped_blob() { close(); }

mapped_blob::mapped_blob(mapped_blob&& other) noexcept
	: _fd(std::exchange(other._fd, -1)),
	  _data(std::exchange(other._data, nullptr)),
	  _size(std::exchange(other._size, 0)),
	  _records(std::exchange(other._records, nullptr)),
	  _count(std::exchange(other._count, 0)),
	  _error(std::exchange(other._error, 0)) {}

mapped_blob& mapped_blob::operator=(mapped_blob&& other) noexcept {
	if (this != &other) {
		close();
		_fd = std::exchange(other._fd, -1);
		_data = std::exchange(other._data, nullptr);
		_size = std::exchange(other._size, 0);
		_records = std::exchange(other._records, nullptr);
		_count = std::exchange(other._count, 0);
		_error = std::exchange(other._error, 0);
	}
	return *this;
}

void mapped_blob::close() noexcept {
	if (_data != nullptr) {
		// The mapping is read-only and `munmap` does not modify it; the cast
		// is the POSIX signature's, not a licence to write.
		::munmap(const_cast<void*>(_data), _size);
		_data = nullptr;
	}
	if (_fd >= 0) {
		::close(_fd);
		_fd = -1;
	}
	_size = 0;
	_records = nullptr;
	_count = 0;
}

blob_status mapped_blob::open(const std::string& path) {
	// Whatever happens next, the old mapping goes: a caller that re-opens after
	// a vacuum must not be left holding the pre-vacuum file because the new one
	// failed to verify.
	close();
	_error = 0;

	// O_CLOEXEC IS NOT OPTIONAL HERE. This is a shell: it forks and execs on
	// every command line, and a history descriptor inherited by every child is
	// both a leak and a way for a child to read the user's history by accident.
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		_error = errno;
		return blob_status::io_error;
	}

	struct ::stat info {};
	if (::fstat(fd, &info) != 0) {
		_error = errno;
		::close(fd);
		return blob_status::io_error;
	}
	if (!S_ISREG(info.st_mode)) {
		// A directory or a device where `history.data` should be. Not ours to
		// interpret and certainly not ours to map.
		_error = EINVAL;
		::close(fd);
		return blob_status::io_error;
	}

	const auto size = static_cast<std::size_t>(info.st_size);

	// AN EMPTY FILE IS AN EMPTY HISTORY. `O_CREAT` in the append path makes a
	// zero-byte `history.data` the ordinary state of a shell that has never
	// vacuumed, and `mmap` of zero bytes is `EINVAL` anyway. Keep the
	// descriptor - #195 wants it for `file_id_t` - and report zero records.
	if (size == 0) {
		_fd = fd;
		return blob_status::ok;
	}

	// Too short to carry an identifier at all. Not mapped, not verified, and
	// above all NOT DESTROYED: whatever those bytes are, they are not a blob we
	// wrote, and the conservative treatment of "not ours" is the right one.
	if (size < k_identifier_end) {
		::close(fd);
		return blob_status::unknown_identifier;
	}

	void* const mapping = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapping == MAP_FAILED) {
		_error = errno;
		::close(fd);
		return blob_status::io_error;
	}

	_fd = fd;
	_data = mapping;
	_size = size;

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
