#include "runtime/history_store.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace lesh::runtime;

// v1 HistoryStore (#94, #113): a dumb, append-only log file behind the
// override point. These tests pin the contract that ships: append, and
// newest-first snapshot iteration over what append wrote, with newlines
// preserved (F-34) and no crash on a file this store did not itself write.

namespace {

std::vector<std::string> collect_newest_first(const history_store& store) {
	std::vector<std::string> out;
	store.for_each_newest_first([&](std::string_view entry) { out.emplace_back(entry); });
	return out;
}

} // namespace

TEST(HistoryStore, MissingFileIteratesZeroEntries) {
	lesh::testing::temp_path scratch;
	history_store store{scratch.file("does_not_exist")};

	EXPECT_TRUE(collect_newest_first(store).empty());
}

TEST(HistoryStore, EmptyFileIteratesZeroEntries) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history");
	{
		std::ofstream touch{path};
	}
	history_store store{path};

	EXPECT_TRUE(collect_newest_first(store).empty());
}

TEST(HistoryStore, SingleLineRoundTrips) {
	lesh::testing::temp_path scratch;
	history_store store{scratch.file("history")};

	ASSERT_TRUE(store.append("echo hello"));

	const auto entries = collect_newest_first(store);
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0], "echo hello");
}

TEST(HistoryStore, NewlinePreservingRoundTrip) {
	lesh::testing::temp_path scratch;
	history_store store{scratch.file("history")};

	// A recalled multiline entry (F-34) must reconstruct exactly, embedded
	// newlines and all - not flattened, not truncated at the first '\n'.
	const std::string multiline = "for i in 1 2 3\ndo\n  echo \"$i\"\ndone";
	ASSERT_TRUE(store.append(multiline));

	const auto entries = collect_newest_first(store);
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0], multiline);

	// The store's file format is one PHYSICAL line per entry: an embedded
	// newline must not appear as a raw byte in the file, or the entry
	// boundary would be ambiguous with the next append.
	std::ifstream raw{scratch.file("history")};
	std::string first_line;
	std::getline(raw, first_line);
	EXPECT_EQ(first_line, "for i in 1 2 3\\ndo\\n  echo \"$i\"\\ndone");
}

TEST(HistoryStore, LiteralBackslashRoundTrips) {
	lesh::testing::temp_path scratch;
	history_store store{scratch.file("history")};

	// A literal backslash must not be confused with the escape it introduces:
	// `printf '\n'` contains the two bytes '\' and 'n', not a newline.
	const std::string entry = "printf '\\n'";
	ASSERT_TRUE(store.append(entry));

	const auto entries = collect_newest_first(store);
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0], entry);
}

TEST(HistoryStore, EmptyEntryRoundTrips) {
	lesh::testing::temp_path scratch;
	history_store store{scratch.file("history")};

	ASSERT_TRUE(store.append(""));

	const auto entries = collect_newest_first(store);
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0], "");
}

TEST(HistoryStore, IterationIsNewestFirst) {
	lesh::testing::temp_path scratch;
	history_store store{scratch.file("history")};

	ASSERT_TRUE(store.append("first"));
	ASSERT_TRUE(store.append("second"));
	ASSERT_TRUE(store.append("third"));

	const auto entries = collect_newest_first(store);
	ASSERT_EQ(entries.size(), 3u);
	EXPECT_EQ(entries[0], "third");
	EXPECT_EQ(entries[1], "second");
	EXPECT_EQ(entries[2], "first");
}

TEST(HistoryStore, SnapshotIsFixedAtCallTime) {
	// "Last-wins reads": a snapshot is whatever is on disk when
	// for_each_newest_first is CALLED, and an append after that is invisible
	// to it - never picked up mid-walk.
	lesh::testing::temp_path scratch;
	history_store store{scratch.file("history")};

	ASSERT_TRUE(store.append("before"));
	std::vector<std::string> seen;
	store.for_each_newest_first([&](std::string_view entry) {
		seen.emplace_back(entry);
		// An append that happens WHILE a snapshot is being walked must not
		// perturb that walk.
		store.append("during");
	});

	ASSERT_EQ(seen.size(), 1u);
	EXPECT_EQ(seen[0], "before");

	// The append made during the walk is durable and visible on the NEXT call.
	const auto entries = collect_newest_first(store);
	ASSERT_EQ(entries.size(), 2u);
	EXPECT_EQ(entries[0], "during");
	EXPECT_EQ(entries[1], "before");
}

