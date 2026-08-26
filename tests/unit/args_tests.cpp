#include "substrate/args.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

// Whether this build can count REAL mallocs. Only an ASan build can, which is
// the build the gate runs (`ctest --preset debug`), so the zero-allocation
// invariant is asserted exactly where it can be asserted at all.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LESH_COUNTS_REAL_MALLOCS 1
#include <sanitizer/allocator_interface.h>
#endif
#endif

using namespace lesh;

// lesh::args, against POSIX XBD 12.2 and against the four corpus cases that
// broke the loops this replaces.
//
// THE HEADER ALREADY ASSERTS THE GRAMMAR. `scan` is a constant expression and
// src/substrate/args.h ends in static_asserts over the same cases; if one of
// those regresses the build fails before this file runs. What is left here is
// everything the grammar cannot decide on its own - which FIELD a letter wrote,
// and with what - plus the two properties that are only observable at runtime:
// that nothing allocates, and that the usage writer produces text.

namespace {

enum class cd_mode : std::uint8_t { logical, physical };

struct cd_opts {
	// THE DEFAULTS ARE HERE, and nowhere else. POSIX: `cd` and `pwd` are -L
	// unless told otherwise.
	cd_mode mode = cd_mode::logical;
	bool e = false;
};

constexpr auto kCd = args::spec<cd_opts>(
	args::option{'L', args::field<&cd_opts::mode>, cd_mode::logical}.help("resolve .. logically"),
	args::option{'P', args::field<&cd_opts::mode>, cd_mode::physical}.help("resolve .. physically"),
	args::option{'e', args::field<&cd_opts::e>}.help("fail if PWD cannot be determined"));

// Every setter kind in one table, so a change to the core has one place that
// notices. `-o` and `-n` also exercise both attachment spellings.
struct wide_opts {
	bool xtrace = false;
	int verbosity = 0;
	std::string_view name{};
	long count = 0;
	std::uint8_t narrow = 0;
	cd_mode mode = cd_mode::logical;
};

constexpr auto kWide = args::spec<wide_opts>(
	args::option{'x', args::field<&wide_opts::xtrace>, args::toggle},
	args::option{'v', "verbose", args::field<&wide_opts::verbosity>, args::count},
	args::option{'o', args::field<&wide_opts::name>, args::value("NAME")},
	args::option{'n', "number", args::field<&wide_opts::count>, args::value("N")},
	args::option{'b', args::field<&wide_opts::narrow>, args::value("BYTE")},
	args::option{'P', args::field<&wide_opts::mode>, cd_mode::physical});

// A utility's argv, owned and MUTABLE, because that is what a utility is handed
// and because a `string_view` field points straight into it.
class words {
public:
	explicit words(std::initializer_list<const char*> from) {
		storage_.reserve(from.size());
		for (const char* w : from)
			storage_.emplace_back(w);
		argv_.reserve(from.size() + 1);
		for (std::string& w : storage_)
			argv_.push_back(w.data());
		argv_.push_back(nullptr);
	}

	char** argv() { return argv_.data(); }
	// Which argv slot `rest` landed on, which is what the POSIX cases assert.
	std::ptrdiff_t index_of(char** rest) const { return rest - argv_.data(); }

private:
	std::vector<std::string> storage_;
	std::vector<char*> argv_;
};

} // namespace

// ---------------------------------------------------------------------------
// The corpus's tripwire: last one wins, inside a cluster as well as across words
// ---------------------------------------------------------------------------

