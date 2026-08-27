// The compass. See issue #7.
//
// Not a benchmark suite - a small set of numbers that answer "are we going the
// wrong way?". Introduced gradually from Phase 0 and promoted to a gate in
// Phase 4, per the scope spec.
//
// Two kinds of number, deliberately:
//
//   Allocation counts are DETERMINISTIC. They are the real signal, they can be
//   asserted rather than eyeballed, and they need no quiet machine. Run these in
//   Debug, where the counters are compiled in.
//
//   Timings are NOISY and only meaningful in Release, where the optimiser has
//   run. Treat them as an order of magnitude, never as a regression test.
//
// Build:  cmake --build --preset bench --target lesh_bench
// Run:    ./build/bench/tools/lesh_bench

#include "fiber/scheduler.h"
#include "runtime/executor.h"
#include "runtime/expander.h"
#include "runtime/shell_state.h"
#include "substrate/grapheme.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <tuple>
#include <string_view>
#include <vector>

using namespace lesh;
using namespace std::chrono;

namespace {

struct Case {
	const char* name;
	const char* source;
};

// Deliberately ordinary command lines. Measuring pathological input would tell us
// about the pathology rather than about the shell.
const std::vector<Case> kCases = {
	{"bare word",          "echo"},
	{"three words",        "echo hello world"},
	{"quoted",             "echo \"hello world\" 'literal'"},
	{"one parameter",      "echo $HOME"},
	{"parameter in word",  "echo pre$HOME-post"},
	{"pipeline",           "cat file | grep pattern | wc -l"},
	{"redirection",        "cmd arg > out 2>&1"},
	{"and-or chain",       "a && b || c && d"},
	{"long argv",          "ls -la -h -R --color=auto /usr /var /etc /tmp /opt"},
};

template <typename F>
double time_ns(size_t iterations, F&& body) {
	const auto start = steady_clock::now();
	for (size_t i = 0; i < iterations; ++i)
		body();
	const auto elapsed = steady_clock::now() - start;
	return static_cast<double>(duration_cast<nanoseconds>(elapsed).count()) / iterations;
}

void report_header(const char* title) {
	std::printf("\n%s\n", title);
	std::printf("%-22s %10s %10s %10s %12s\n",
	            "case", "pool", "heap", "bytes", "ns/op");
	std::printf("%-22s %10s %10s %10s %12s\n",
	            "----------------------", "----------", "----------", "----------", "------------");
}

// Keeps the optimiser from deleting a scan whose result nothing reads.
volatile int benchmark_sink = 0;

// The fiber under the switch-cost measurement below: yield, forever. One slice
// is one `yield` plus the resume that follows it, i.e. one round trip.
void yield_forever(fiber::scheduler& on, void* /*userdata*/) {
	for (;;)
		on.yield();
}

} // namespace

