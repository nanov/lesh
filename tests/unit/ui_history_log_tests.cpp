#include "ui/history/log.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace lesh::ui::history;

// TIER 2 OF THE TWO-TIER HISTORY (#192, ADR-0010 §Tier 2): the append log's
// framing, and the three ways a file that was being appended to when a machine
// lost power is not a clean sequence of frames.
//
// The round trip is four tests and the damage is the rest, which is the right
// proportion: a format whose only job is to survive a crash mid-write is a
// format whose tests are almost all about crashes mid-write. Two of them are
// exhaustive rather than representative - EVERY truncation offset of a five
// frame file, EVERY byte of its middle frame flipped - because the bug this
// format exists to prevent is a read off the end of a buffer at one offset
// nobody thought of, and a sampled sweep is a sweep that misses it. Under the
// debug preset each of those runs the whole reader under ASan and UBSan, which
// is the point of the ticket and not a side effect of it.

namespace {

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) noexcept {
	return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] std::string as_text(std::span<const std::byte> bytes) {
	return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] record make_record(std::string_view cmd, std::uint64_t when = 0,
                                 std::string_view cwd = {}, std::int32_t exit_code = 0,
                                 std::uint64_t session_id = 0) {
	return record{
		.cmd = as_bytes(cmd),
		.when = when,
		.cwd = as_bytes(cwd),
		.exit_code = exit_code,
		.session_id = session_id,
	};
}

// The log is opened exactly as the real appender's owner will open it (#195):
// append-only, close-on-exec, 0600. Nothing here seeks, so `O_APPEND` is what
// puts each frame at the end.
[[nodiscard]] int open_log(const std::string& path) {
	return ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
}

[[nodiscard]] std::vector<std::byte> read_file(const std::string& path) {
	std::ifstream in{path, std::ios::binary};
	const std::string text{std::istreambuf_iterator<char>(in),
	                       std::istreambuf_iterator<char>()};
	std::vector<std::byte> out(text.size());
	if (!text.empty())
		std::memcpy(out.data(), text.data(), text.size());
	return out;
}

[[nodiscard]] std::size_t file_size(const std::string& path) {
	struct ::stat info {};
	if (::stat(path.c_str(), &info) != 0)
		return 0;
	return static_cast<std::size_t>(info.st_size);
}

[[nodiscard]] std::uint32_t read_u32_le(std::span<const std::byte> bytes,
                                        std::size_t at) noexcept {
	return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at]))
	     | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + 1])) << 8)
	     | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + 2])) << 16)
	     | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + 3])) << 24);
}

void append_u32_le(std::vector<std::byte>& out, std::uint32_t value) {
	out.push_back(static_cast<std::byte>(value & 0xFFu));
	out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
	out.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
	out.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
}

void flip(std::vector<std::byte>& bytes, std::size_t at) {
	bytes[at] = static_cast<std::byte>(std::to_integer<unsigned>(bytes[at]) ^ 0xA5u);
}

// What a walk found, in a form that outlives the buffer it walked. The records
// `for_each` yields BORROW from the input, so a test that kept them would be
// testing its own lifetime discipline rather than the reader's.
struct walked {
	log_scan scan;
	std::vector<std::string> commands;
	std::vector<std::uint64_t> whens;
};

[[nodiscard]] walked collect(std::span<const std::byte> bytes) {
	walked out;
	out.scan = for_each(bytes, [&](const record& one) {
		out.commands.push_back(as_text(one.cmd));
		out.whens.push_back(one.when);
	});
	return out;
}

// A log on disk plus the offsets of its frame boundaries, which is what the two
// exhaustive tests need in order to say what the RIGHT answer is at each offset
// rather than merely that the reader survived.
struct written_log {
	std::vector<std::byte> bytes;
	// `frame_end[i]` is the offset just past frame `i` - so a file truncated to
	// `frame_end[i]` contains exactly frames 0..i whole.
	std::vector<std::size_t> frame_end;
	std::vector<std::string> commands;
};

