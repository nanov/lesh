// The style grammar's SGR round trip - the half that needs `ui/prompt/` (#157).
//
// WHY IT IS ITS OWN FILE. The grammar itself is `leshper/style_grammar.h` and
// its tests are `leshper_style_grammar_tests.cpp`, which must include nothing
// from `ui/` - the editor's tests do not reach into the host's tree. The round
// trip cannot obey that: it needs `prompt.h`'s `emit_sgr` on one side and
// `sgr.h`'s `apply_sgr` on the other, and the emitter is the HOST's since #170.
// So the one check that spans all three headers lives here, and the grammar's
// own file keeps to the grammar.
//
// It is also the only site where the three headers may meet at all:
// `style_grammar.h` must not include `prompt.h`, because `prompt.h` includes the
// grammar and a header cannot include its own includer.

#include "leshper/sgr.h"
#include "leshper/style_grammar.h"
#include "leshper/surface.h"
#include "ui/prompt/prompt.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using lesh::leshper::apply_sgr;
using lesh::leshper::parse_style;
using lesh::leshper::style;
using lesh::leshper::style_parse;

// The round trip: a spec parses into a `style`, `prompt::emit_sgr` turns that
// into SGR bytes from reset semantics, and `sgr.h`'s `apply_sgr` reads those
// bytes back into the style they came from. `constexpr` so the same function
// backs both the compile-time proof below and the runtime assertions further
// down - two ways of running the same check are not two checks.
constexpr bool style_round_trips_through_sgr(std::string_view spec) {
	const style_parse parsed = parse_style(spec);
	if (!parsed.ok)
		return false;
	std::string bytes;
	lesh::ui::prompt::emit_sgr(parsed.value, bytes);
	return apply_sgr(std::string_view{bytes}, style{}) == parsed.value;
}

// The one round trip the ticket asks for as a compile-time proof.
static_assert(style_round_trips_through_sgr("cyan+black.bold"));

} // namespace

TEST(UiStyleGrammar, RoundTripsThroughSgr) {
	EXPECT_TRUE(style_round_trips_through_sgr(""));
	EXPECT_TRUE(style_round_trips_through_sgr("cyan"));
	EXPECT_TRUE(style_round_trips_through_sgr("cyan+black.bold"));
	EXPECT_TRUE(style_round_trips_through_sgr("#89dceb+#333333"));
	EXPECT_TRUE(style_round_trips_through_sgr("bright-red.undercurl.reverse"));
	// A spec that fails to parse never round-trips - there is no `style` to
	// compare against.
	EXPECT_FALSE(style_round_trips_through_sgr("CYAN"));
}