TEST(Args, TheLastOfLAndPWinsInAllFourOrderings) {
	// cd-p.tst:354 and :365, which the research note (S3.5) identifies as the one
	// place the corpus actually constrains this parser. A table-driven parse that
	// records only WHETHER a letter appeared cannot answer them; binding both
	// letters to one field answers them by construction.
	struct {
		std::initializer_list<const char*> argv;
		cd_mode expected;
	} const cases[] = {
		{{"cd", "-P", "-L", "-PL"}, cd_mode::logical},
		{{"cd", "-L", "-P", "-LP"}, cd_mode::physical},
		{{"cd", "-LP"}, cd_mode::physical},
		{{"cd", "-PL"}, cd_mode::logical},
	};
	for (const auto& c : cases) {
		words w{c.argv};
		const auto r = args::parse(kCd, w.argv());
		EXPECT_FALSE(r.err);
		EXPECT_EQ(r.opts.mode, c.expected);
	}
}

TEST(Args, AnUngroupedLetterIsUnaffectedByTheModeLetters) {
	words w{{"cd", "-Pe"}};
	const auto r = args::parse(kCd, w.argv());
	EXPECT_FALSE(r.err);
	EXPECT_EQ(r.opts.mode, cd_mode::physical);
	EXPECT_TRUE(r.opts.e);
}

TEST(Args, TheDefaultsAreTheStructsMemberInitializers) {
	words w({"cd"});
	const auto r = args::parse(kCd, w.argv());
	EXPECT_FALSE(r.err);
	EXPECT_EQ(r.opts.mode, cd_mode::logical);
	EXPECT_FALSE(r.opts.e);
	EXPECT_EQ(w.index_of(r.rest), 1);
	EXPECT_EQ(*r.rest, nullptr);
}

// ---------------------------------------------------------------------------
// POSIX XBD 12.2 - the note's 17-case suite, here against the STORES
// ---------------------------------------------------------------------------

TEST(Args, OptionsCluster) {
	words w{{"cd", "-LP", "/tmp"}};
	const auto r = args::parse(kCd, w.argv());
	EXPECT_FALSE(r.err);
	EXPECT_EQ(r.opts.mode, cd_mode::physical);
	EXPECT_EQ(w.index_of(r.rest), 2);
	EXPECT_STREQ(*r.rest, "/tmp");
}

TEST(Args, AnOptionArgumentMayBeAttachedOrSeparate) {
	words attached{{"read", "-o:", "v"}};
	const auto a = args::parse(kWide, attached.argv());
	EXPECT_FALSE(a.err);
	EXPECT_EQ(a.opts.name, ":");
	EXPECT_EQ(attached.index_of(a.rest), 2);

	words separate{{"read", "-o", ":", "v"}};
	const auto s = args::parse(kWide, separate.argv());
	EXPECT_FALSE(s.err);
	EXPECT_EQ(s.opts.name, ":");
	EXPECT_EQ(separate.index_of(s.rest), 3);
}

TEST(Args, AClusterMayEndInAnArgumentTakingOption) {
	words w{{"read", "-vo:", "v"}};
	const auto r = args::parse(kWide, w.argv());
	EXPECT_FALSE(r.err);
	EXPECT_EQ(r.opts.verbosity, 1);
	EXPECT_EQ(r.opts.name, ":");
	EXPECT_EQ(w.index_of(r.rest), 2);
}

TEST(Args, ADoubleDashEndsTheOptions) {
	words w{{"read", "--", "-x"}};
	const auto r = args::parse(kWide, w.argv());
	EXPECT_FALSE(r.err);
	EXPECT_FALSE(r.opts.xtrace);
	EXPECT_EQ(w.index_of(r.rest), 2);
	EXPECT_STREQ(*r.rest, "-x");
}

TEST(Args, ALoneDashIsAnOperand) {
	// cd's OLDPWD operand, and trap's reset action. Reading it as an empty option
	// group is the bug this case exists to catch.
	words w{{"cd", "-"}};
	const auto r = args::parse(kCd, w.argv());
	EXPECT_FALSE(r.err);
	EXPECT_EQ(w.index_of(r.rest), 1);
	EXPECT_STREQ(*r.rest, "-");
}