[[nodiscard]] written_log write_log(const std::string& path,
                                    const std::vector<std::string>& commands) {
	written_log out;
	out.commands = commands;

	const int fd = open_log(path);
	EXPECT_GE(fd, 0) << "could not open " << path << ", errno " << errno;
	if (fd < 0)
		return out;

	log_appender appender;
	for (std::size_t i = 0; i < commands.size(); ++i) {
		const record one = make_record(commands[i], 1'000 + i, "/home/dn/src/lesh",
		                               static_cast<std::int32_t>(i), 42);
		EXPECT_EQ(appender.append(fd, one), append_status::ok) << "frame " << i;
		out.frame_end.push_back(file_size(path));
	}
	::close(fd);

	out.bytes = read_file(path);
	return out;
}

[[nodiscard]] std::vector<std::string> numbered(std::size_t count) {
	std::vector<std::string> out;
	out.reserve(count);
	for (std::size_t i = 0; i < count; ++i)
		out.push_back("git rebase --onto main feature/" + std::to_string(i));
	return out;
}

} // namespace

// ---------------------------------------------------------------------------
// The checksum
// ---------------------------------------------------------------------------

TEST(UiHistoryLog, Crc32AgreesWithTheStandardVector) {
	// "123456789" -> 0xCBF43926 is the check value every CRC32/ISO-HDLC
	// implementation in the world publishes. A hand-rolled table (ADR-0010: no
	// zlib) that got the reflection or the final xor wrong would still be
	// self-consistent - it would round trip perfectly and agree with nothing -
	// so the test that matters is the one against somebody else's number.
	EXPECT_EQ(crc32(as_bytes("123456789")), 0xCBF4'3926u);
	EXPECT_EQ(crc32(as_bytes("")), 0u);
	EXPECT_EQ(crc32(as_bytes("a")), 0xE8B7'BE43u);
	EXPECT_EQ(crc32(as_bytes(std::string_view{"\x00", 1})), 0xD202'EF8Du);
}

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST(UiHistoryLog, EmptyInputYieldsNothing) {
	// A `history.new.log` that has been created and never written to. Not
	// damage, not a torn tail: nothing.
	const walked out = collect({});
	EXPECT_EQ(out.scan.frames, 0u);
	EXPECT_EQ(out.scan.skipped_frames, 0u);
	EXPECT_EQ(out.scan.discarded_bytes, 0u);
	EXPECT_EQ(out.scan.tail_bytes, 0u);
	EXPECT_TRUE(out.commands.empty());
}

TEST(UiHistoryLog, OneFrameRoundTripsEveryField) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.new.log");

	const int fd = open_log(path);
	ASSERT_GE(fd, 0);
	log_appender appender;
	ASSERT_EQ(appender.append(fd, make_record("git commit --amend", 1'724'000'000ull,
	                                          "/home/dn/src/lesh", 128,
	                                          0xDEAD'BEEF'FEED'FACEull)),
	          append_status::ok);
	::close(fd);

	const std::vector<std::byte> bytes = read_file(path);
	std::vector<record> seen;
	std::vector<std::string> commands;
	std::vector<std::string> cwds;
	const log_scan scan = for_each(bytes, [&](const record& one) {
		seen.push_back(one);
		commands.push_back(as_text(one.cmd));
		cwds.push_back(as_text(one.cwd));
	});

	ASSERT_EQ(scan.frames, 1u);
	EXPECT_EQ(scan.discarded_bytes, 0u);
	EXPECT_EQ(scan.tail_bytes, 0u);
	EXPECT_EQ(commands[0], "git commit --amend");
	EXPECT_EQ(cwds[0], "/home/dn/src/lesh");
	EXPECT_EQ(seen[0].when, 1'724'000'000ull);
	EXPECT_EQ(seen[0].exit_code, 128);
	EXPECT_EQ(seen[0].session_id, 0xDEAD'BEEF'FEED'FACEull);
}

TEST(UiHistoryLog, FramesComeBackInAppendOrder) {
	lesh::testing::temp_path scratch;
	// APPEND ORDER, oldest first - the opposite of Tier 1's newest-first, and
	// deliberately so: a log grows at the end and a blob is sorted before it is
	// written. #193's merge is where the two orders are reconciled; a reader
	// that quietly reversed here would hide that decision.
	const written_log log = write_log(scratch.file("history.new.log"), numbered(5));
	const walked out = collect(log.bytes);

	EXPECT_EQ(out.commands, log.commands);
	EXPECT_EQ(out.scan.frames, 5u);
	EXPECT_EQ(out.scan.skipped_frames, 0u);
	EXPECT_EQ(out.scan.discarded_bytes, 0u);
	EXPECT_EQ(out.scan.tail_bytes, 0u);
	EXPECT_EQ(out.whens.front(), 1'000u);
	EXPECT_EQ(out.whens.back(), 1'004u);
}

TEST(UiHistoryLog, CommandBytesNeedNotBeText) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.new.log");

	// The framing's actual advantage over the text file it replaces. A newline,
	// a NUL and a lone 0x80 inside a command line are payload bytes inside a
	// known length; the scanner this format replaced would have split the entry
	// in half at the newline and then escaped its way around the rest (#1581).
	const std::string raw{"echo 'one\ntwo'\x00\x80\xFF", 18};
	ASSERT_EQ(raw.size(), 18u);

	const int fd = open_log(path);
	ASSERT_GE(fd, 0);
	log_appender appender;
	ASSERT_EQ(appender.append(fd, make_record(raw)), append_status::ok);
	::close(fd);

	const walked out = collect(read_file(path));
	ASSERT_EQ(out.scan.frames, 1u);
	EXPECT_EQ(out.commands[0], raw);
}

TEST(UiHistoryLog, TheFrameOnDiskIsLengthChecksumVersionThenPayload) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.new.log");

	const int fd = open_log(path);
	ASSERT_GE(fd, 0);
	log_appender appender;
	ASSERT_EQ(appender.append(fd, make_record("ls -la")), append_status::ok);
	::close(fd);

	// THE FORMAT IS A PROMISE TO A FUTURE READER, so one test asserts the bytes
	// rather than the round trip: a change that swapped the length and the
	// checksum, or moved the version byte, would round trip perfectly here and
	// make every log written by every earlier lesh unreadable.
	const std::vector<std::byte> bytes = read_file(path);
	ASSERT_GT(bytes.size(), k_frame_header_bytes);

	const std::uint32_t length = read_u32_le(bytes, 0);
	EXPECT_EQ(length, bytes.size() - k_frame_header_bytes);
	const auto payload = std::span{bytes}.subspan(k_frame_header_bytes);
	EXPECT_EQ(read_u32_le(bytes, 4), crc32(payload));
	EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[8]), k_log_format_version);
}

TEST(UiHistoryLog, YieldedRecordsBorrowTheInputBytes) {
	lesh::testing::temp_path scratch;
	const written_log log = write_log(scratch.file("history.new.log"), numbered(3));

	// The read path's whole contract, and the reason #193 can hand a worker a
	// buffer instead of a vector of strings: a record points INTO the bytes it
	// was parsed from and nothing is copied on the way out.
	const auto* const first = reinterpret_cast<const unsigned char*>(log.bytes.data());
	const unsigned char* const last = first + log.bytes.size();
	std::size_t seen = 0;
	for_each(log.bytes, [&](const record& one) {
		const auto* const cmd = reinterpret_cast<const unsigned char*>(one.cmd.data());
		EXPECT_GE(cmd, first);
		EXPECT_LE(cmd + one.cmd.size(), last);
		++seen;
	});
	EXPECT_EQ(seen, 3u);
}

// ---------------------------------------------------------------------------
// A torn tail
// ---------------------------------------------------------------------------

TEST(UiHistoryLog, TruncationAtEveryByteOffsetKeepsExactlyTheWholeFrames) {
	lesh::testing::temp_path scratch;
	const written_log log = write_log(scratch.file("history.new.log"), numbered(5));
	ASSERT_EQ(log.frame_end.size(), 5u);
	ASSERT_GT(log.bytes.size(), 100u);

	// EVERY OFFSET, from nothing to the whole file. A crash lands the write
	// wherever it lands, so "the reader survives a truncated log" is a claim
	// about all of them and not about a handful somebody chose. Two assertions
	// per offset, and the second is the one with teeth: not merely that the
	// walk did not crash, but that it yielded EXACTLY the frames that were
	// written whole - never a partial one dressed up as a record, never a frame
	// dropped because an earlier one ended near the cut.
	for (std::size_t keep = 0; keep <= log.bytes.size(); ++keep) {
		const walked out = collect(std::span{log.bytes}.first(keep));

		std::size_t whole = 0;
		while (whole < log.frame_end.size() && log.frame_end[whole] <= keep)
			++whole;
		const std::vector<std::string> want{log.commands.begin(),
		                                    log.commands.begin() + static_cast<std::ptrdiff_t>(whole)};

		EXPECT_EQ(out.commands, want) << "truncated to " << keep;
		EXPECT_EQ(out.scan.frames, whole) << "truncated to " << keep;
		// Everything past the last whole frame is the torn tail, and nothing
		// before it was thrown away.
		const std::size_t good = whole == 0 ? 0 : log.frame_end[whole - 1];
		EXPECT_EQ(out.scan.tail_bytes, keep - good) << "truncated to " << keep;
	}
}

TEST(UiHistoryLog, AHeaderShorterThanNineBytesIsATornTailNotAFrame) {
	// The narrow case the length check cannot even be asked: the crash landed
	// inside the header itself.
	for (std::size_t size = 0; size < k_frame_header_bytes; ++size) {
		std::vector<std::byte> bytes(size, std::byte{0xFF});
		const walked out = collect(bytes);
		EXPECT_EQ(out.scan.frames, 0u) << "header truncated to " << size;
		EXPECT_EQ(out.scan.tail_bytes, size) << "header truncated to " << size;
	}
}

// ---------------------------------------------------------------------------
// Damage in the middle, and the resync
// ---------------------------------------------------------------------------

TEST(UiHistoryLog, OneFlippedByteAnywhereInTheMiddleFrameCostsOnlyThatFrame) {
	lesh::testing::temp_path scratch;
	const written_log log = write_log(scratch.file("history.new.log"), numbered(5));
	ASSERT_EQ(log.frame_end.size(), 5u);

	const std::size_t middle_begin = log.frame_end[1];
	const std::size_t middle_end = log.frame_end[2];
	ASSERT_GT(middle_end, middle_begin + k_frame_header_bytes);

	// EVERY BYTE of the middle frame, its header included, and the answer is
	// the same for all of them: the other four come back. The header bytes are
	// the interesting ones, because a flip in the LENGTH field turns this frame
	// into a header claiming the rest of the file - the case a reader that
	// obeyed "length does not fit -> stop" to the letter would answer by
	// throwing away frames 3 and 4, which are sitting right there intact. See
	// `log.h` on why a length that does not fit resyncs.
	const std::vector<std::string> want{log.commands[0], log.commands[1],
	                                    log.commands[3], log.commands[4]};
	for (std::size_t at = middle_begin; at < middle_end; ++at) {
		std::vector<std::byte> damaged = log.bytes;
		flip(damaged, at);

		const walked out = collect(damaged);
		EXPECT_EQ(out.commands, want) << "flipped byte " << at;
		// Nothing after the damage was lost, so there is no tail at all.
		EXPECT_EQ(out.scan.tail_bytes, 0u) << "flipped byte " << at;
	}
}

TEST(UiHistoryLog, GarbageBetweenTwoFramesIsResyncedPastByteForByte) {
	lesh::testing::temp_path scratch;
	const written_log log = write_log(scratch.file("history.new.log"), numbered(2));
	ASSERT_EQ(log.frame_end.size(), 2u);

	// A block a filesystem lost, or a half-written frame from a shell that died
	// between two healthy ones. Thirty-seven bytes so the count in the scan is
	// unmistakable, and 0xAB so that any candidate header inside them claims a
	// length of 0xABABABAB and fits nothing.
	constexpr std::size_t junk_bytes = 37;
	std::vector<std::byte> spliced{log.bytes.begin(),
	                               log.bytes.begin() + static_cast<std::ptrdiff_t>(log.frame_end[0])};
	spliced.insert(spliced.end(), junk_bytes, std::byte{0xAB});
	spliced.insert(spliced.end(), log.bytes.begin() + static_cast<std::ptrdiff_t>(log.frame_end[0]),
	               log.bytes.end());

	const walked out = collect(spliced);
	EXPECT_EQ(out.commands, log.commands);
	EXPECT_EQ(out.scan.frames, 2u);
	// Exactly the junk and not one byte of either frame: the resync advances by
	// one and re-asks, so it stops at the first offset that is a frame again.
	EXPECT_EQ(out.scan.discarded_bytes, junk_bytes);
	EXPECT_EQ(out.scan.tail_bytes, 0u);
}

TEST(UiHistoryLog, AFrameWhoseChecksumAgreesButWhosePayloadIsNotARecordResyncs) {
	lesh::testing::temp_path scratch;
	const written_log log = write_log(scratch.file("history.new.log"), numbered(2));

	// A perfectly well-framed frame - correct length, correct CRC, our version
	// byte - carrying bytes that are not a FlatBuffers `Record`. Nothing in the
	// framing can tell; only the Verifier can, and this is the test that says
	// the reader asks it before it hands a caller spans to read. Without that
	// question the borrowed `cmd` below would be whatever these bytes decode to,
	// which is a read off the end of the buffer waiting for an offset.
	std::vector<std::byte> payload(24);
	for (std::size_t i = 0; i < payload.size(); ++i)
		payload[i] = static_cast<std::byte>((i * 7 + 3) & 0xFF);

	std::vector<std::byte> forged;
	append_u32_le(forged, static_cast<std::uint32_t>(payload.size()));
	append_u32_le(forged, crc32(payload));
	forged.push_back(static_cast<std::byte>(k_log_format_version));
	forged.insert(forged.end(), payload.begin(), payload.end());

	std::vector<std::byte> spliced{log.bytes.begin(),
	                               log.bytes.begin() + static_cast<std::ptrdiff_t>(log.frame_end[0])};
	spliced.insert(spliced.end(), forged.begin(), forged.end());
	spliced.insert(spliced.end(), log.bytes.begin() + static_cast<std::ptrdiff_t>(log.frame_end[0]),
	               log.bytes.end());

	const walked out = collect(spliced);
	EXPECT_EQ(out.commands, log.commands);
	EXPECT_EQ(out.scan.frames, 2u);
	EXPECT_EQ(out.scan.discarded_bytes, forged.size());
}

TEST(UiHistoryLog, AllZeroBytesAreNeverMistakenForFrames) {
	// The one systematic way a resync could latch onto garbage: `crc32({})` is
	// zero, so nine zero bytes are a header that checks out against itself
	// perfectly, and a run of zeros is what a sparse hole or a lost block looks
	// like. A zero length is therefore not a frame, at any offset.
	const std::vector<std::byte> zeros(64, std::byte{0});
	const walked out = collect(zeros);
	EXPECT_EQ(out.scan.frames, 0u);
	EXPECT_EQ(out.scan.skipped_frames, 0u);
	EXPECT_EQ(out.scan.discarded_bytes, zeros.size());
	EXPECT_EQ(out.scan.tail_bytes, zeros.size());
}

TEST(UiHistoryLog, EveryPrefixOfAFrameCarryingRandomBytesIsSurvivable) {
	// Damage that is not a flipped bit but a wholesale overwrite, swept over
	// every length. Nothing is asserted about WHAT comes back - a pseudo-random
	// buffer could in principle contain a frame - only that asking is safe,
	// which under the debug preset means ASan and UBSan watched the reader walk
	// it. This is the shape of the bug the format was chosen to prevent.
	std::vector<std::byte> noise(512);
	std::uint32_t state = 0x1234'5678u;
	for (std::byte& one : noise) {
		state = state * 1'103'515'245u + 12'345u;
		one = static_cast<std::byte>((state >> 16) & 0xFF);
	}

	for (std::size_t size = 0; size <= noise.size(); ++size) {
		const walked out = collect(std::span{noise}.first(size));
		EXPECT_LE(out.scan.discarded_bytes, size) << "noise of " << size;
	}
}

// ---------------------------------------------------------------------------
// A version this build does not know
// ---------------------------------------------------------------------------

TEST(UiHistoryLog, AnUnknownVersionIsSkippedByItsLengthNotResyncedPast) {
	lesh::testing::temp_path scratch;
	const written_log log = write_log(scratch.file("history.new.log"), numbered(3));
	ASSERT_EQ(log.frame_end.size(), 3u);

	// A frame a future lesh wrote. Its CRC still checks out, so its LENGTH is
	// still trustworthy and the reader steps over it in one move - the whole
	// reason the version byte is inside the checksummed header's reach rather
	// than inside the payload. `discarded_bytes` staying at zero is the
	// assertion that separates "skipped" from "resynced past": both would
	// produce the same two commands, and only one of them is the format working
	// as designed.
	std::vector<std::byte> bytes = log.bytes;
	bytes[log.frame_end[0] + 8] = static_cast<std::byte>(0x7F);

	const walked out = collect(bytes);
	EXPECT_EQ(out.commands, (std::vector<std::string>{log.commands[0], log.commands[2]}));
	EXPECT_EQ(out.scan.frames, 2u);
	EXPECT_EQ(out.scan.skipped_frames, 1u);
	EXPECT_EQ(out.scan.discarded_bytes, 0u);
	EXPECT_EQ(out.scan.tail_bytes, 0u);
}

// ---------------------------------------------------------------------------
// Two writers, one file
// ---------------------------------------------------------------------------

TEST(UiHistoryLog, TwoDescriptorsInterleaveWholeFrames) {
	// The reason `append` is one `writev` and never two writes, and the direct
	// descendant of `HistoryStore.TwoHandlesInterleaveWithoutCorruption`
	// (`history_store_tests.cpp:148`) for the file this replaces. Two shells
	// append to one log with NO LOCK - locking is #195's - so the only thing
	// keeping their frames out of each other is the atomicity of a single
	// `O_APPEND` write. Every frame either landed whole or did not land.
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.new.log");

	constexpr int count = 200;
	std::vector<std::string> from_a;
	std::vector<std::string> from_b;
	for (int i = 0; i < count; ++i) {
		from_a.push_back("a-entry-" + std::to_string(i));
		from_b.push_back("b-entry-" + std::to_string(i));
	}

	const auto writer = [&path](const std::vector<std::string>& entries) {
		const int fd = open_log(path);
		ASSERT_GE(fd, 0);
		log_appender appender;
		for (const std::string& one : entries)
			EXPECT_EQ(appender.append(fd, make_record(one, 1, "/tmp")),
			          append_status::ok);
		::close(fd);
	};

	std::thread a([&] { writer(from_a); });
	std::thread b([&] { writer(from_b); });
	a.join();
	b.join();

	const walked out = collect(read_file(path));
	EXPECT_EQ(out.scan.discarded_bytes, 0u);
	EXPECT_EQ(out.scan.tail_bytes, 0u);
	ASSERT_EQ(out.scan.frames, static_cast<std::size_t>(2 * count));

	// No frame missing, truncated, or fused with a neighbour: the multiset of
	// what came back is the multiset of what went in.
	std::vector<std::string> expected = from_a;
	expected.insert(expected.end(), from_b.begin(), from_b.end());
	std::vector<std::string> actual = out.commands;
	std::sort(expected.begin(), expected.end());
	std::sort(actual.begin(), actual.end());
	EXPECT_EQ(actual, expected);
}

// ---------------------------------------------------------------------------
// When the write itself fails
// ---------------------------------------------------------------------------

TEST(UiHistoryLog, AFailedWriteIsReportedWithItsErrno) {
	log_appender appender;
	EXPECT_EQ(appender.append(-1, make_record("echo hi")), append_status::write_failed);
	EXPECT_EQ(appender.error(), EBADF);
}

TEST(UiHistoryLog, AWriteToAReadOnlyDescriptorFailsWithoutTouchingTheFile) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.new.log");
	{
		std::ofstream touch{path, std::ios::binary};
	}

	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	ASSERT_GE(fd, 0);
	log_appender appender;
	EXPECT_EQ(appender.append(fd, make_record("echo hi")), append_status::write_failed);
	EXPECT_EQ(appender.error(), EBADF);
	::close(fd);

	EXPECT_EQ(file_size(path), 0u);
}

TEST(UiHistoryLog, AnAppenderIsReusableAndKeepsNoStateBetweenFrames) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.new.log");

	// One appender per shell, one builder inside it, ten thousand command lines
	// through both. A frame that inherited a byte of the previous one would
	// fail its own CRC, so this is the test that says the builder is cleared.
	const int fd = open_log(path);
	ASSERT_GE(fd, 0);
	log_appender appender;
	std::vector<std::string> commands;
	for (int i = 0; i < 500; ++i) {
		commands.push_back(std::string(static_cast<std::size_t>(i % 64) + 1, 'x')
		                   + std::to_string(i));
		ASSERT_EQ(appender.append(fd, make_record(commands.back(), 1, "/tmp")),
		          append_status::ok)
			<< "frame " << i << ", errno " << appender.error();
	}
	::close(fd);

	const walked out = collect(read_file(path));
	EXPECT_EQ(out.scan.frames, commands.size());
	EXPECT_EQ(out.commands, commands);
	EXPECT_EQ(out.scan.discarded_bytes, 0u);
}
