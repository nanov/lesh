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
#include "leshper/abi.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "runtime/executor.h"
#include "runtime/expander.h"
#include "runtime/shell_state.h"
#include "substrate/grapheme.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"
#include "ui/history_search.h"
#include "ui/loop.h"
#include "ui/reactors.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
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

// ---------------------------------------------------------------------------
// THE AUTOSUGGESTER'S WORST CASE (#206), and the harness it needs
// ---------------------------------------------------------------------------
//
// A history whose entries share no prefix with what is typed, so the walk runs
// to the end: no strict extension is ever found, `max_matches` is zero, and
// every one of the entries is examined. That is the shape #202 measured at 6x
// under ASan and the one this section re-measures in release.
//
// The history is deliberately ordinary lines rather than one repeated string:
// `match_entry` in prefix mode compares the first bytes, so entries that all
// begin with the same character would measure a different comparison than a
// real history does.
ui::vector_history_source novel_history(std::size_t entries) {
	std::vector<std::string> lines;
	lines.reserve(entries);
	static const char* const shapes[] = {
		"git commit -m \"work on %zu\"", "ls -la /usr/share/doc/%zu",
		"cat file%zu.txt | grep pattern | wc -l", "make -j8 target%zu",
		"echo hello world %zu", "grep -rn needle src/%zu",
	};
	char line[128];
	for (std::size_t i = 0; i < entries; ++i) {
		std::snprintf(line, sizeof(line), shapes[i % 6], i);
		lines.emplace_back(line);
	}
	return ui::vector_history_source{std::move(lines)};
}

// The host, over a pipe, with the real autosuggester on its real fiber. The
// output goes to /dev/null rather than to a pipe nobody drains: a render per
// keystroke fills a 64 KB pipe long before the measurement is over.
struct driven_loop {
	driven_loop() {
		std::ignore = ::pipe(_in);
		::fcntl(_in[0], F_SETFL, O_NONBLOCK);
		_null = ::open("/dev/null", O_WRONLY);
		options.manage_terminal = false;
		options.prompt = "> ";
	}
	~driven_loop() {
		for (int fd : {_in[0], _in[1], _null})
			if (fd >= 0)
				::close(fd);
	}
	driven_loop(const driven_loop&) = delete;
	driven_loop& operator=(const driven_loop&) = delete;

	[[nodiscard]] ui::loop_fds fds() const { return ui::loop_fds{_in[0], _null}; }
	void type(std::string_view bytes) const {
		std::ignore = ::write(_in[1], bytes.data(), bytes.size());
	}