TEST(Args, OperandsFollowTheOptionsAndAreNotPermuted) {
	words w{{"cd", "-L", "a", "-P"}};
	const auto r = args::parse(kCd, w.argv());
	EXPECT_FALSE(r.err);
	// POSIX Guideline 9: the `-P` after an operand is an OPERAND, not an option,
	// so the mode is still the -L the first word asked for.
	EXPECT_EQ(r.opts.mode, cd_mode::logical);
	EXPECT_EQ(w.index_of(r.rest), 2);
}

// ---------------------------------------------------------------------------
// The setters
// ---------------------------------------------------------------------------

TEST(Args, AToggleWritesTrueForMinusAndFalseForPlus) {
	words minus_then_plus{{"set", "-x", "+x"}};
	EXPECT_FALSE(args::parse(kWide, minus_then_plus.argv()).opts.xtrace);

	words plus_then_minus{{"set", "+x", "-x"}};
	EXPECT_TRUE(args::parse(kWide, plus_then_minus.argv()).opts.xtrace);

	words clustered{{"set", "+xx"}};
	EXPECT_FALSE(args::parse(kWide, clustered.argv()).opts.xtrace);
}

TEST(Args, APlusWordIsRejectedWhereTheRowDoesNotAdmitIt) {
	words w{{"cd", "+L"}};
	const auto r = args::parse(kCd, w.argv());
	EXPECT_EQ(r.err, (args::error{args::error_kind::unknown_option, 'L'}));
	EXPECT_EQ(r.rest, nullptr);
}

TEST(Args, ACounterCountsEveryAppearance) {
	words w{{"x", "-vvv", "-v"}};
	const auto r = args::parse(kWide, w.argv());
	EXPECT_FALSE(r.err);
	EXPECT_EQ(r.opts.verbosity, 4);
}

TEST(Args, AStoredViewPointsIntoArgvRatherThanCopyingIt) {
	words w{{"x", "-o", "errexit"}};
	const auto r = args::parse(kWide, w.argv());
	EXPECT_FALSE(r.err);
	EXPECT_EQ(r.opts.name, "errexit");
	// The whole zero-heap claim in one assertion: the field IS the caller's word.
	EXPECT_EQ(r.opts.name.data(), w.argv()[2]);
}

TEST(Args, AnIntegralOptionArgumentIsConvertedAndRangeChecked) {
	words plain{{"x", "-n", "42"}};
	EXPECT_EQ(args::parse(kWide, plain.argv()).opts.count, 42);

	words negative{{"x", "-n-7"}};
	EXPECT_EQ(args::parse(kWide, negative.argv()).opts.count, -7);

	words not_a_number{{"x", "-n", "4x"}};
	EXPECT_EQ(args::parse(kWide, not_a_number.argv()).err,
	          (args::error{args::error_kind::invalid_argument, 'n'}));

	// The range check is against the FIELD, not against the widest integer: 256
	// does not fit the uint8_t `-b` binds, and 255 does.
	words too_wide{{"x", "-b", "256"}};
	EXPECT_EQ(args::parse(kWide, too_wide.argv()).err,
	          (args::error{args::error_kind::invalid_argument, 'b'}));
	words fits{{"x", "-b", "255"}};
	EXPECT_EQ(args::parse(kWide, fits.argv()).opts.narrow, 255);
	words unsigned_negative{{"x", "-b", "-1"}};
	EXPECT_EQ(args::parse(kWide, unsigned_negative.argv()).err,
	          (args::error{args::error_kind::invalid_argument, 'b'}));
}

// ---------------------------------------------------------------------------
// Long names - never for a POSIX builtin, always available to a ported one
// ---------------------------------------------------------------------------

TEST(Args, ALongNameMatchesBesideItsLetter) {
	words separate{{"x", "--number", "9"}};
	EXPECT_EQ(args::parse(kWide, separate.argv()).opts.count, 9);

	words attached{{"x", "--number=9"}};
	EXPECT_EQ(args::parse(kWide, attached.argv()).opts.count, 9);

	words no_argument{{"x", "--verbose", "--verbose"}};
	EXPECT_EQ(args::parse(kWide, no_argument.argv()).opts.verbosity, 2);
}

