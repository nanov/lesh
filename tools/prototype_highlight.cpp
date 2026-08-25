// PROTOTYPE - THROWAWAY. Answers #95: what does a highlighter need that the
// syntax layer does not already give? Walks real parses and prints categorized
// spans; benchmarks re-parse at 4KiB. Not built by CMake, not shipped, delete
// with the branch. Build:
//   clang++ -std=c++23 -O2 -DLESH_ENABLE_ASSERTS -Isrc \
//     tools/prototype_highlight.cpp src/syntax/lexer.cpp src/syntax/parser.cpp \
//     -o /tmp/proto_hl
#include "substrate/arena.h"
#include "syntax/ast.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace lesh;
using namespace lesh::syntax;

static const char* seg_name(token_kind k) {
	switch (k) {
		case token_kind::seg_literal: return "literal";
		case token_kind::seg_single_quoted: return "squote";
		case token_kind::seg_dollar_single_quoted: return "$'quote";
		case token_kind::seg_double_quoted: return "dquote";
		case token_kind::seg_parameter: return "param";
		case token_kind::seg_command_sub: return "cmdsub";
		case token_kind::seg_arithmetic: return "arith";
		case token_kind::seg_tilde: return "tilde";
		default: return "?";
	}
}

// F-21 wants quoted-string-BY-KIND: re-lex one word's interior via C-6.
static void print_word_segments(std::string_view src, const token& t) {
	lexer lx(src, t.offset);
	for (;;) {
		token s = lx.next(lex_mode::word_interior);
		if (s.kind == token_kind::end || s.offset >= t.end_offset()) break;
		if (s.end_offset() > t.end_offset()) break;
		std::printf("      seg [%u,%u) %-8s %.*s\n", s.offset, s.end_offset(),
		            seg_name(s.kind), (int)s.length,
		            s.offset < src.size() ? src.data() + s.offset : "<virtual>");
		if (s.end_offset() == t.end_offset()) break;
	}
}

// The command-name workaround a highlighter must do TODAY: word_role has no
// command_name, so the first word child that is not an assignment is the name.
static void walk(const tree& tr, node_index idx, std::string_view src, int depth) {
	const node& n = tr[idx];
	const span sp = tr.span_of(n);
	const char* kind = "";
	switch (n.kind) {
		case node_kind::simple_command: kind = "simple_command"; break;
		case node_kind::word: kind = "word"; break;
		case node_kind::assignment: kind = "assignment"; break;
		case node_kind::redirect: kind = "redirect"; break;
		case node_kind::here_doc: kind = "here_doc"; break;
		case node_kind::error: kind = "ERROR"; break;
		default: kind = "node"; break;
	}
	std::printf("%*s[%u,%u) %s aux=%u%s%s\n", depth * 2, "", sp.offset, sp.end(),
	            kind, n.aux, n.error != parse_error::none ? " DEFECT:" : "",
	            n.error != parse_error::none && tr.error_detail(n) ? tr.error_detail(n) : "");
	if (n.kind == node_kind::simple_command) {
		bool named = false;
		for (uint32_t i = 0; i < n.children_count; ++i) {
			const node& c = tr[tr.child_of(n, i)];
			if (!named && c.kind == node_kind::word &&
			    static_cast<word_role>(c.aux) == word_role::ordinary) {
				std::printf("%*s  ^ command name (DERIVED, not recorded): %.*s\n",
				            depth * 2, "", (int)tr.span_of(c).length,
				            src.data() + tr.span_of(c).offset);
				named = true;
			}
		}
	}
	if (n.kind == node_kind::word)
		print_word_segments(src, tr.token_at(n.first_token));
	if (n.kind == node_kind::here_doc) {
		const here_doc_body& b = tr.here_doc_at(n.aux);
		std::printf("%*s  body [%u,%u) expand=%d (NOT tokens - walk here_doc_at)\n",
		            depth * 2, "", b.offset, b.offset + b.length, (int)b.expand);
	}
	for (uint32_t i = 0; i < n.children_count; ++i)
		walk(tr, tr.child_of(n, i), src, depth + 1);
}

