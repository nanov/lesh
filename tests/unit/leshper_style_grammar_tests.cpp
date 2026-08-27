// The style string grammar's runtime half (#157, spec §6.10's "string grammar
// arrives with its first string source").
//
// WHAT IS NOT HERE, AND WHY. `style_grammar.h`'s own `style_grammar_selftest`
// namespace already proves every interesting spec at compile time - a failure
// there is a build failure, which is the right outcome for a grammar rule that
// regressed. What a running test adds is what a constant expression cannot
// exercise: the runtime codegen path (the constexpr proofs never touch it) and
// a fuzz-ish sweep over every short spec, which is cheap here and would just be
// restating the selftest's cases as a loop if it lived there instead.
//
// AND NOTHING FROM `ui/`. The SGR round trip needs `ui/prompt/`'s `emit_sgr`,
// which is the HOST's since #170, so it is `ui_style_grammar_tests.cpp`'s. The
// grammar is the editor's and its tests reach no further than the editor.

#include "leshper/style_grammar.h"
#include "leshper/surface.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

namespace {

using lesh::leshper::attribute;
using lesh::leshper::color;
using lesh::leshper::has;
using lesh::leshper::parse_style;
using lesh::leshper::style;
using lesh::leshper::style_parse;

} // namespace

TEST(LeshperStyleGrammar, EmptySpecIsDefaultStyle) {
	const style_parse result = parse_style("");
	EXPECT_TRUE(result.ok);
	EXPECT_EQ(result.value, style{});
	EXPECT_EQ(result.error_at, 0u);
}

TEST(LeshperStyleGrammar, NamedColorsAndBrightOffsetAndAliases) {
	EXPECT_EQ(parse_style("cyan").value.fg, color::of_index(6));
	EXPECT_EQ(parse_style("black").value.fg, color::of_index(0));
	EXPECT_EQ(parse_style("white").value.fg, color::of_index(7));
	EXPECT_EQ(parse_style("purple").value.fg, color::of_index(5));
	EXPECT_EQ(parse_style("magenta").value.fg, color::of_index(5));
	EXPECT_EQ(parse_style("bright-red").value.fg, color::of_index(9));
	EXPECT_EQ(parse_style("bright-purple").value.fg, color::of_index(13));
	EXPECT_EQ(parse_style("bright-white").value.fg, color::of_index(15));
}

TEST(LeshperStyleGrammar, DecimalIndex) {
	EXPECT_EQ(parse_style("0").value.fg, color::of_index(0));
	EXPECT_EQ(parse_style("7").value.fg, color::of_index(7));
	EXPECT_EQ(parse_style("007").value.fg, color::of_index(7));   // leading zeros
	EXPECT_EQ(parse_style("255").value.fg, color::of_index(255));

	EXPECT_FALSE(parse_style("256").ok);
	EXPECT_FALSE(parse_style("1234").ok);   // more than 3 digits, regardless of value
}

TEST(LeshperStyleGrammar, HexTruecolor) {
	const style_parse full = parse_style("#89dceb");
	ASSERT_TRUE(full.ok);
	EXPECT_EQ(full.value.fg, color::of_rgb(0x89, 0xdc, 0xeb));

	const style_parse upper = parse_style("#89DCEB");
	ASSERT_TRUE(upper.ok);
	EXPECT_EQ(upper.value.fg, color::of_rgb(0x89, 0xdc, 0xeb));

	const style_parse shorthand = parse_style("#fff");
	ASSERT_TRUE(shorthand.ok);
	EXPECT_EQ(shorthand.value.fg, color::of_rgb(255, 255, 255));

	EXPECT_FALSE(parse_style("#12345").ok);    // 5 hex digits: neither 3 nor 6
	EXPECT_FALSE(parse_style("#gggggg").ok);   // not hex
}

