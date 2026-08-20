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

#include "runtime/executor.h"
#include "runtime/expander.h"
#include "runtime/shell_state.h"
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

	std::printf("\n");
	return 0;
}
