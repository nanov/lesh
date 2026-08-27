#pragma once

// TIER 2 OF THE TWO-TIER HISTORY: `history.new.log`, the append log (#192,
// ADR-0010 §Tier 2).
//
// Tier 1 is one file rewritten whole. Tier 2 is the thing that makes that
// affordable: every command line lands here as one frame, and the expensive
// rewrite happens once every 25 of them (#194). So this layer is written on the
// path where a shell has just been asked to run something, and read on the path
// where a shell is starting up - and both of those are paths where the process
// can die halfway.
//
// THE FRAME, and it is the whole format:
//
//     [u32 payload_len LE] [u32 crc32(payload)] [u8 format_version] [payload]
//
// Nine bytes and then the bytes, where the payload is one `record` as a
// standalone finished FlatBuffer - `record_writer` and `record_reader` in
// `blob.h`, the latter being where the eight-byte alignment these nine bytes
// destroy is handed back to FlatBuffers. Fixed-width, little-endian, no
// delimiter anywhere: THE FORMAT HAS NO ESCAPING PROBLEM, which is the entire
// reason it replaced a text scanner. A command line containing a newline, a
// NUL, or the four bytes of some other frame's header is just bytes inside a
// length the reader already knows.
//
// WHY A CRC AND NOT JUST A LENGTH. A length alone cannot tell a whole frame
// from a torn one whose first four bytes happen to parse; the CRC is what makes
// "this is a frame" a question with an answer, and it is what a resync can test
// a candidate header against. It is NOT a security property - it is a way to
// notice a half-written frame and the bytes a crash scribbled beside it.
//
// ONE WRITE PER FRAME, and that is a correctness requirement rather than a
// performance one. Two sibling shells append to this file with no lock (locking
// is #195's), so the only thing keeping their frames from being spliced into
// each other is POSIX's guarantee that a single `O_APPEND` write is atomic
// against every other appender. `append` therefore issues exactly one `writev`
// and treats a short write as a failure - never as something to finish with a
// second call, which is precisely the window the single call exists to close.
// `runtime/history_store.cpp` made the same argument for the file this replaces.
//
// NO LOCKING AND NO FILE HERE. `append` is handed a descriptor; who opened it,
// with which flags, under which `flock`, and what happens when it turns out to
// point at a vacuumed-away inode are #195's questions. `for_each` is handed
// bytes; who mapped or read them is #193's and #194's. This file knows framing
// and nothing else, and that is why it can be tested by flipping bytes in a
// `std::vector` rather than by orchestrating processes.

#include "ui/history/blob.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace lesh::ui::history {

// `[u32 len][u32 crc][u8 version]`, ahead of every payload.
inline constexpr std::size_t k_frame_header_bytes = 9;

// What `append` stamps into the version byte, and the only value `for_each`
// hands to a sink.
//
// A frame carrying anything else is SKIPPED, not resynced past: its length is
// still trustworthy (the CRC said so), so a future lesh's frames cost this one
// exactly nothing to walk over. That is the whole job of the byte - it buys the
// right to change the payload format later without making an older shell treat
// a newer shell's log as damage.
inline constexpr std::uint8_t k_log_format_version = 1;

// CRC32, IEEE 802.3, reflected: the polynomial 0xEDB88320, initial and final
// value 0xFFFFFFFF. Same function zlib's `crc32` computes, hand-rolled off a
// compile-time table because ADR-0010 says so and because a checksum is not
// worth a dependency. `crc32({})` is 0.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;

// How an `append` ended.
enum class append_status : std::uint8_t {
	// The whole frame reached the descriptor in one call.
	ok,
	// `writev` failed. `log_appender::error()` is the errno.
	write_failed,
	// It wrote SOMETHING, but not the whole frame - a full filesystem, or a
	// `RLIMIT_FSIZE` hit mid-frame. The file now ends in a partial frame, which
	// the reader will resync past; what must not happen is a second write to
	// finish it, because a sibling shell's frame may already have landed in
	// between. The caller's recourse is to report the loss, not to patch it up.
	short_write,
	// The serialized record does not fit a `u32` length. Unreachable with any
	// command line a terminal can deliver; named rather than asserted because
	// the length field is four bytes and something has to say what happens at
	// the edge of them.
	too_large,
};

