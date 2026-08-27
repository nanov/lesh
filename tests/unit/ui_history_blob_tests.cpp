#include "ui/history/blob.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lesh::ui::history;

// TIER 1 OF THE TWO-TIER HISTORY (#191, ADR-0010 §Tier 1): the round trip
// through `history.data`, and the four ways opening one can go wrong. Plus, at
// the seam where Tier 2 borrows this file's serializer, one record on its own -
// `record_writer` and `record_reader` (#192), whose buffer is a different one
// (a bare `Record` root, no identifier) reached by a different path (a log
// frame, at an address FlatBuffers would not otherwise read from).
//
// The interesting half of this file is the failure half. A blob that round
// trips is table stakes; what the format is FOR is that a file somebody else
// wrote, or a file a crash left half-written, is distinguishable from a file we
// wrote - and distinguishable in the right DIRECTION, because ADR-0010 attaches
// opposite policies to "not ours" (leave it alone forever) and "ours and
// broken" (rebuild it). A `blob_status` that folded those together would still
// pass a round-trip test and would still lose somebody's data.

namespace {

// Command lines are bytes and the tests say so in bytes: every helper here
// takes and returns `std::string`, and the spans are cast at the seam, so a
// test can put a NUL or a 0x80 in a command line as easily as an ASCII one.
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

void write_file(const std::string& path, std::span<const std::byte> bytes) {
	std::ofstream out{path, std::ios::binary | std::ios::trunc};
	ASSERT_TRUE(out.is_open()) << "could not create " << path;
	out.write(reinterpret_cast<const char*>(bytes.data()),
	          static_cast<std::streamsize>(bytes.size()));
	ASSERT_TRUE(out.good());
}

[[nodiscard]] std::vector<std::byte> read_file(const std::string& path) {
	std::ifstream in{path, std::ios::binary};
	const std::string text{std::istreambuf_iterator<char>(in),
	                       std::istreambuf_iterator<char>()};
	std::vector<std::byte> out(text.size());
	std::memcpy(out.data(), text.data(), text.size());
	return out;
}

// Serializes `records` and drops the result at `path`. The writer dies with the
// call, so every byte a later `mapped_blob` hands back provably came from the
// file and not from a buffer the test was still holding.
void write_blob(const std::string& path, std::span<const record> newest_first) {
	blob_writer writer;
	write_file(path, writer.build(newest_first));
}

[[nodiscard]] std::vector<std::string> commands_of(const mapped_blob& blob) {
	std::vector<std::string> out;
	for (const record& one : blob.records())
		out.push_back(as_text(one.cmd));
	return out;
}

} // namespace

// ---------------------------------------------------------------------------
// The round trip
// ---------------------------------------------------------------------------

TEST(UiHistoryBlob, EmptyRangeIsAValidBlob) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	// The vacuum of an empty history has to write SOMETHING, and what it writes
	// has to verify. A zero-record blob is not a degenerate case to guard
	// against; it is the first file every shell ever writes.
	write_blob(path, {});

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);
	EXPECT_TRUE(blob.records().empty());
	EXPECT_EQ(blob.records().size(), 0u);
	EXPECT_GT(blob.size_bytes(), 0u);
}

TEST(UiHistoryBlob, SingleRecordRoundTripsEveryField) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const record written = make_record("git commit --amend", 1'724'000'000ull,
	                                   "/home/dn/src/lesh", 128, 0xDEAD'BEEF'FEED'FACEull);
	write_blob(path, std::span{&written, 1});

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);
	ASSERT_EQ(blob.records().size(), 1u);

	const record read = blob.records()[0];
	EXPECT_EQ(as_text(read.cmd), "git commit --amend");
	EXPECT_EQ(read.when, 1'724'000'000ull);
	EXPECT_EQ(as_text(read.cwd), "/home/dn/src/lesh");
	EXPECT_EQ(read.exit_code, 128);
	EXPECT_EQ(read.session_id, 0xDEAD'BEEF'FEED'FACEull);
}

TEST(UiHistoryBlob, OrderIsPreservedExactlyAsGiven) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	// NEWEST FIRST IS THE CALLER'S ORDER, not a sort the writer applies: the
	// timestamps here run the WRONG way on purpose, so a writer that quietly
	// sorted by `when` would fail this and a writer that writes what it was
	// given passes it.
	const std::vector<record> newest_first{
		make_record("third", 100),
		make_record("second", 200),
		make_record("first", 300),
	};
	write_blob(path, newest_first);

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);
	EXPECT_EQ(commands_of(blob), (std::vector<std::string>{"third", "second", "first"}));
	EXPECT_EQ(blob.records()[0].when, 100u);
	EXPECT_EQ(blob.records()[2].when, 300u);
}

