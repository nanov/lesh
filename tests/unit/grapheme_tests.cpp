#include "substrate/grapheme.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace lesh::grapheme;

// #108. The correctness claim for a hand-rolled segmenter is only worth making
// because Unicode ships the evidence: auxiliary/GraphemeBreakTest.txt is 766
// machine-readable cases covering exactly GB1-GB13 and GB999. The vendored file
// is read at run time rather than baked into a generated fixture, so a stale
// table and a stale expectation cannot go stale together.

namespace {

std::string encode(char32_t cp) {
	std::string out;
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
	return out;
}

std::string encode(std::initializer_list<char32_t> cps) {
	std::string out;
	for (const char32_t cp : cps)
		out += encode(cp);
	return out;
}

struct conformance_case {
	std::string text;               // the case, encoded
	std::vector<std::size_t> boundaries;  // byte offsets, including 0 and size()
	std::string source;             // the original line, for the failure message
};

// `÷ 0061 × 0300 ÷` - U+00F7 marks a boundary, U+00D7 marks its absence.
std::vector<conformance_case> load_conformance(const std::string& path) {
	std::vector<conformance_case> cases;
	std::ifstream file(path);
	EXPECT_TRUE(file.is_open()) << "cannot open " << path;

	std::string line;
	while (std::getline(file, line)) {
		const std::string source = line;
		const auto hash = line.find('#');
		if (hash != std::string::npos)
			line.erase(hash);
		std::istringstream tokens(line);
		std::string token;
		conformance_case entry;
		while (tokens >> token) {
			if (token == "\xC3\xB7")
				entry.boundaries.push_back(entry.text.size());
			else if (token == "\xC3\x97")
				continue;
			else
				entry.text += encode(static_cast<char32_t>(std::stoul(token, nullptr, 16)));
		}
		if (entry.boundaries.empty())
			continue;
		entry.source = source;
		cases.push_back(std::move(entry));
	}
	return cases;
}

std::vector<std::size_t> forward_boundaries(std::string_view text) {
	std::vector<std::size_t> out{0};
	for (std::size_t i = 0; i < text.size();)
		out.push_back(i = next_boundary(text, i));
	return out;
}

std::vector<std::size_t> backward_boundaries(std::string_view text) {
	std::vector<std::size_t> out{text.size()};
	for (std::size_t i = text.size(); i > 0;)
		out.insert(out.begin(), i = prev_boundary(text, i));
	return out;
}

const std::vector<conformance_case>& conformance() {
	static const std::vector<conformance_case> cases =
		load_conformance(std::string(LESH_UCD_DIR) + "/GraphemeBreakTest.txt");
	return cases;
}

} // namespace

// ---------------------------------------------------------------------------
// The standard's own evidence
// ---------------------------------------------------------------------------

TEST(GraphemeConformance, TheVendoredFileIsTheOneWeMeasured) {
	// If this number moves, the UCD under third_party/ucd is not Unicode 17.0.0
	// and the generator's pin and the tables disagree about what is being tested.
	EXPECT_EQ(conformance().size(), 766u);
	EXPECT_STREQ(UNICODE_VERSION, "17.0.0");
}

TEST(GraphemeConformance, AllCasesSegmentForwards) {
	std::size_t passed = 0;
	for (const conformance_case& entry : conformance()) {
		const std::vector<std::size_t> got = forward_boundaries(entry.text);
		if (got == entry.boundaries)
			++passed;
		else
			ADD_FAILURE() << "forward: " << entry.source;
	}
	EXPECT_EQ(passed, conformance().size());
}

TEST(GraphemeConformance, AllCasesSegmentBackwards) {
	// prev_boundary re-derives the same set from the other end. GB12/GB13 and
	// GB9c are not decidable backwards, so this is the check that the safe-start
	// backup really is safe rather than merely usually right.
	std::size_t passed = 0;
	for (const conformance_case& entry : conformance()) {
		const std::vector<std::size_t> got = backward_boundaries(entry.text);
		if (got == entry.boundaries)
			++passed;
		else
			ADD_FAILURE() << "backward: " << entry.source;
	}
	EXPECT_EQ(passed, conformance().size());
}