TEST(LeshperStyleGrammar, Modifiers) {
	const style_parse result = parse_style("cyan.bold.underline");
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.value.fg, color::of_index(6));
	EXPECT_TRUE(has(result.value.attrs, attribute::bold));
	EXPECT_TRUE(has(result.value.attrs, attribute::underline));

	const style_parse three = parse_style("red.dim.italic");
	ASSERT_TRUE(three.ok);
	EXPECT_TRUE(has(three.value.attrs, attribute::dim));
	EXPECT_TRUE(has(three.value.attrs, attribute::italic));

	// Undercurl is ours beyond prmt's list (surface.h's opportunistic
	// attribute, #97) - the grammar has to be able to ask for it.
	EXPECT_TRUE(has(parse_style("undercurl").value.attrs, attribute::undercurl));
	EXPECT_TRUE(has(parse_style("strikethrough").value.attrs, attribute::strikethrough));
	EXPECT_TRUE(has(parse_style("reverse").value.attrs, attribute::reverse));
}

TEST(LeshperStyleGrammar, ForegroundAndBackground) {
	const style_parse both = parse_style("#ffffff+#333333");
	ASSERT_TRUE(both.ok);
	EXPECT_EQ(both.value.fg, color::of_rgb(255, 255, 255));
	EXPECT_EQ(both.value.bg, color::of_rgb(0x33, 0x33, 0x33));

	const style_parse bg_only = parse_style("+blue");
	ASSERT_TRUE(bg_only.ok);
	EXPECT_EQ(bg_only.value.bg, color::of_index(4));
	EXPECT_EQ(bg_only.value.fg, color::of_default());

	const style_parse mixed = parse_style("cyan+#222.dim");
	ASSERT_TRUE(mixed.ok);
	EXPECT_EQ(mixed.value.fg, color::of_index(6));
	EXPECT_EQ(mixed.value.bg, color::of_rgb(0x22, 0x22, 0x22));
	EXPECT_TRUE(has(mixed.value.attrs, attribute::dim));
}

TEST(LeshperStyleGrammar, LastColorItemWins) {
	const style_parse result = parse_style("red.blue");
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.value.fg, color::of_index(4));
}

TEST(LeshperStyleGrammar, ErrorsReportTheFailingItemsByteOffset) {
	struct case_ {
		std::string_view spec;
		std::size_t error_at;
	};
	static constexpr case_ kCases[] = {
		{"CYAN", 0},         // strict lowercase: not a color, not a modifier
		{"cyan..bold", 5},   // an empty item, between two dots
		{"cyan.blod", 5},    // neither a color nor a modifier
		{"#12345", 0},       // 5 hex digits
		{"256", 0},          // out of [0, 255]
		{"+", 0},            // bare +: no fg, no bg
		{"bright-", 0},      // prefix with no name after it
	};
	for (const case_& one : kCases) {
		const style_parse result = parse_style(one.spec);
		EXPECT_FALSE(result.ok) << one.spec;
		EXPECT_EQ(result.error_at, one.error_at) << one.spec;
	}
}

// A cheap, deterministic sweep: every one- and two-byte spec, including bytes
// no grammar rule ever names (control bytes, high-bit bytes, embedded NUL).
// Nothing here asserts a particular outcome for any one input - only that
// `parse_style` answers rather than crashing, and that a failing answer's
// `error_at` never points past the spec it was given.
TEST(LeshperStyleGrammar, OneAndTwoByteSpecsNeverCrash) {
	for (unsigned a = 0; a <= 0xFF; ++a) {
		const char one = static_cast<char>(a);
		const std::string_view spec{&one, 1};
		const style_parse result = parse_style(spec);
		ASSERT_LE(result.error_at, spec.size());
		if (result.ok)
			EXPECT_EQ(result.error_at, 0u);
	}

	for (unsigned a = 0; a <= 0xFF; ++a) {
		for (unsigned b = 0; b <= 0xFF; ++b) {
			const char two[2] = {static_cast<char>(a), static_cast<char>(b)};
			const std::string_view spec{two, 2};
			const style_parse result = parse_style(spec);
			ASSERT_LE(result.error_at, spec.size());
			if (result.ok)
				EXPECT_EQ(result.error_at, 0u);
		}
	}
}