TEST(Args, AnUnknownLongNameHasNoLetterToReport) {
	words w{{"x", "--nosuch"}};
	const auto r = args::parse(kWide, w.argv());
	EXPECT_EQ(r.err, (args::error{args::error_kind::unknown_option, '\0'}));
}

TEST(Args, ASpecWithoutLongNamesTreatsThemAsUnknownOptions) {
	// cd declares letters only, so `cd --logical` must fail exactly as it does
	// today rather than quietly growing a GNU spelling.
	words w{{"cd", "--logical"}};
	EXPECT_EQ(args::parse(kCd, w.argv()).err.kind, args::error_kind::unknown_option);
}

TEST(Args, AValueOnALongNameThatTakesNoneIsRejected) {
	words w{{"x", "--verbose=3"}};
	EXPECT_EQ(args::parse(kWide, w.argv()).err,
	          (args::error{args::error_kind::invalid_argument, 'v'}));
}

// ---------------------------------------------------------------------------
// The error kinds
// ---------------------------------------------------------------------------

TEST(Args, AnUnknownOptionNamesItsLetterAndNullsTheOperandTail) {
	words w{{"cd", "-Z"}};
	const auto r = args::parse(kCd, w.argv());
	EXPECT_EQ(r.err, (args::error{args::error_kind::unknown_option, 'Z'}));
	// Null rather than a valid tail, so an unchecked result cannot be iterated.
	EXPECT_EQ(r.rest, nullptr);
	EXPECT_FALSE(static_cast<bool>(r));
}

TEST(Args, TheUnknownLetterInsideAClusterIsTheOneReported) {
	words w{{"cd", "-LZP"}};
	EXPECT_EQ(args::parse(kCd, w.argv()).err,
	          (args::error{args::error_kind::unknown_option, 'Z'}));
}

TEST(Args, AMissingOptionArgumentIsItsOwnErrorKind) {
	words bare{{"x", "-o"}};
	EXPECT_EQ(args::parse(kWide, bare.argv()).err,
	          (args::error{args::error_kind::missing_argument, 'o'}));

	words at_the_end_of_a_cluster{{"x", "-vo"}};
	EXPECT_EQ(args::parse(kWide, at_the_end_of_a_cluster.argv()).err,
	          (args::error{args::error_kind::missing_argument, 'o'}));
}

TEST(Args, AnEmptyOptionArgumentIsAcceptedRatherThanMissing) {
	// `-o ''` is a real, empty value; only a MISSING word is an error.
	words w{{"x", "-o", ""}};
	const auto r = args::parse(kWide, w.argv());
	EXPECT_FALSE(r.err);
	EXPECT_TRUE(r.opts.name.empty());
}

// ---------------------------------------------------------------------------
// The usage writer
// ---------------------------------------------------------------------------

TEST(Args, UsageIsAssembledFromTheSameTableTheParseReads) {
	std::string text;
	const args::sink out{[](void* context, std::string_view piece) {
		                     static_cast<std::string*>(context)->append(piece);
	                     },
	                     &text};
	args::write_usage(out, "cd", kCd);

	EXPECT_NE(text.find("usage: cd [-L] [-P] [-e] [operand...]"), std::string::npos) << text;
	EXPECT_NE(text.find("resolve .. logically"), std::string::npos) << text;
	EXPECT_NE(text.find("fail if PWD cannot be determined"), std::string::npos) << text;
}