// ---------------------------------------------------------------------------
// Boundaries, by hand
// ---------------------------------------------------------------------------

TEST(GraphemeBoundary, AsciiIsOneClusterPerByte) {
	EXPECT_EQ(next_boundary("hello", 0), 1u);
	EXPECT_EQ(next_boundary("hello", 4), 5u);
	EXPECT_EQ(prev_boundary("hello", 5), 4u);
	EXPECT_EQ(prev_boundary("hello", 1), 0u);
	EXPECT_EQ(prev_boundary("hello", 0), 0u);
}

TEST(GraphemeBoundary, CrLfIsOneCluster) {
	EXPECT_EQ(next_boundary("\r\n", 0), 2u);
	EXPECT_EQ(prev_boundary("\r\n", 2), 0u);
	// GB4/GB5 the other way round: LF then CR is two.
	EXPECT_EQ(next_boundary("\n\r", 0), 1u);
}

TEST(GraphemeBoundary, OneBackspaceDeletesTheWholeCluster) {
	// What the editor actually asks this module. Each of these is one keypress.
	const std::string acute = encode({0x0041, 0x0301});          // A + combining acute
	EXPECT_EQ(prev_boundary(acute, acute.size()), 0u);

	const std::string family = encode({0x1F469, 0x200D, 0x1F466});
	EXPECT_EQ(prev_boundary(family, family.size()), 0u);

	const std::string flag = encode({0x1F1E9, 0x1F1EA});
	EXPECT_EQ(prev_boundary(flag, flag.size()), 0u);

	// GB9c, the rule Unicode 15.1 added: क + virama + ष is one conjunct.
	const std::string conjunct = encode({0x0915, 0x094D, 0x0937});
	EXPECT_EQ(prev_boundary(conjunct, conjunct.size()), 0u);

	// Hangul jamo compose into one syllable (GB6-GB8).
	const std::string syllable = encode({0x1100, 0x1161, 0x11A8});
	EXPECT_EQ(prev_boundary(syllable, syllable.size()), 0u);
}

TEST(GraphemeBoundary, FlagsCountFromTheStartOfTheRun) {
	// GB12/GB13 pair regional indicators from the left, so the third flag letter
	// starts a new cluster. This is the case a naive backwards scan gets wrong.
	const std::string three = encode({0x1F1E9, 0x1F1EA, 0x1F1E9});
	EXPECT_EQ(next_boundary(three, 0), 8u);
	EXPECT_EQ(prev_boundary(three, three.size()), 8u);
	EXPECT_EQ(prev_boundary(three, 8), 0u);
}

// ---------------------------------------------------------------------------
// Width - the half no surveyed library answers (N-4)
// ---------------------------------------------------------------------------

TEST(GraphemeWidth, CombiningMarksCostNothing) {
	EXPECT_EQ(cluster_width(encode({0x0041, 0x0301})), 1);
	EXPECT_EQ(cluster_width(encode({0x0041, 0x0301, 0x0308, 0x0331})), 1);
	EXPECT_EQ(string_width("hello"), 5);
}

TEST(GraphemeWidth, ClusterWidthIsNotTheSumOfCodepointWidths) {
	// The measurement that decided #88: summing per-codepoint widths gives 4 for
	// each of these, and a clustering terminal renders 2. utf8proc has no API
	// that answers this, and neither does anything else surveyed.
	EXPECT_EQ(cluster_width(encode({0x1F469, 0x200D, 0x1F466})), 2);   // woman-ZWJ-boy
	EXPECT_EQ(cluster_width(encode({0x1F469, 0x1F3FB})), 2);           // + skin tone
	EXPECT_EQ(cluster_width(encode({0x1F1E9, 0x1F1EA})), 2);           // DE flag

	// helix's open TODO, spelled out: man facepalming, medium-light skin tone.
	EXPECT_EQ(cluster_width(encode({0x1F926, 0x1F3FC, 0x200D, 0x2642, 0xFE0F})), 2);
}