TEST(UiHistoryBlob, CommandBytesNeedNotBeText) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	// The whole reason the schema says `[ubyte]` and not `string`. A command
	// line is whatever the user typed at the terminal, which includes a lone
	// 0x80 pasted out of a latin-1 file and an embedded NUL out of a fat-
	// fingered ^@ - and a history that cannot recall those is a history with a
	// bug, not a history with good taste.
	const std::string raw{"echo \x80\xFF\x00 done", 13};
	ASSERT_EQ(raw.size(), 13u);
	const record written = make_record(raw);
	write_blob(path, std::span{&written, 1});

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);
	ASSERT_EQ(blob.records().size(), 1u);
	EXPECT_EQ(as_text(blob.records()[0].cmd), raw);
}

TEST(UiHistoryBlob, AbsentCwdReadsBackEmpty) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const record written = make_record("pwd", 7);
	write_blob(path, std::span{&written, 1});

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);
	ASSERT_EQ(blob.records().size(), 1u);
	// An optional field the writer left out is not a hole the reader has to
	// check for: it comes back as an empty span, which is what an unknown cwd
	// means anyway.
	EXPECT_TRUE(blob.records()[0].cwd.empty());
	EXPECT_EQ(blob.records()[0].exit_code, 0);
}

TEST(UiHistoryBlob, NegativeExitCodeRoundTrips) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const record written = make_record("false", 0, {}, -1);
	write_blob(path, std::span{&written, 1});

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);
	ASSERT_EQ(blob.records().size(), 1u);
	EXPECT_EQ(blob.records()[0].exit_code, -1);
}

TEST(UiHistoryBlob, RangeForWalksNewestFirst) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	std::vector<record> newest_first;
	std::vector<std::string> texts;
	texts.reserve(64);
	for (int i = 0; i < 64; ++i)
		texts.push_back("cmd " + std::to_string(i));
	for (const std::string& one : texts)
		newest_first.push_back(make_record(one));
	write_blob(path, newest_first);

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);

	// The merge walk in #193 iterates and stops early; both halves of that have
	// to work off the range itself, not off an index the caller keeps.
	std::size_t seen = 0;
	for (const record& one : blob.records()) {
		ASSERT_EQ(as_text(one.cmd), texts[seen]);
		if (++seen == 8)
			break;
	}
	EXPECT_EQ(seen, 8u);
	EXPECT_EQ(blob.records().size(), 64u);
}

TEST(UiHistoryBlob, ManyRecordsRoundTrip) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	constexpr std::size_t count = 5'000;
	std::vector<std::string> texts;
	texts.reserve(count);
	for (std::size_t i = 0; i < count; ++i)
		texts.push_back("grep -rn pattern" + std::to_string(i) + " src/");
	std::vector<record> newest_first;
	newest_first.reserve(count);
	for (std::size_t i = 0; i < count; ++i)
		newest_first.push_back(make_record(texts[i], i, "/tmp", 0, i));
	write_blob(path, newest_first);

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);
	ASSERT_EQ(blob.records().size(), count);
	EXPECT_EQ(as_text(blob.records()[0].cmd), texts[0]);
	EXPECT_EQ(as_text(blob.records()[count / 2].cmd), texts[count / 2]);
	EXPECT_EQ(as_text(blob.records()[count - 1].cmd), texts[count - 1]);
	EXPECT_EQ(blob.records()[count - 1].session_id, count - 1);
}

TEST(UiHistoryBlob, RecordsAreViewsIntoOneMapping) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const std::vector<record> newest_first{
		make_record("alpha", 1, "/one"),
		make_record("beta", 2, "/two"),
	};
	write_blob(path, newest_first);

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);

	// ZERO COPY, stated as something a test can fail. Reading the same record
	// twice must hand back the SAME address - a reader that materialised a copy
	// per call would hand back two - and the two records' bytes must lie within
	// one mapping's worth of each other, which is only true if they point into
	// the mapping rather than into anything the reader allocated per record.
	const record first_read = blob.records()[0];
	const record second_read = blob.records()[0];
	EXPECT_EQ(first_read.cmd.data(), second_read.cmd.data());

	const auto* const a = reinterpret_cast<const unsigned char*>(blob.records()[0].cmd.data());
	const auto* const b = reinterpret_cast<const unsigned char*>(blob.records()[1].cwd.data());
	const auto distance = static_cast<std::size_t>(a < b ? b - a : a - b);
	EXPECT_LT(distance, blob.size_bytes());
}

