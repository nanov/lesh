#include "ui/history/log.h"

#include <array>
#include <cerrno>
#include <limits>

#include <sys/uio.h>
#include <unistd.h>

namespace lesh::ui::history {

namespace {

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------

// The reflected IEEE 802.3 polynomial. Reflected because the whole world's
// CRC32 is - zlib's, Ethernet's, gzip's - and a checksum that agreed with
// nobody would be a checksum nobody could check the implementation of.
constexpr std::uint32_t k_polynomial = 0xEDB8'8320u;

// Built by the COMPILER, not by a static initializer at process start: 256
// entries of eight shifts each is nothing, but it is nothing that would
// otherwise happen on the startup path of every shell, including the
// non-interactive ones that never open a history file at all.
[[nodiscard]] constexpr std::array<std::uint32_t, 256> build_crc32_table() noexcept {
	std::array<std::uint32_t, 256> table{};
	for (std::uint32_t byte = 0; byte < 256u; ++byte) {
		std::uint32_t remainder = byte;
		for (int bit = 0; bit < 8; ++bit)
			remainder = (remainder & 1u) ? ((remainder >> 1) ^ k_polynomial)
			                             : (remainder >> 1);
		table[byte] = remainder;
	}
	return table;
}

constexpr std::array<std::uint32_t, 256> k_crc32_table = build_crc32_table();

// ---------------------------------------------------------------------------
// The header, byte by byte
// ---------------------------------------------------------------------------

// LITTLE-ENDIAN BY HAND, not by pointer cast. The frame header sits at whatever
// offset the previous frame ended at, so it is aligned to nothing, and the file
// is meant to be readable on a machine that is not the one that wrote it. Four
// shifts cost nothing next to the CRC that follows and take both questions off
// the table.
[[nodiscard]] std::uint32_t read_u32_le(const std::byte* at) noexcept {
	return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(at[0]))
	     | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(at[1])) << 8)
	     | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(at[2])) << 16)
	     | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(at[3])) << 24);
}