struct one_alias : alias_source {
	bool lookup_alias(std::string_view name, std::string_view& value) const override {
		if (name == "e") { value = "echo "; return true; }
		return false;
	}
};

static void show(const char* title, std::string_view src, const alias_source* al = nullptr) {
	std::printf("== %s ==  input: %.*s\n", title, (int)src.size(), src.data());
	buffer_pool pool(64 * 1024);
	tree tr = parse(pool, src, al);
	std::printf("  incomplete=%d has_errors=%d\n", (int)tr.incomplete(), (int)tr.has_errors());
	walk(tr, tr.root(), src, 1);
	// Trivia audit: bytes covered by no token at all (comments live here).
	std::vector<bool> covered(src.size(), false);
	for (uint32_t i = 0; i < tr.token_count(); ++i) {
		const token& t = tr.token_at(i);
		for (uint32_t b = t.offset; b < t.end_offset() && b < src.size(); ++b)
			covered[b] = true;
	}
	std::string hole;
	for (size_t b = 0; b < src.size(); ++b)
		if (!covered[b] && src[b] != ' ' && src[b] != '\t' && src[b] != '\n') hole += src[b];
	if (!hole.empty())
		std::printf("  UNCOVERED (no token, not blank): \"%s\"\n", hole.c_str());
	std::printf("\n");
}

int main() {
	// --- capability probes ---------------------------------------------------
	show("quote kinds", "echo 'a' \"b $x\" $'c' plain$(ls)$((1+2)) ~/d");
	show("cmdsub opacity", "echo $(ls -l foo | grep bar)");
	show("defect: unterminated quote", "echo \"x");
	show("defect: unterminated if", "if true; then echo a");
	show("incomplete: trailing backslash", "echo a\\");
	show("comments", "echo hi # a comment");
	show("here-doc", "cat <<END\nbody line\nEND\n");
	one_alias al;
	show("alias WITH table", "e c c", &al);
	show("alias with nullptr (highlight parse)", "e c c");
	show("redirect target", "echo x > /tmp/out");

	// --- benchmarks ----------------------------------------------------------
	std::string big;
	while (big.size() < 4096)
		big += "for f in a b c; do echo \"$f-$(date)\" >> log; done\n"
		       "case $x in a*) echo 'match';; *) v=$((v+1));; esac\n";
	big.resize(4096);
	buffer_pool pool(256 * 1024);
	auto& ctr = metrics::allocations();
	// warm
	{ tree t0 = parse(pool, big); (void)t0; }
	ctr.reset();
	{ tree t1 = parse(pool, big); (void)t1; }
	std::printf("== 4KiB parse: pool_allocs=%zu heap_allocs=%zu pool_bytes=%zu heap_bytes=%zu\n",
	            ctr.pool_allocations, ctr.heap_allocations, ctr.bytes_from_pool,
	            ctr.bytes_from_heap);
	constexpr int N = 2000;
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < N; ++i) { tree t = parse(pool, big); (void)t; }
	auto t1 = std::chrono::steady_clock::now();
	double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / N;
	std::printf("== 4KiB re-parse: %.1f us/parse (N-1 budget: 1000 us)\n", us);
	// 100 KiB paste probe (N-1: <50ms)
	std::string paste;
	while (paste.size() < 100 * 1024) paste += big;
	buffer_pool pool2(1024 * 1024);
	auto p0 = std::chrono::steady_clock::now();
	{ tree t = parse(pool2, paste); (void)t; }
	auto p1 = std::chrono::steady_clock::now();
	std::printf("== 100KiB parse: %.2f ms (N-1 budget: 50 ms)\n",
	            std::chrono::duration<double, std::milli>(p1 - p0).count());
	return 0;
}