// ---------------------------------------------------------------------------
// One record on its own - the Tier 2 payload (#192)
// ---------------------------------------------------------------------------

TEST(UiHistoryBlob, AStandaloneRecordRoundTripsEveryField) {
	// The other buffer this file knows how to make: rooted at a bare `Record`,
	// no file identifier, and it is what one frame of `history.new.log` carries.
	record_writer writer;
	const record written = make_record("kill -TERM %1", 1'724'000'001ull, "/var/tmp",
	                                   -9, 0x0123'4567'89AB'CDEFull);
	const std::span<const std::byte> built = writer.build(written);
	const std::vector<std::byte> buffer{built.begin(), built.end()};

	record_reader reader;
	record read;
	ASSERT_TRUE(reader.read(buffer, read));
	EXPECT_EQ(as_text(read.cmd), "kill -TERM %1");
	EXPECT_EQ(read.when, 1'724'000'001ull);
	EXPECT_EQ(as_text(read.cwd), "/var/tmp");
	EXPECT_EQ(read.exit_code, -9);
	EXPECT_EQ(read.session_id, 0x0123'4567'89AB'CDEFull);
}

TEST(UiHistoryBlob, AStandaloneRecordBorrowsTheBytesItWasReadFrom) {
	record_writer writer;
	const record written = make_record("cat /etc/hosts", 3, "/etc");
	const std::span<const std::byte> built = writer.build(written);
	const std::vector<std::byte> buffer{built.begin(), built.end()};

	// THE REBASE, stated as something a test can fail. `record_reader` verifies
	// in an eight-aligned copy - a frame payload starts nine bytes into a file
	// and FlatBuffers will not read an unaligned root - and then puts the spans
	// back onto the caller's bytes. A reader that forgot the second half would
	// hand out pointers into a scratch the next frame overwrites, and every
	// assertion above would still pass.
	record_reader reader;
	record read;
	ASSERT_TRUE(reader.read(buffer, read));
	const auto* const first = reinterpret_cast<const unsigned char*>(buffer.data());
	const auto* const cmd = reinterpret_cast<const unsigned char*>(read.cmd.data());
	EXPECT_GE(cmd, first);
	EXPECT_LE(cmd + read.cmd.size(), first + buffer.size());

	// And reading a SECOND record does not disturb the first, which is the
	// property #193's walk depends on.
	const record other = make_record("cat /etc/services", 4, "/etc");
	const std::span<const std::byte> rebuilt = writer.build(other);
	const std::vector<std::byte> second{rebuilt.begin(), rebuilt.end()};
	record also;
	ASSERT_TRUE(reader.read(second, also));
	EXPECT_EQ(as_text(read.cmd), "cat /etc/hosts");
	EXPECT_EQ(as_text(also.cmd), "cat /etc/services");
}

TEST(UiHistoryBlob, AReaderIsReusableAndAcceptsTheSmallestRecord) {
	record_writer writer;
	record_reader reader;
	for (int i = 0; i < 64; ++i) {
		const std::string cmd(static_cast<std::size_t>(i) + 1, 'z');
		const record written = make_record(cmd);
		const std::span<const std::byte> built = writer.build(written);
		const std::vector<std::byte> buffer{built.begin(), built.end()};
		record read;
		ASSERT_TRUE(reader.read(buffer, read)) << "length " << cmd.size();
		EXPECT_EQ(as_text(read.cmd), cmd);
		EXPECT_TRUE(read.cwd.empty());
	}
}

TEST(UiHistoryBlob, ATruncatedOrGarbledStandaloneRecordIsRefusedAtEveryLength) {
	record_writer writer;
	const record written = make_record("make -j8 && ./build/debug/lesh_tests", 5,
	                                   "/home/dn/src/lesh");
	const std::span<const std::byte> built = writer.build(written);
	const std::vector<std::byte> whole{built.begin(), built.end()};

	// The reader stands between a frame whose CRC agreed by coincidence and a
	// caller that trusts the spans it is handed, so "refused, and never a read
	// off the end" is asserted the way Tier 1 asserts it: at every prefix, and
	// at every single-byte flip. Nothing is claimed about WHICH answer comes
	// back for a flip - a buffer can survive one - only that asking is safe.
	record_reader reader;
	for (std::size_t keep = 0; keep < whole.size(); ++keep) {
		record read;
		EXPECT_FALSE(reader.read(std::span{whole}.first(keep), read))
			<< "truncated to " << keep;
	}
	for (std::size_t at = 0; at < whole.size(); ++at) {
		std::vector<std::byte> damaged = whole;
		damaged[at] = static_cast<std::byte>(std::to_integer<unsigned>(damaged[at]) ^ 0xA5u);
		record read;
		if (!reader.read(damaged, read))
			continue;
		for (std::byte b : read.cmd)
			(void)b;
		for (std::byte b : read.cwd)
			(void)b;
	}
}

// ---------------------------------------------------------------------------
// The four ways it goes wrong
// ---------------------------------------------------------------------------

TEST(UiHistoryBlob, MissingFileIsIoErrorNotCorruption) {
	lesh::testing::temp_path scratch;

	mapped_blob blob;
	EXPECT_EQ(blob.open(scratch.file("never_written")), blob_status::io_error);
	EXPECT_EQ(blob.error(), ENOENT);
	// A first run is not damage. The caller sees ENOENT and starts an empty
	// history; it must not see anything that would make it refuse to vacuum.
	EXPECT_FALSE(blob.is_open());
	EXPECT_TRUE(blob.records().empty());
}

TEST(UiHistoryBlob, ZeroByteFileIsAnEmptyHistory) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");
	{
		std::ofstream touch{path, std::ios::binary};
	}

	// `O_CREAT` on the append path makes this the ordinary state of a shell
	// that has never vacuumed. Reporting it as damage would make a first run
	// look like a corruption, and `mmap` of zero bytes is EINVAL besides.
	mapped_blob blob;
	EXPECT_EQ(blob.open(path), blob_status::ok);
	EXPECT_TRUE(blob.records().empty());
	EXPECT_EQ(blob.size_bytes(), 0u);
}

TEST(UiHistoryBlob, UnknownIdentifierIsItsOwnAnswer) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const record written = make_record("echo hi");
	write_blob(path, std::span{&written, 1});

	// A perfectly well-formed FlatBuffer with somebody else's four bytes at
	// offset 4 - a future lesh, or another program entirely. ADR-0010: never
	// destroy it. That policy lives in #193's caller, and it can only run if
	// this layer keeps the answer SEPARATE from `corrupt`.
	std::vector<std::byte> bytes = read_file(path);
	ASSERT_GE(bytes.size(), 8u);
	bytes[4] = std::byte{'S'};
	bytes[5] = std::byte{'H'};
	bytes[6] = std::byte{'H'};
	bytes[7] = std::byte{'9'};
	write_file(path, bytes);

	mapped_blob blob;
	EXPECT_EQ(blob.open(path), blob_status::unknown_identifier);
	EXPECT_FALSE(blob.is_open());
	EXPECT_TRUE(blob.records().empty());
}

TEST(UiHistoryBlob, FileTooShortToCarryAnIdentifierIsUnknown) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	// Three bytes cannot be asked the question, so the answer is the
	// conservative one: not ours, leave it alone. Not `corrupt`, which is a
	// licence to overwrite.
	const std::array<std::byte, 3> stub{std::byte{1}, std::byte{2}, std::byte{3}};
	write_file(path, stub);

	mapped_blob blob;
	EXPECT_EQ(blob.open(path), blob_status::unknown_identifier);
	EXPECT_TRUE(blob.records().empty());
}