int main() {
	constexpr size_t kIterations = 20000;

	std::printf("lesh compass - allocation counts are the signal, timings are indicative\n");
#ifdef LESH_ENABLE_ASSERTS
	std::printf("counters: ON (Debug or RelWithDebInfo)\n");
#else
	std::printf("counters: OFF (Release) - allocation columns will read zero\n");
#endif

	report_header("front end: lex + parse");
	for (const auto& c : kCases) {
		buffer_pool pool{BUFFER_POOL_SIZE};
		metrics::allocations().reset();
		{
			const syntax::tree t = syntax::parse(pool, c.source);
			(void)t.node_count();
		}
		const auto counts = metrics::allocations();

		const double ns = time_ns(kIterations, [&] {
			buffer_pool inner{BUFFER_POOL_SIZE};
			const syntax::tree t = syntax::parse(inner, c.source);
			(void)t.node_count();
		});

		std::printf("%-22s %10zu %10zu %10zu %12.0f\n", c.name,
		            counts.pool_allocations, counts.heap_allocations,
		            counts.bytes_from_pool + counts.bytes_from_heap, ns);
	}

	report_header("expansion (per word, no execution)");
	{
		runtime::shell_state state;
		std::ignore = state.set("HOME", "/home/tester");
		for (const auto& c : kCases) {
			buffer_pool pool{BUFFER_POOL_SIZE};
			const syntax::tree t = syntax::parse(pool, c.source);
			if (t[t.root()].children_count == 0)
				continue;

			metrics::allocations().reset();
			{
				runtime::expander ex{pool, state};
				arena_array<std::string_view> fields{pool, 8};
				const syntax::node_index cmd = t.child_of(t[t.root()], 0);
				if (t[cmd].kind == syntax::node_kind::simple_command)
					for (uint32_t i = 0; i < t[cmd].children_count; ++i)
						ex.expand_word(t, t.child_of(t[cmd], i), fields);
			}
			const auto counts = metrics::allocations();

			const double ns = time_ns(kIterations, [&] {
				buffer_pool inner{BUFFER_POOL_SIZE};
				const syntax::tree u = syntax::parse(inner, c.source);
				runtime::expander ex{inner, state};
				arena_array<std::string_view> fields{inner, 8};
				const syntax::node_index cmd = u.child_of(u[u.root()], 0);
				if (u[cmd].kind == syntax::node_kind::simple_command)
					for (uint32_t i = 0; i < u[cmd].children_count; ++i)
						ex.expand_word(u, u.child_of(u[cmd], i), fields);
			});

			std::printf("%-22s %10zu %10zu %10zu %12.0f\n", c.name,
			            counts.pool_allocations, counts.heap_allocations,
			            counts.bytes_from_pool + counts.bytes_from_heap, ns);
		}
	}

	// Keystroke latency. The line editor re-lexes on every keystroke for
	// highlighting, so this is the number that decides whether that is viable. It
	// becomes a gate in Phase 4; until then it is a reading.
	std::printf("\nkeystroke latency (lex every prefix of a line)\n");
	{
		const std::string line = "git commit -m \"a message with $VAR and $(cmd)\" --amend";
		const double ns = time_ns(2000, [&] {
			for (size_t n = 1; n <= line.size(); ++n) {
				syntax::lexer lx{std::string_view(line).substr(0, n)};
				while (lx.next().kind != syntax::token_kind::end) {}
			}
		});
		std::printf("  %zu-char line, all %zu prefixes: %.0f ns (%.1f ns per prefix)\n",
		            line.size(), line.size(), ns, ns / static_cast<double>(line.size()));
	}

	// Grapheme scanning. #108's latency clause: boundary walking and cluster
	// width are O(cluster) with two dependent trie loads per codepoint, and the
	// unit of work that matters is one line of a prompt, not one codepoint. A
	// redraw walks the whole line, so that is what is measured.
	std::printf("\ngrapheme scan (one line: boundaries + cluster width)\n");
	std::printf("  %-26s %6s %9s %10s %9s\n",
	            "line", "bytes", "clusters", "ns/scan", "ns/byte");
	{
		struct Line { const char* name; std::string text; };
		std::string cjk, emoji, marks;
		// U+6F22, then U+1F469 U+200D U+1F466, then e with three combining marks.
		for (int i = 0; i < 40; ++i) cjk += "\xe6\xbc\xa2";
		for (int i = 0; i < 10; ++i) emoji += "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa6";
		for (int i = 0; i < 20; ++i) marks += "e\xcc\x81\xcc\x88\xcc\xb1";

		const std::vector<Line> lines = {
			{"ascii, 80 columns",       std::string(80, 'x')},
			{"a real command line",     "git commit -m \"a message with $VAR\" --amend"},
			{"CJK, 40 clusters",        cjk},
			{"ZWJ emoji, 10 clusters",  emoji},
			{"combining marks, 20",     marks},
		};

		for (const Line& l : lines) {
			size_t clusters = 0;
			for (size_t i = 0; i < l.text.size(); ++clusters)
				i = grapheme::next_boundary(l.text, i);

			const double ns = time_ns(20000, [&] {
				int columns = 0;
				for (size_t i = 0; i < l.text.size();) {
					const size_t next = grapheme::next_boundary(l.text, i);
					columns += grapheme::cluster_width(
						std::string_view(l.text).substr(i, next - i));
					i = next;
				}
				benchmark_sink += columns;
			});

			std::printf("  %-26s %6zu %9zu %10.0f %9.2f\n", l.name, l.text.size(),
			            clusters, ns, ns / static_cast<double>(l.text.size()));
		}
	}

	// Fiber switch cost. THE NUMBER THAT DECIDES WHETHER THE WHOLE COOPERATIVE
	// DESIGN IS AFFORDABLE (#198, part of #82): the host loop slices reactors
	// twice per turn, so a switch has to be cheap next to the work between
	// switches - a keystroke's re-lex is hundreds of nanoseconds, so a switch in
	// the single-digit nanoseconds is free and a switch in the microseconds is a
	// redesign.
	//
	// One "round trip" is `scheduler::run_one_slice` on a fiber whose body does
	// nothing but `yield`: host -> fiber -> host, two context switches plus our
	// own bookkeeping. So this is an upper bound on minicoro's bare
	// `mco_resume`/`mco_yield` pair, not a measurement of it in isolation.
	//
	// Measured on the dev machine when this landed (#198), release, arm64:
	//
	//   13.1-13.4 ns   this figure - one `run_one_slice` round trip
	//    6.0- 6.5 ns   minicoro's bare `mco_resume`/`mco_yield` pair, same
	//                  machine, same day, `-O3 -DNDEBUG`, thread_local current-co
	//    5.9- 6.3 ns   the same pair with `-DMCO_NO_MULTITHREAD`
	//
	// So: the research note's 5.4 ns reproduces (6.0-6.5 for the same thing
	// today), the `thread_local` current-coroutine pointer we deliberately keep
	// costs a few tenths of a nanosecond rather than the ~12 ns the note's
	// 5.4-vs-17.3 bracket might suggest - that bracket was MCO_DEBUG's asserts,
	// not the TLS load - and the remaining ~7 ns is OUR bookkeeping per slice:
	// the state transitions, the slice counter, the status read and the
	// error check. Against a keystroke's re-lex, which is hundreds of
	// nanoseconds, both halves are free.
	//
	// In Debug this figure is meaningless: ASan instruments every frame and the
	// watchdog reads the clock twice per slice.
	std::printf("\nfiber switch cost (host -> fiber -> host, one slice each)\n");
	{
		fiber::scheduler sched;
		fiber::fiber& f = sched.spawn(&yield_forever, nullptr, "bench");
		const double ns = time_ns(2000000, [&] { sched.run_one_slice(f); });
		std::printf("  %-40s %12.1f ns\n", "yield/resume round trip", ns);
		std::printf("  %-40s %12zu bytes (guard %zu)\n", "stack per fiber",
		            f.stack().stack_size, f.stack().guard_size);
#ifdef LESH_ENABLE_ASSERTS
		std::printf("  (Debug/RelWithDebInfo: instrumented and watchdogged - "
		            "read the release number)\n");
#endif
	}

	std::printf("\n");
	return 0;
}