void write_u32_le(std::byte* at, std::uint32_t value) noexcept {
	at[0] = static_cast<std::byte>(value & 0xFFu);
	at[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
	at[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
	at[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

// Is there a frame at `offset`, and if so how long is its payload and what
// version does it claim?
//
// This is the ONLY predicate in the file, and both the ordinary walk and the
// resync ask it - which is the point. A resync that tested candidates by a
// weaker rule than the walk uses would accept frames the walk would have
// rejected, and the two would disagree about what the file contains depending
// on whether a byte earlier in it had rotted.
//
// A ZERO LENGTH IS NEVER A FRAME. `crc32({})` is zero, so nine zero bytes are a
// header that validates itself perfectly - and a run of zeros is the single
// most likely thing to find in a damaged file, whether from a sparse hole, an
// `ftruncate` that grew the file, or a filesystem that lost a block. Refusing
// length zero costs a frame we could never have written (the smallest finished
// FlatBuffer is a dozen bytes) and takes away the one systematic way a resync
// could latch onto garbage.
[[nodiscard]] bool frame_at(std::span<const std::byte> bytes, std::size_t offset,
                            std::size_t& length, std::uint8_t& version) noexcept {
	const std::size_t left = bytes.size() - offset;
	if (left < k_frame_header_bytes)
		return false;

	const std::uint32_t claimed = read_u32_le(bytes.data() + offset);
	if (claimed == 0)
		return false;
	if (claimed > left - k_frame_header_bytes)
		return false;

	const auto payload = bytes.subspan(offset + k_frame_header_bytes, claimed);
	if (crc32(payload) != read_u32_le(bytes.data() + offset + 4))
		return false;

	length = claimed;
	version = std::to_integer<std::uint8_t>(bytes[offset + 8]);
	return true;
}

} // namespace

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
	std::uint32_t remainder = 0xFFFF'FFFFu;
	for (const std::byte one : bytes) {
		const auto index =
			static_cast<std::uint8_t>(remainder ^ std::to_integer<std::uint8_t>(one));
		remainder = k_crc32_table[index] ^ (remainder >> 8);
	}
	return remainder ^ 0xFFFF'FFFFu;
}

// ---------------------------------------------------------------------------
// Appending
// ---------------------------------------------------------------------------

struct log_appender::impl {
	// Serializes the record. A member so a shell that runs ten thousand
	// commands allocates one builder, not ten thousand.
	record_writer writer;
	int error = 0;
};

log_appender::log_appender() : _impl(std::make_unique<impl>()) {}
log_appender::~log_appender() = default;
log_appender::log_appender(log_appender&&) noexcept = default;
log_appender& log_appender::operator=(log_appender&&) noexcept = default;

int log_appender::error() const noexcept { return _impl->error; }

append_status log_appender::append(int fd, const record& one) {
	_impl->error = 0;

	const std::span<const std::byte> payload = _impl->writer.build(one);
	if (payload.size() > std::numeric_limits<std::uint32_t>::max())
		return append_status::too_large;

	std::array<std::byte, k_frame_header_bytes> header{};
	write_u32_le(header.data(), static_cast<std::uint32_t>(payload.size()));
	write_u32_le(header.data() + 4, crc32(payload));
	header[8] = static_cast<std::byte>(k_log_format_version);

	// TWO BUFFERS, ONE SYSCALL. `writev` is what lets the header and the payload
	// reach the file together without first memcpy'ing them into one buffer, and
	// POSIX gives a `writev` on an `O_APPEND` descriptor the same atomicity it
	// gives a `write`: the offset moves to the end and the whole vector lands
	// with no other appender's bytes in between. Two shells sharing this file
	// therefore interleave FRAMES, which the reader handles, rather than bytes,
	// which nothing could.
	//
	// The `const_cast` is `iovec`'s signature, which has one field for reads and
	// writes both; nothing here writes through it.
	std::array<::iovec, 2> parts{};
	parts[0].iov_base = header.data();
	parts[0].iov_len = header.size();
	parts[1].iov_base = const_cast<std::byte*>(payload.data());
	parts[1].iov_len = payload.size();

	const auto whole = static_cast<::ssize_t>(header.size() + payload.size());
	::ssize_t written = -1;
	do {
		written = ::writev(fd, parts.data(), static_cast<int>(parts.size()));
	} while (written == -1 && errno == EINTR);

	if (written < 0) {
		_impl->error = errno;
		return append_status::write_failed;
	}
	// NOT RETRIED. A second write would have to start from wherever the first
	// one stopped, and by then a sibling shell's frame may sit between the two
	// halves - the exact corruption the single call is here to prevent. The
	// partial frame stays in the file and the reader resyncs past it.
	if (written != whole)
		return append_status::short_write;
	return append_status::ok;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

log_scan for_each(std::span<const std::byte> bytes,
                  const std::function<void(const record&)>& sink) {
	log_scan result;

	// One reader for the walk, not one per frame: it owns the aligned scratch
	// every payload is verified in (`blob.h`), and a log with a thousand frames
	// should grow that buffer once.
	record_reader reader;

	std::size_t offset = 0;
	// Where the last frame this walk believed in ended. Everything past it at
	// the end of the walk is the torn tail, however it got that way.
	std::size_t after_last_frame = 0;

	while (offset < bytes.size()) {
		std::size_t length = 0;
		std::uint8_t version = 0;
		if (frame_at(bytes, offset, length, version)) {
			const auto payload = bytes.subspan(offset + k_frame_header_bytes, length);

			record one;
			if (version != k_log_format_version) {
				// Somebody else's format, framed by rules we still share. Its
				// length is as trustworthy as any other frame's, so walking over
				// it is not a guess.
				++result.skipped_frames;
			} else if (reader.read(payload, one)) {
				++result.frames;
				sink(one);
			} else {
				// THE CRC AGREED AND THE PAYLOAD IS NOT A RECORD. Either a
				// coincidence at a candidate header the resync should not have
				// stopped at, or a frame whose bytes were replaced wholesale
				// with something equally self-consistent. Neither is a frame,
				// and the safe reading is the pessimistic one: this position is
				// not a boundary, so fall through and resync from the next byte
				// exactly as a CRC mismatch does. Yielding it instead would put
				// bytes the Verifier refused in front of a caller that trusts
				// the spans it is handed.
				++offset;
				++result.discarded_bytes;
				continue;
			}

			offset += k_frame_header_bytes + length;
			after_last_frame = offset;
			continue;
		}

		// RESYNC, one byte at a time, and framing-only: the loop re-asks
		// `frame_at` at every offset and never looks at payload bytes as
		// anything but CRC input. Quadratic in the worst case - each candidate
		// costs a CRC over the length it claims - and that is accepted, because
		// the region between two good frames is a torn write or a damaged
		// block, not a file, and a shell pays it once at startup.
		++offset;
		++result.discarded_bytes;
	}

	result.tail_bytes = bytes.size() - after_last_frame;
	return result;
}

} // namespace lesh::ui::history
