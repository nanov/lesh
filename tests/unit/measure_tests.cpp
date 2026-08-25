#include "substrate/measure.h"

#include <gtest/gtest.h>

#include <string>

using namespace lesh::grapheme;

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

} // namespace

// ---------------------------------------------------------------------------
// #114. display_width sums string_width over the spans between recognized
// CSI/OSC/SS3 escape sequences; the escapes themselves cost nothing.
// ---------------------------------------------------------------------------

TEST(MeasureWidth, PlainAsciiHasNoEscapesToSkip) {
	EXPECT_EQ(display_width(""), 0);
	EXPECT_EQ(display_width("hello"), 5);
	EXPECT_EQ(display_width("lesh$ "), 6);
}

TEST(MeasureWidth, SgrColorAdjacentToTextCostsNothing) {
	// The sequence #97's floor requires: SGR is `CSI ... 'm'`, and 256-color /
	// truecolor are both still SGR, so one CSI recognizer covers all three.
	EXPECT_EQ(display_width("\x1b[31mred\x1b[0m"), 3);
	EXPECT_EQ(display_width("\x1b[38;5;196mtruecolor-ish\x1b[0m"), 13);
	EXPECT_EQ(display_width("\x1b[38;2;255;0;0mtruecolor\x1b[0m"), 9);
}

TEST(MeasureWidth, SgrWrappingEachWordSumsAcrossSpans) {
	EXPECT_EQ(display_width("\x1b[1mbold\x1b[0m \x1b[4munderline\x1b[0m"), 4 + 1 + 9);
}

TEST(MeasureWidth, CsiCursorAndPrivateModeSequencesAreRecognizedToo) {
	// Not SGR, but still CSI: a parameter byte in 0x30-0x3F covers both plain
	// digits and the '?' private-mode marker, so any CSI form is skipped, not
	// only the color one.
	EXPECT_EQ(display_width("\x1b[2K\x1b[?25lhidden-cursor-line\x1b[?25h"), 18);
}

TEST(MeasureWidth, OscWithBelTerminatorIsSkipped) {
	// e.g. OSC 0 - set the window title.
	EXPECT_EQ(display_width("\x1b]0;title\x07visible"), 7);
}

TEST(MeasureWidth, OscWithStTerminatorIsSkipped) {
	EXPECT_EQ(display_width("\x1b]0;title\x1b\\visible"), 7);
}

TEST(MeasureWidth, OscBetweenTwoWordsCostsNothing) {
	EXPECT_EQ(display_width("left\x1b]2;ignored\x07right"), 4 + 5);
}

TEST(MeasureWidth, Ss3SequenceIsSkipped) {
	EXPECT_EQ(display_width("before\x1bOPafter"), 6 + 5);   // \x1bOP: SS3 + 'P'
}

TEST(MeasureWidth, WideAndCombiningClustersMeasureThroughTheSegmenter) {
	// One CJK ideograph (2 columns) between two SGR resets.
	EXPECT_EQ(display_width("\x1b[32m" + encode({0x4E00}) + "\x1b[0m"), 2);
	// A + combining acute is one cluster, one column, inside an SGR span.
	EXPECT_EQ(display_width("\x1b[1m" + encode({0x0041, 0x0301}) + "\x1b[0m"), 1);
	// woman-ZWJ-boy: one cluster, two columns (#88's measurement).
	EXPECT_EQ(display_width(encode({0x1F469, 0x200D, 0x1F466})), 2);
}

TEST(MeasureWidth, PolicyIsForwardedToTheUnderlyingSegmenter) {
	constexpr char32_t INVERTED_BANG = 0x00A1;   // East_Asian_Width Ambiguous
	EXPECT_EQ(display_width("\x1b[31m" + encode({INVERTED_BANG}) + "\x1b[0m"), 1);

	width_policy cjk;
	cjk.ambiguous = 2;
	EXPECT_EQ(display_width("\x1b[31m" + encode({INVERTED_BANG}) + "\x1b[0m", cjk), 2);
}

// ---------------------------------------------------------------------------
// Malformed escapes and malformed UTF-8 (N-4): degrade, never abort or stall.
// ---------------------------------------------------------------------------

TEST(MeasureMalformed, LoneEscAtEndOfInputIsZeroWidth) {
	EXPECT_EQ(display_width("hi\x1b"), 2);
}

TEST(MeasureMalformed, UnrecognizedIntroducerFallsThroughAsOrdinaryBytes) {
	// ESC 'c' (RIS, full reset) is a real ANSI escape but not one this measurer
	// recognizes (not CSI/OSC/SS3): the ESC costs nothing (Control), and 'c'
	// is measured as ordinary text, per this file's documented degrade policy.
	EXPECT_EQ(display_width("\x1b" "cafter"), 6);
}

TEST(MeasureMalformed, UnterminatedCsiDegradesToLiteralBytes) {
	// No final byte before end of input: not a recognized CSI, so every byte
	// after the (zero-width) ESC measures literally. "\x1b[1;3" -> '[' '1' ';'
	// '3' = 4 columns.
	EXPECT_EQ(display_width("\x1b[1;3"), 4);
}

TEST(MeasureMalformed, UnterminatedOscDegradesToLiteralBytes) {
	// No BEL, no ST, ever: same degrade as an unterminated CSI.
	const std::string text = "\x1b]0;no-terminator";
	EXPECT_EQ(display_width(text), static_cast<int>(text.size()) - 1);   // minus the ESC
}

TEST(MeasureMalformed, TruncatedSs3IsNotConsumedAsOne) {
	// "\x1bO" with nothing after it: SS3 needs one more byte, so it is not
	// recognized. ESC costs 0, 'x' and the fallen-through 'O' cost 1 each.
	EXPECT_EQ(display_width("x\x1bO"), 2);
}

TEST(MeasureMalformed, MalformedUtf8InsideAVisibleSpanDegradesPerGrapheme) {
	// Each invalid byte still costs one column (U+FFFD, width 1), same as
	// grapheme::string_width on its own - the measurer adds no new UTF-8
	// handling, it reuses #108's.
	EXPECT_EQ(display_width("\x1b[31m\xFF\xFE\x1b[0m"), 2);
}

TEST(MeasureMalformed, ScansTerminateOverArbitraryBytes) {
	// Every byte value, including 0x1B itself repeated, must produce a finite,
	// non-negative answer without looping.
	std::string junk;
	for (int i = 0; i < 256; ++i)
		junk += static_cast<char>(static_cast<unsigned char>(i));
	junk += junk;   // and again, so ESC sees ESC as a "next byte" too
	const int width = display_width(junk);
	EXPECT_GE(width, 0);
}

TEST(MeasureMalformed, EmptyInputIsZero) {
	EXPECT_EQ(display_width(std::string_view{}), 0);
}