TEST(GraphemeWidth, WideCjkTakesTwoColumns) {
	EXPECT_EQ(cluster_width(encode({0x4E00})), 2);   // W
	EXPECT_EQ(cluster_width(encode({0x3000})), 2);   // F, ideographic space
	EXPECT_EQ(cluster_width(encode({0xFF21})), 2);   // F, fullwidth A
	EXPECT_EQ(cluster_width(encode({0x1100, 0x1161, 0x11A8})), 2);   // Hangul syllable
	EXPECT_EQ(string_width(encode({0x4E00, 0x0041, 0x4E01})), 5);
}

TEST(GraphemeWidth, VariationSelectorsOutrankTheDefaultPresentation) {
	EXPECT_EQ(cluster_width(encode({0x2764})), 1);            // heart, text by default
	EXPECT_EQ(cluster_width(encode({0x2764, 0xFE0F})), 2);    // VS16: emoji
	EXPECT_EQ(cluster_width(encode({0x2764, 0xFE0E})), 1);    // VS15: text
}

TEST(GraphemeWidth, ControlsOccupyNoColumn) {
	EXPECT_EQ(cluster_width("\t"), 0);
	EXPECT_EQ(cluster_width("\n"), 0);
	EXPECT_EQ(cluster_width(""), 0);
	EXPECT_EQ(cluster_width(encode({0x200B})), 0);   // zero width space
}

TEST(GraphemeWidth, PolicyBendsTheAnswerWithoutTouchingTheTables) {
	// The helix lesson: the right width belongs to the terminal on the far side
	// of the pty, not to Unicode. These are the three knobs #108 asks to exist.
	constexpr char32_t INVERTED_BANG = 0x00A1;   // East_Asian_Width Ambiguous
	EXPECT_EQ(cluster_width(encode({INVERTED_BANG})), 1);

	width_policy cjk;
	cjk.ambiguous = 2;
	EXPECT_EQ(cluster_width(encode({INVERTED_BANG}), cjk), 2);

	width_policy unicode8;
	unicode8.emoji_presentation = 1;
	EXPECT_EQ(cluster_width(encode({0x1F469}), unicode8), 1);

	width_policy no_clustering;
	no_clustering.zwj_sequence_is_one_image = false;
	EXPECT_EQ(cluster_width(encode({0x1F469, 0x200D, 0x1F466}), no_clustering), 4);

	// The default is unchanged by any of that.
	EXPECT_EQ(cluster_width(encode({0x1F469, 0x200D, 0x1F466})), 2);
}

TEST(GraphemeWidth, AmbiguousIsSurfacedAsAClassNotSilentlyResolved) {
	// widecharwidth's design, and UAX #11's reason for it: a caller that cares
	// can see that a choice was made on its behalf.
	EXPECT_EQ(codepoint_width_class(0x00A1), width_class::ambiguous);
	EXPECT_EQ(codepoint_width_class(0x4E00), width_class::two);
	EXPECT_EQ(codepoint_width_class(0x0041), width_class::one);
	EXPECT_EQ(codepoint_width_class(0x0301), width_class::zero);
}

// ---------------------------------------------------------------------------
// Malformed UTF-8 (N-4)
//
// The line editor's buffer is bytes the user typed or pasted, which is to say
// bytes. Every one of these runs under ASan and UBSan on the ctest --preset
// debug gate, where #59, #62 and #63 each caught real undefined behaviour from a
// one-line input.
// ---------------------------------------------------------------------------

TEST(GraphemeMalformed, EveryBadByteIsItsOwnCluster) {
	// One byte consumed per malformed unit: a caller always advances, so a scan
	// over arbitrary bytes terminates.
	EXPECT_EQ(next_boundary("\xFF", 0), 1u);
	EXPECT_EQ(next_boundary("\x80", 0), 1u);            // stray continuation
	EXPECT_EQ(next_boundary("\xC0\x80", 0), 1u);        // overlong NUL
	EXPECT_EQ(next_boundary("\xED\xA0\x80", 0), 1u);    // surrogate half
	EXPECT_EQ(next_boundary("\xF5\x80\x80\x80", 0), 1u);// above U+10FFFF
	EXPECT_EQ(next_boundary("\xE2\x82", 0), 1u);        // truncated at end of input
	EXPECT_EQ(next_boundary("\xE2\x82" "a", 0), 1u);    // truncated by a lead byte
}