	ui::loop_options options;
	int _in[2]{-1, -1};
	int _null = -1;
};

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

	// THE COOPERATIVE WALK'S PRICE (#206). The switch above is the cheapest part
	// of a yield; the yield the shell actually takes goes back to the HOST, which
	// polls the terminal before it comes back. So the three rows below are the
	// three layers of one yield, each measured on its own:
	//
	//   run_one_slice   host -> fiber -> host                     (the row above)
	//   tick            + the snapshot, the sort and `runnable`
	//   turn(0)         + the readiness check, the drains, the render check
	//
	// and the fourth group is what they add up to on the reactor that takes the
	// longest walk in the shell: the autosuggester, on a history whose entries the
	// typed prefix never extends.
	//
	// WHERE #206 FOUND THE COST, and it was none of the three: `turn(0)` was
	// 6706 ns of which ~6500 was one `poll(2)` that found nothing, which XNU
	// charges 8 us for and `select` charges 0.2 (see `ready_now` in loop.cpp).
	// The walk was then still 25x its own no-yield time on arithmetic alone - an
	// entry is 4 ns and a yield is 155 - so the walk strides its cancellation
	// poll (see `history_search::poll_every`). This machine, release:
	//
	//                                        before      after
	//   turn(0), nothing to do              6706.6 ns   223.2 ns
	//   walk, poll per entry, no yield        31.2 us    22.2 us
	//   walk, through the loop             19885.1 us    26.9 us
	//   ...as a multiple of no-yield            984x      1.21x
	//   yields per walk                          5000        20
	//   gap between polls, p95                15.58 us   2.92 us
	std::printf("\ncooperative yield, layer by layer (#206)\n");
	{
		fiber::scheduler sched;
		std::ignore = sched.spawn(&yield_forever, nullptr, "bench");
		const double tick_ns = time_ns(1000000, [&] { benchmark_sink += sched.tick() ? 1 : 0; });
		std::printf("  %-40s %12.1f ns\n", "tick() with one yielding fiber", tick_ns);

		driven_loop host;
		leshper::registry reg;
		ui::event_loop loop{host.fds(), host.options};
		loop.attach_registry(reg);
		loop.enter_read();
		const double idle_ns = time_ns(200000, [&] { benchmark_sink += loop.turn(0).events; });
		std::printf("  %-40s %12.1f ns\n", "turn(0), nothing to do", idle_ns);
	}

	std::printf("\nautosuggester walk (%d entries, prefix nothing extends)\n", 5000);
	{
		constexpr std::size_t kEntries = 5000;
		const ui::vector_history_source source = novel_history(kEntries);
		double baseline_ns = 0.0;

		// THE FLOOR: the same walk with nothing under it. `history_search::run`
		// with no cancel poll at all is the work itself, and nothing the shell
		// does can be cheaper than this.
		ui::history_search::options search;
		search.search = ui::history_search::mode::prefix;
		search.max_matches = 0;
		search.max_ranges = 0;
		const double bare_ns = time_ns(200, [&] {
			ui::history_search searcher{search};
			const auto walked = searcher.run("zqx", source, {}, {});
			benchmark_sink += static_cast<int>(walked.entries_examined);
		});
		std::printf("  %-40s %12.1f us (%.1f ns/entry)\n", "no poll, searcher alone",
		            bare_ns / 1000.0, bare_ns / static_cast<double>(kEntries));

		// AND THE BASELINE THE 1.5x IS 1.5x OF: the same reactor, the same token
		// and the same poll per entry, on the HOST'S OWN STACK - which is what
		// `loop_harness` is, and it is the shipped path a reactor takes when
		// nothing gave it a fiber (`lesh_request::cooperate` is null, so the poll
		// reads the flag and returns). The difference between this row and the
		// next is exactly what YIELDING costs.
		{
			leshper::registry reg;
			ui::owned_autosuggester sugg{&source};
			std::ignore = ui::register_autosuggester(reg, sugg.get());
			leshper::loop_harness harness{reg};
			leshper::state typed;
			typed.buffer.replace(typed.buffer.begin_position(), typed.buffer.begin_position(),
			                     "zqx");
			typed.cursor = typed.buffer.end_position();
			typed.gen.bump();
			const double polled_ns = time_ns(200, [&] {
				benchmark_sink +=
					static_cast<int>(harness.react(typed, LESH_EVENT_BUFFER_CHANGED).size());
			});
			std::printf("  %-40s %12.1f us (%.1f ns/entry)\n", "poll per entry, no yield",
			            polled_ns / 1000.0, polled_ns / static_cast<double>(kEntries));
			baseline_ns = polled_ns;
		}

		// AND THROUGH THE HOST, which is the shell's real number: every keystroke
		// is one full walk, and every cancellation poll on the way is a yield.
		driven_loop host;
		leshper::registry reg;
		ui::owned_autosuggester sugg{&source};
		ui::event_loop loop{host.fds(), host.options};
		loop.attach_registry(reg);
		std::ignore = ui::register_autosuggester(reg, sugg.get());
		loop.enter_read();

		// AND THE GAP BETWEEN TERMINAL POLLS, which is the keystroke latency this
		// ticket is about. A key that arrives just after one poll waits for the next
		// one, so the interval between consecutive turns IS the latency a walk in
		// flight adds - measured here rather than over a pty, where 60 us of kernel
		// and interpreter sit on top of a number whose budget is 100.
		constexpr std::size_t kKeystrokes = 40;
		std::vector<double> gaps;
		gaps.reserve(1u << 19);
		const auto started = steady_clock::now();
		for (std::size_t k = 0; k < kKeystrokes; ++k) {
			const std::size_t before = loop.reactor_computes("autosuggester");
			host.type("z");
			auto last = steady_clock::now();
			while (loop.reactor_computes("autosuggester") == before
			       || loop.reactors().runnable(ui::group_mask(ui::fiber_group::emitters))) {
				benchmark_sink += loop.turn(0).events;
				const auto now = steady_clock::now();
				gaps.push_back(static_cast<double>(
					duration_cast<nanoseconds>(now - last).count()));
				last = now;
			}
		}
		const double per_walk_ns =
			static_cast<double>(duration_cast<nanoseconds>(steady_clock::now() - started).count())
			/ static_cast<double>(kKeystrokes);
		std::printf("  %-40s %12.1f us (%.1f ns/entry, %.2fx)\n", "through the loop, one walk",
		            per_walk_ns / 1000.0, per_walk_ns / static_cast<double>(kEntries),
		            per_walk_ns / baseline_ns);
		std::printf("  %-40s %12zu slices, %zu yields\n", "for that last walk",
		            loop.reactor_slices("autosuggester"), loop.reactor_yields("autosuggester"));
		std::sort(gaps.begin(), gaps.end());
		const auto at = [&gaps](double p) {
			return gaps.empty() ? 0.0 : gaps[static_cast<std::size_t>(
				static_cast<double>(gaps.size() - 1) * p)];
		};
		std::printf("  %-40s p50 %.2f us  p95 %.2f us  max %.2f us\n",
		            "keystroke wait (gap between polls)", at(0.5) / 1000.0,
		            at(0.95) / 1000.0, gaps.empty() ? 0.0 : gaps.back() / 1000.0);
	}

	// Command-boundary cost. THE OTHER HALF OF THE COOPERATIVE DESIGN'S PRICE
	// (#199, step 1a of #145): the runtime calls
	// `cooperation::on_command_boundary()` once per command at the same place it
	// polls for pending traps, so the cheapest command there is - a loop body of
	// one arithmetic assignment - carries one indirect call to an empty function
	// per iteration. A tight `while` loop is the worst case the shell has: the
	// boundary work is a fixed cost per command, so the shorter the command the
	// larger its share.
	//
	// Measured through `run_input`, not through `run`, because `run_input` is what
	// the script path and the interactive path both call, and it is where the
	// per-command read/execute alternation lives.
	//
	// Three passes over 100000 iterations rather than many passes over few: the
	// number wanted is nanoseconds per ITERATION, and a loop this long buries the
	// parse and the pool setup around it.
	// TWO SPELLINGS OF THE SAME LOOP, and they should now agree to within a few
	// percent. `[ $i -lt 100000 ]` is the shape #199 named, and it used to be ~19x
	// the cost of `test $i -lt 100000` on this machine - 17400ns an iteration
	// against 912 - almost all of it SYSTEM time: `[` holds a pattern character,
	// so every iteration attempted pathname expansion on a word that could only
	// ever match itself and paid an opendir/readdir/closedir for it. #204 taught
	// the glob gate POSIX 2.13.1's rule (an unterminated `[` is an ordinary
	// character) and the row fell to 939-952ns.
	//
	// Both are kept, because a DIVERGENCE between them is now the signal: the two
	// loops differ only in how the condition is spelled, so a gap reappearing here
	// means a word that names itself has started costing syscalls again.
	std::printf("\ncommand-boundary cost (while loop through run_input)\n");
	{
		constexpr size_t kLoopIterations = 100000;
		const struct { const char* name; const char* source; } loops[] = {
			{"[ ... ] condition", "i=0; while [ $i -lt 100000 ]; do i=$((i+1)); done"},
			{"test ... condition", "i=0; while test $i -lt 100000; do i=$((i+1)); done"},
		};
		for (const auto& loop : loops) {
			buffer_pool pool{BUFFER_POOL_SIZE};
			runtime::shell_state state;
			runtime::tree_walking_executor exec{pool, state};
			const double ns =
				time_ns(3, [&] { benchmark_sink += exec.run_input(loop.source, false); });
			std::printf("  %-40s %12.0f ns (%.0f ns per iteration)\n", loop.name, ns,
			            ns / static_cast<double>(kLoopIterations));
		}
	}

	std::printf("\n");
	return 0;
}