TEST(HistoryStore, TwoHandlesInterleaveWithoutCorruption) {
	// Two shells (two `history_store` handles) appending to the SAME file
	// concurrently: the v1 answer is append-only O_APPEND writes, and every
	// entry survives intact no matter how the writes interleave - because
	// each entry is exactly one write(2) call.
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history");

	constexpr int kCount = 200;
	std::vector<std::string> handle_a_entries;
	std::vector<std::string> handle_b_entries;
	for (int i = 0; i < kCount; ++i) {
		handle_a_entries.push_back("a-entry-" + std::to_string(i));
		handle_b_entries.push_back("b-entry-" + std::to_string(i));
	}

	std::thread writer_a([&] {
		history_store store{path};
		for (const auto& entry : handle_a_entries)
			EXPECT_TRUE(store.append(entry));
	});
	std::thread writer_b([&] {
		history_store store{path};
		for (const auto& entry : handle_b_entries)
			EXPECT_TRUE(store.append(entry));
	});
	writer_a.join();
	writer_b.join();

	history_store reader{path};
	const auto entries = collect_newest_first(reader);
	ASSERT_EQ(entries.size(), static_cast<size_t>(2 * kCount));

	// No entry is missing, truncated, or fused with a neighbour: every
	// recorded string is exactly one of the ones either handle wrote.
	std::vector<std::string> expected = handle_a_entries;
	expected.insert(expected.end(), handle_b_entries.begin(), handle_b_entries.end());
	std::vector<std::string> actual = entries;
	std::sort(expected.begin(), expected.end());
	std::sort(actual.begin(), actual.end());
	EXPECT_EQ(actual, expected);
}

TEST(HistoryStore, MalformedTrailingBackslashDegradesGracefully) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history");
	{
		// Hand-written, not produced by append(): a line whose last byte is a
		// lone backslash, which is not a complete escape of anything.
		std::ofstream raw{path};
		raw << "well-formed\n";
		raw << "trailing\\\n";
	}

	history_store store{path};
	const auto entries = collect_newest_first(store);

	ASSERT_EQ(entries.size(), 2u);
	// Newest first: the malformed line was appended last.
	EXPECT_EQ(entries[0], "trailing\\");
	EXPECT_EQ(entries[1], "well-formed");
}

TEST(HistoryStore, MalformedUnknownEscapeDegradesGracefully) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history");
	{
		// `\q` is not an escape this format defines - neither '\n' nor '\\'.
		std::ofstream raw{path};
		raw << "one\\qtwo\n";
	}

	history_store store{path};
	const auto entries = collect_newest_first(store);

	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0], "one\\qtwo");
}

TEST(HistoryStore, UnterminatedFinalLineStillDecodes) {
	lesh::testing::temp_path scratch;
	const std::string path = scratch.file("history");
	{
		// A write cut off mid-entry (e.g. a crash) leaves the file without a
		// trailing newline. Data still recorded before the cut must not be
		// thrown away.
		std::ofstream raw{path};
		raw << "complete\n";
		raw << "cut off mid";
	}

	history_store store{path};
	const auto entries = collect_newest_first(store);

	ASSERT_EQ(entries.size(), 2u);
	EXPECT_EQ(entries[0], "cut off mid");
	EXPECT_EQ(entries[1], "complete");
}

TEST(HistoryStore, DefaultPathMirrorsLeshrcConvention) {
	const std::string home = ::getenv("HOME") ? ::getenv("HOME") : "";
	if (home.empty())
		GTEST_SKIP() << "no $HOME in this environment";

	const auto path = history_store::default_path();
	ASSERT_TRUE(path.has_value());
	EXPECT_EQ(*path, home + "/.lesh_history");
}