TEST(UiHistoryBlob, TruncatedFileFailsTheVerifierCleanly) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	std::vector<record> newest_first;
	std::vector<std::string> texts;
	for (int i = 0; i < 32; ++i)
		texts.push_back("some reasonably long command line number " + std::to_string(i));
	for (const std::string& one : texts)
		newest_first.push_back(make_record(one, 1, "/home/dn"));
	write_blob(path, newest_first);

	const std::vector<std::byte> whole = read_file(path);
	ASSERT_GT(whole.size(), 64u);

	// Every truncation point past the identifier, in steps, because the
	// interesting failures are not at one offset: a vector length that runs off
	// the end, a vtable that does, an offset that points into the tail that is
	// no longer there. CLEANLY is the assertion - `corrupt`, no crash, and
	// (under the debug preset) no ASan report.
	//
	// The last eight bytes are left out of the sweep on purpose. FlatBuffers
	// builds bottom-up, so the tail of the file is the FIRST object written,
	// and it can be preceded at the very end by up to seven bytes of alignment
	// padding that no offset points at - trimming only those would leave a
	// buffer that still verifies, which is a true fact about the format and not
	// a truncation this test is about.
	for (std::size_t keep = 8; keep + 8 <= whole.size(); keep += 7) {
		write_file(path, std::span{whole}.first(keep));
		mapped_blob blob;
		EXPECT_EQ(blob.open(path), blob_status::corrupt) << "truncated to " << keep;
		EXPECT_TRUE(blob.records().empty()) << "truncated to " << keep;
	}
}

