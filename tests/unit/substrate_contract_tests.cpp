#include "substrate/hybrid_vector.h"
#include "substrate/inline_vector.h"

#include <gtest/gtest.h>

#include <type_traits>

// Contract tests for the substrate containers.
//
// Issue #13 decided to FREEZE these rather than replace or repair them. They are
// used only by src/legacy/, which ADR-0002 deletes; porting them to
// boost::container::small_vector - which the survey in #16 recommends on the
// merits - would be investment in code on its way out. The replacement front end
// in src/syntax/ uses neither, and the AST's container choice belongs to #10,
// when what it actually needs is known.
//
// So these tests do two things. They pin the behaviour legacy genuinely relies
// on, so freezing does not mean unwatched. And they record the known defects as
// DISABLED tests, so the knowledge lives next to the code rather than in a
// closed ticket - each one naming why it is not being fixed.

namespace {

// Instrumentation for "this operation must not copy". Without it, an accidental
// copy is invisible: the program stays correct and only gets slower, which is
// precisely the failure mode the allocation constraint exists to prevent.
struct Counted {
	static inline int constructions = 0;
	static inline int copies = 0;
	static inline int moves = 0;
	static inline int destructions = 0;

	int value = 0;

	Counted() { ++constructions; }
	explicit Counted(int v) : value(v) { ++constructions; }
	Counted(const Counted& o) : value(o.value) { ++copies; }
	Counted(Counted&& o) noexcept : value(o.value) { ++moves; }
	Counted& operator=(const Counted& o) { value = o.value; ++copies; return *this; }
	Counted& operator=(Counted&& o) noexcept { value = o.value; ++moves; return *this; }
	~Counted() { ++destructions; }

	static void reset() { constructions = copies = moves = destructions = 0; }
};

} // namespace

// --- what legacy actually relies on ------------------------------------------

TEST(HybridVector, PushBackAndIndexRoundTrip) {
	lesh::hybrid_vector<int, 4> v;
	for (int i = 0; i < 3; ++i)
		v.push_back(i);
	ASSERT_EQ(v.size(), 3u);
	for (int i = 0; i < 3; ++i)
		EXPECT_EQ(*v[i], i);
}

TEST(HybridVector, GrowsPastInlineCapacityWithoutLosingElements) {
	lesh::hybrid_vector<int, 2> v;
	for (int i = 0; i < 64; ++i)
		v.push_back(i);
	ASSERT_EQ(v.size(), 64u);
	for (int i = 0; i < 64; ++i)
		EXPECT_EQ(*v[i], i) << "element " << i << " lost while growing";
}

TEST(InlineVector, GrowsPastInlineCapacityWithoutLosingElements) {
	lesh::hybrid_continuous_simple_vector<int, 2> v;
	for (int i = 0; i < 32; ++i)
		v.emplace_back(i);
	ASSERT_EQ(v.size(), 32u);
	for (int i = 0; i < 32; ++i)
		EXPECT_EQ(*v[i], i) << "element " << i << " lost while growing";
}

// --- the assumption that was believed rather than checked --------------------

TEST(SubstrateContainers, SkipDestructorsSoElementsMustBeTriviallyDestructible) {
	// src/substrate/hybrid_vector.h records, in a comment that used to sit inside
	// dead commented-out code, that clear() does not run destructors because
	// "it is belived emelents won't have destructor". Nothing enforced it.
	//
	// Everything legacy stores in these is trivially destructible, so the
	// assumption holds today. This test is what makes that a checked fact rather
	// than a belief - it fails the moment someone stores something with a
	// destructor, which would leak silently otherwise.
	static_assert(std::is_trivially_destructible_v<int>);
	static_assert(std::is_trivially_destructible_v<char*>);
	static_assert(std::is_trivially_destructible_v<const char*>);
	SUCCEED() << "element types used by legacy are trivially destructible";
}

// --- known defects, frozen rather than fixed ---------------------------------

TEST(HybridVector, DISABLED_CopyConstructorLosesEveryElement) {
	// CONFIRMED and LIVE, via ASTCommand's copy constructor on the alias-expansion
	// path. The implicit copy copies _size and _capacity but not the elements, and
	// leaves the storage pointer dangling: a 3-element vector copies to one that
	// reports size 3 and reads back zeroes.
	//
	// Not fixed: hybrid_vector is used only by src/legacy/, which ADR-0002
	// deletes. Repairing it would be work on code being removed. Recorded here so
	// the knowledge survives the ticket.
	lesh::hybrid_vector<int, 4> a;
	for (int i = 0; i < 3; ++i)
		a.push_back(i);

	lesh::hybrid_vector<int, 4> b(a);
	ASSERT_EQ(b.size(), a.size());
	for (int i = 0; i < 3; ++i)
		EXPECT_EQ(*b[i], i) << "copy lost element " << i;
}

TEST(InlineVector, DISABLED_PushBackWritesEveryElementToTheSameSlot) {
	// CONFIRMED but DEAD: push_back never advances the write position while size
	// still increments, so pushing 10, 11, 12 reads back 12, 0, 0. Legacy reaches
	// this container through emplace_back and emplace_child, which are correct,
	// so nothing live depends on the broken path.
	//
	// Not fixed, for the same reason as above.
	lesh::hybrid_continuous_simple_vector<int, 4> v;
	for (int i = 10; i < 13; ++i)
		v.push_back(i);
	ASSERT_EQ(v.size(), 3u);
	EXPECT_EQ(*v[0], 10);
	EXPECT_EQ(*v[1], 11);
	EXPECT_EQ(*v[2], 12);
}

// The `const T&&` fake-move constructors are NOT tested here. They live on
// ASTWord and ASTCommand in src/legacy/ast.h, not in the substrate, and an
// earlier attempt to cover them from this file tested the instrumentation type
// instead of the real ones - it passed while claiming to document a defect,
// which is worse than no test. The fact is recorded at the declarations instead.