TEST(Args, UsagePrintsTheArgumentPlaceholderAndTheLongName) {
	std::string text;
	const args::sink out{[](void* context, std::string_view piece) {
		                     static_cast<std::string*>(context)->append(piece);
	                     },
	                     &text};
	args::write_usage(out, "x", kWide);

	EXPECT_NE(text.find("[-o NAME]"), std::string::npos) << text;
	EXPECT_NE(text.find("[-n N]"), std::string::npos) << text;
	EXPECT_NE(text.find("--number N"), std::string::npos) << text;
}

// ---------------------------------------------------------------------------
// The invariant the whole design exists for
// ---------------------------------------------------------------------------

#ifdef LESH_COUNTS_REAL_MALLOCS

namespace {

std::atomic<size_t> g_mallocs{0};
std::atomic<bool> g_counting{false};

void note_malloc(const volatile void*, size_t) noexcept {
	if (g_counting.load(std::memory_order_relaxed))
		g_mallocs.fetch_add(1, std::memory_order_relaxed);
}
void note_free(const volatile void*) noexcept {}

// ONE INSTALL FOR THE WHOLE FILE. ASan keeps a fixed-size array of malloc hooks
// - five, in compiler-rt - and the installer APPENDS rather than replacing, so
// a per-call-site install silently stops counting once the array fills. #129
// paid for that lesson in allocation_tests.cpp; this file follows the same rule.
const bool g_hooks_installed =
	__sanitizer_install_malloc_and_free_hooks(note_malloc, note_free) != 0;

template <typename Work>
size_t mallocs_during(Work&& work) {
	EXPECT_TRUE(g_hooks_installed) << "the sanitizer allocator hook is what this measures with";
	g_mallocs.store(0, std::memory_order_relaxed);
	g_counting.store(true, std::memory_order_relaxed);
	work();
	g_counting.store(false, std::memory_order_relaxed);
	return g_mallocs.load(std::memory_order_relaxed);
}

} // namespace

TEST(Args, TheMallocCounterCountsAMallocWhenThereIsOne) {
	// The positive control, and it is not optional: the assertion below is a
	// ZERO, and a broken instrument reports zero for everything. No gtest macro
	// inside the window - an assertion that allocated would be measuring itself.
	size_t observed = 0;
	const size_t counted = mallocs_during([&observed] {
		const std::string big(4096, 'x');
		observed = big.size();
	});
	EXPECT_EQ(observed, 4096u);
	EXPECT_GT(counted, 0u);
}

TEST(Args, TenThousandParsesAllocateNothing) {
	// The number the whole design is for. Every off-the-shelf option library the
	// research note measured fails here, because all of them copy argv into a
	// vector<string> before they look at it.
	words w{{"set", "-vvx", "-o", "pipefail", "-n", "12", "--", "a", "b"}};
	char** const argv = w.argv();

	int verbosity = 0;
	const size_t counted = mallocs_during([argv, &verbosity] {
		for (int i = 0; i < 10'000; ++i) {
			const auto r = args::parse(kWide, argv);
			// Read the result, so nothing above can be optimised away.
			verbosity += r.opts.verbosity + (r.rest != nullptr ? 1 : 0);
		}
	});
	EXPECT_EQ(counted, 0u) << "the parse fell back to the heap";
	EXPECT_EQ(verbosity, 30'000);
}

TEST(Args, TheUsageWriterIsHeapFreeToo) {
	// The sink is what owns memory, if anything does; writing into a fixed buffer
	// shows that the writer itself asks for none.
	struct fixed {
		char bytes[512]{};
		size_t used = 0;
	} buffer;
	const args::sink out{[](void* context, std::string_view piece) {
		                     auto* b = static_cast<fixed*>(context);
		                     for (char c : piece)
			                     if (b->used < sizeof(b->bytes))
				                     b->bytes[b->used++] = c;
	                     },
	                     &buffer};

	const size_t counted = mallocs_during([&out] { args::write_usage(out, "cd", kCd); });
	EXPECT_EQ(counted, 0u);
	EXPECT_GT(buffer.used, 0u);
}

#endif // LESH_COUNTS_REAL_MALLOCS