TEST(UiHistoryBlob, GarbageBehindOurIdentifierIsCorruptNotACrash) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	// Our four bytes over something that is not a FlatBuffer at all. This is
	// the hand-edited file, and the half-written one a crash left behind.
	std::vector<std::byte> bytes(256);
	for (std::size_t i = 0; i < bytes.size(); ++i)
		bytes[i] = static_cast<std::byte>((i * 37 + 11) & 0xFF);
	bytes[4] = std::byte{'S'};
	bytes[5] = std::byte{'H'};
	bytes[6] = std::byte{'H'};
	bytes[7] = std::byte{'1'};
	write_file(path, bytes);

	mapped_blob blob;
	EXPECT_EQ(blob.open(path), blob_status::corrupt);
	EXPECT_TRUE(blob.records().empty());
}

TEST(UiHistoryBlob, ByteFlipsAnywhereInTheBufferNeverFault) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const std::vector<record> newest_first{
		make_record("make -j8", 11, "/home/dn/src/lesh", 0, 1),
		make_record("./build/debug/lesh_tests", 12, "/home/dn/src/lesh", 1, 1),
	};
	write_blob(path, newest_first);
	const std::vector<std::byte> whole = read_file(path);

	// The Verifier's actual job. Either it rejects the buffer or every record
	// it then licenses is readable - what must never happen is a read off the
	// end of the mapping, which is exactly the class of bug ASan catches here
	// and the text-delimiter scanner this format replaced kept shipping.
	for (std::size_t at = 0; at < whole.size(); ++at) {
		std::vector<std::byte> damaged = whole;
		damaged[at] = static_cast<std::byte>(std::to_integer<unsigned>(damaged[at]) ^ 0xA5u);
		write_file(path, damaged);

		mapped_blob blob;
		const blob_status status = blob.open(path);
		if (status != blob_status::ok)
			continue;
		for (const record& one : blob.records()) {
			volatile std::size_t total = one.cmd.size() + one.cwd.size();
			(void)total;
			for (std::byte b : one.cmd)
				(void)b;
			for (std::byte b : one.cwd)
				(void)b;
		}
	}
}

// ---------------------------------------------------------------------------
// Lifetime (ADR-0007)
// ---------------------------------------------------------------------------

TEST(UiHistoryBlob, ReopenReplacesThePreviousMapping) {
	lesh::testing::temp_path scratch;
	const std::string before = scratch.file("before.data");
	const std::string after = scratch.file("after.data");

	const record old_one = make_record("old");
	const record new_one = make_record("new");
	write_blob(before, std::span{&old_one, 1});
	write_blob(after, std::span{&new_one, 1});

	mapped_blob blob;
	ASSERT_EQ(blob.open(before), blob_status::ok);
	ASSERT_EQ(blob.open(after), blob_status::ok);
	EXPECT_EQ(commands_of(blob), (std::vector<std::string>{"new"}));
}

TEST(UiHistoryBlob, FailedReopenLeavesNothingStaleBehind) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const record written = make_record("still here?");
	write_blob(path, std::span{&written, 1});

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);
	ASSERT_EQ(blob.records().size(), 1u);

	// A failed re-open must not leave the caller holding the PREVIOUS file:
	// after a vacuum, "the old mapping is still good" is precisely the wrong
	// answer, and it is the answer a reader would silently get if `open`
	// released only on success.
	EXPECT_EQ(blob.open(scratch.file("gone")), blob_status::io_error);
	EXPECT_FALSE(blob.is_open());
	EXPECT_TRUE(blob.records().empty());
}

TEST(UiHistoryBlob, MoveTransfersTheMapping) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const record written = make_record("mv");
	write_blob(path, std::span{&written, 1});

	mapped_blob source;
	ASSERT_EQ(source.open(path), blob_status::ok);

	mapped_blob moved = std::move(source);
	EXPECT_TRUE(moved.is_open());
	EXPECT_EQ(commands_of(moved), (std::vector<std::string>{"mv"}));
	// NOLINTNEXTLINE(bugprone-use-after-move) - asserting the moved-from state
	EXPECT_FALSE(source.is_open());
	// NOLINTNEXTLINE(bugprone-use-after-move)
	EXPECT_TRUE(source.records().empty());
}