// Appends frames to a descriptor somebody else owns.
//
// A CLASS AND NOT A FREE FUNCTION for one reason: the FlatBuffers builder that
// serializes the record. It is reused across appends, so the once-per-command
// path settles into no allocation at all rather than building and destroying a
// builder per command line. `error()` is the second reason - an errno wants
// somewhere to live that is not a return value the caller has to unpack.
//
// NOT THREAD-SAFE, and does not need to be: appends run on the loop thread
// (ADR-0009). Two PROCESSES appending to the same file is the case this format
// is built for; two threads sharing one appender is not a thing that happens.
class log_appender {
public:
	log_appender();
	~log_appender();

	log_appender(const log_appender&) = delete;
	log_appender& operator=(const log_appender&) = delete;
	log_appender(log_appender&&) noexcept;
	log_appender& operator=(log_appender&&) noexcept;

	// Serializes `one`, frames it, and writes the frame to `fd` with a single
	// `writev`. `fd` should have been opened `O_WRONLY|O_APPEND`; nothing here
	// seeks, so a descriptor without `O_APPEND` writes wherever it happens to
	// be, which is the caller's business and not this one's.
	//
	// `one`'s spans need only outlive the call.
	append_status append(int fd, const record& one);

	// The errno from the `writev` that failed, when the last `append` answered
	// `write_failed`; zero otherwise.
	[[nodiscard]] int error() const noexcept;

private:
	struct impl;
	std::unique_ptr<impl> _impl;
};

// What a walk over a log found. Counters, for a caller that wants to say
// something about a damaged file and for tests that want to say it precisely.
struct log_scan {
	// Frames handed to the sink: whole, CRC-checked, this version, and a
	// payload the FlatBuffers Verifier accepted as a `Record`.
	std::size_t frames = 0;
	// Whole, CRC-checked frames carrying a `format_version` this build does not
	// know. Walked over by their length; never handed to the sink.
	std::size_t skipped_frames = 0;
	// Every byte no frame claimed - the resync's leavings plus the torn tail.
	// Zero for a log written by this code and never damaged.
	std::size_t discarded_bytes = 0;
	// Of those, the ones after the last good frame: the torn tail on its own.
	// A crash during an append leaves exactly this and nothing else.
	std::size_t tail_bytes = 0;
};

// Walks `bytes` and calls `sink` once per good frame, in file order (which is
// APPEND ORDER, oldest first - the opposite of Tier 1's, because a log grows at
// the end and a blob is sorted before it is written).
//
// The record handed to `sink` BORROWS from `bytes`, exactly as `mapped_blob`'s
// do from a mapping: `cmd` and `cwd` point into the caller's buffer, and a sink
// that means to keep them copies them.
//
// THE THREE WAYS A LOG IS NOT A CLEAN SEQUENCE OF FRAMES, and what each costs:
//
//   - A TORN TAIL. The process died between the `writev` and the end of it, or
//     the disk filled. The last header's length runs off the end of the buffer,
//     the walk stops, and everything before it is kept - ADR-0010's rule, and
//     the reason an append never needs a commit record.
//
//   - A FRAME THAT DOES NOT CHECK OUT. The CRC disagrees, or agrees over bytes
//     the Verifier will not read as a `Record`. Both mean the same thing: the
//     reader is not where it thought it was. It RESYNCS - advances one byte at
//     a time until it finds a header whose length fits and whose CRC validates
//     the bytes it frames - and the payload is never searched for a delimiter,
//     because there are none to search for. Frames after the damage survive.
//
//   - AN UNKNOWN VERSION. Skipped by its length, counted, walk continues.
//
// THE ONE PLACE THIS READS ADR-0010 GENEROUSLY. The ADR states the torn-tail
// rule as "`offset + 9 + len > size` -> stop", and a length field is exactly
// where a single flipped byte turns a middle frame into a header claiming the
// rest of the file. Read literally, one bit rot in the middle of a log would
// discard every frame after it - and #192 requires the opposite in as many
// words ("corrupt one byte in every position of the middle frame - reader
// yields the other four"). So a length that does not fit is treated as what it
// is, a header that failed its check, and it resyncs. When the input really was
// truncated the resync finds nothing, the walk stops, and the behaviour is the
// ADR's to the byte; the generosity only shows up in the case the ADR does not
// describe.
log_scan for_each(std::span<const std::byte> bytes,
                  const std::function<void(const record&)>& sink);

} // namespace lesh::ui::history