TEST(GraphemeMalformed, ScansTerminateAndAdvanceOverArbitraryBytes) {
	std::string junk;
	for (int b = 0; b < 256; ++b)
		junk += static_cast<char>(b);
	junk += encode({0x1F469, 0x200D, 0x1F466});
	junk += "\xF0\x9F";

	std::size_t steps = 0;
	for (std::size_t i = 0; i < junk.size(); ++steps) {
		const std::size_t next = next_boundary(junk, i);
		ASSERT_GT(next, i) << "next_boundary stalled at " << i;
		i = next;
	}
	EXPECT_GT(steps, 0u);

	for (std::size_t i = junk.size(); i > 0;) {
		const std::size_t prev = prev_boundary(junk, i);
		ASSERT_LT(prev, i) << "prev_boundary stalled at " << i;
		i = prev;
	}

	// Width over the same garbage is finite and never negative.
	EXPECT_GT(string_width(junk), 0);
}

TEST(GraphemeMalformed, ValidTextSurroundedByGarbageStillSegments) {
	const std::string text = "\xFF" + encode({0x0041, 0x0301}) + "\xC0" + encode({0x4E00});
	EXPECT_EQ(next_boundary(text, 0), 1u);            // the bad byte alone
	EXPECT_EQ(next_boundary(text, 1), 4u);            // A + acute
	EXPECT_EQ(next_boundary(text, 4), 5u);            // the second bad byte
	EXPECT_EQ(next_boundary(text, 5), 8u);            // the ideograph
	EXPECT_EQ(prev_boundary(text, text.size()), 5u);
}

TEST(GraphemeMalformed, DecodingRejectsTheSequencesThatBypassLengthChecks) {
	EXPECT_FALSE(decode("\xC0\xAF", 0).valid);              // overlong '/'
	EXPECT_FALSE(decode("\xE0\x80\xAF", 0).valid);          // overlong '/'
	EXPECT_FALSE(decode("\xF0\x80\x80\xAF", 0).valid);      // overlong '/'
	EXPECT_TRUE(decode("\x2F", 0).valid);
	EXPECT_EQ(static_cast<unsigned>(decode("\xC0\xAF", 0).cp), 0xFFFDu);
	EXPECT_EQ(decode("\xC0\xAF", 0).length, 1u);
	EXPECT_EQ(decode("", 0).length, 0u);                    // end of input, no stall
}

// ---------------------------------------------------------------------------
// The tables themselves
// ---------------------------------------------------------------------------

TEST(GraphemeTables, StayTheSizeTheResearchBudgeted) {
	// ~33 KB was the number ADR-0005 was argued against, so it is the number that
	// has to hold. utf8proc's equivalent data is 336,606 bytes; ICU is 33 MB.
	constexpr std::size_t total =
		sizeof(tables::STAGE1) + sizeof(tables::STAGE2) + sizeof(tables::RECORDS);
	static_assert(total < 40 * 1024, "the trie has outgrown its budget");
	EXPECT_LT(total, 40u * 1024u);

	// Everything is constant-folded: the tables cost binary size and nothing else.
	static_assert(lookup(U'一').east_asian_width == eaw::w);
	static_assert(lookup(0x200D).cluster_break == gcb::zwj);
	static_assert(lookup(0x094D).conjunct == incb::linker);
	static_assert(lookup(0x1F469).extended_pictographic);
	static_assert(cluster_width("A") == 1);
}

TEST(GraphemeTables, CodepointsPastTheCodespaceDegradeRatherThanReadOffTheEnd) {
	EXPECT_EQ(lookup(0x110000).cluster_break, lookup(0xFFFD).cluster_break);
	EXPECT_EQ(lookup(0xFFFFFFFF).east_asian_width, lookup(0xFFFD).east_asian_width);
}