TEST(UiHistoryBlob, CloseIsIdempotent) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const record written = make_record("close");
	write_blob(path, std::span{&written, 1});

	mapped_blob blob;
	ASSERT_EQ(blob.open(path), blob_status::ok);
	blob.close();
	blob.close();
	EXPECT_FALSE(blob.is_open());
	EXPECT_TRUE(blob.records().empty());
}

TEST(UiHistoryBlob, DestructorReleasesTheDescriptor) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history.data");

	const record written = make_record("fd");
	write_blob(path, std::span{&written, 1});

	// ADR-0007's gate is the count at exit, and a leaked file descriptor is not
	// something the leak sanitizer counts. A thousand open-and-destroy cycles
	// is: the soft `RLIMIT_NOFILE` on this platform is in the hundreds, so a
	// destructor that forgot to `close` runs the process out of descriptors
	// long before this loop ends, and the failure names itself.
	for (int i = 0; i < 1'000; ++i) {
		mapped_blob blob;
		ASSERT_EQ(blob.open(path), blob_status::ok) << "iteration " << i
		                                            << ", errno " << blob.error();
	}
}

TEST(UiHistoryBlob, WriterIsReusableAcrossBuilds) {
	lesh::testing::temp_path scratch;
	const std::string first = scratch.file("first.data");
	const std::string second = scratch.file("second.data");

	// The vacuum keeps one writer and rewrites every 25 appends; a second build
	// that inherited a byte of the first would be a corrupted history file.
	blob_writer writer;
	const std::vector<record> a{make_record("one"), make_record("two")};
	write_file(first, writer.build(a));
	const std::vector<record> b{make_record("three")};
	write_file(second, writer.build(b));

	mapped_blob blob;
	ASSERT_EQ(blob.open(first), blob_status::ok);
	EXPECT_EQ(commands_of(blob), (std::vector<std::string>{"one", "two"}));
	ASSERT_EQ(blob.open(second), blob_status::ok);
	EXPECT_EQ(commands_of(blob), (std::vector<std::string>{"three"}));
}

TEST(UiHistoryBlob, WriterBytesAreEmptyBeforeTheFirstBuild) {
	const blob_writer writer;
	EXPECT_TRUE(writer.bytes().empty());
}

// ---------------------------------------------------------------------------
// The committed generated header
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] bool have_flatc() {
	return std::system("command -v flatc > /dev/null 2>&1") == 0;
}

[[nodiscard]] std::string read_text(const std::string& path) {
	std::ifstream in{path, std::ios::binary};
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(UiHistoryBlobSchema, GeneratedHeaderIsCurrent) {
	// `history_generated.h` is COMMITTED, not generated by the build, so that
	// `flatc` is not a build dependency (ADR-0010 §Placement). The cost of that
	// is that nothing in the build proves the header still matches the schema,
	// and a hand-edited generated file is a wrong answer that still compiles -
	// the same argument `unicode_tables_current` makes about the UCD tables.
	// So: regenerate here, and diff.
	if (!have_flatc())
		GTEST_SKIP() << "flatc is not on PATH; install it (brew install flatbuffers, "
		                "version 25.12.19 - see third_party/flatbuffers/README.lesh.md) "
		                "to check the committed header against the schema";

	lesh::testing::temp_path scratch;
	const std::string schema = std::string{LESH_HISTORY_SCHEMA_DIR} + "/history.fbs";
	const std::string command =
		"flatc --cpp -o '" + scratch.dir() + "' '" + schema + "' > /dev/null 2>&1";
	ASSERT_EQ(std::system(command.c_str()), 0) << "flatc failed on " << schema;

	const std::string fresh = read_text(scratch.file("history_generated.h"));
	ASSERT_FALSE(fresh.empty()) << "flatc produced no history_generated.h";

	const std::string committed =
		read_text(std::string{LESH_HISTORY_SCHEMA_DIR} + "/history_generated.h");

	EXPECT_EQ(committed, fresh)
		<< "src/ui/history/history_generated.h is not what flatc emits from "
		   "history.fbs.\n"
		   "Regenerate it:  flatc --cpp -o src/ui/history src/ui/history/history.fbs\n"
		   "If only the FLATBUFFERS_VERSION static_assert differs, your flatc is a "
		   "different version from the vendored headers - see "
		   "third_party/flatbuffers/README.lesh.md.";
}
