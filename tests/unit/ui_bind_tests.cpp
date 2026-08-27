// The `bind` builtin, and the link boundary it reaches across (#117 decision 7,
// #118).
//
// THE HOST'S SIDE OF THE KEYMAP, and that is why it is not in
// `leshper_keymap_tests.cpp`. Everything here needs `runtime/` - the builtin
// table, a `shell_state`, and a redirection of the process's real stdout to read
// what the builtin printed. The editor's own tests include nothing outside
// `leshper/` and `substrate/`, which is the link rule #168 made structural,
// stated once more in test form.
//
// What is under test is the ADAPTER as much as the builtin: `binding_console` is
// the narrow interface the runtime declares and something that links both sides
// implements, and the class below is that implementation.

#include "leshper/abi.h"
#include "leshper/editor.h"
#include "leshper/event.h"
#include "leshper/keymap.h"
#include "leshper/registry.h"
#include "leshper/state.h"
#include "runtime/builtins.h"
#include "runtime/shell_state.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace lesh::leshper;

namespace {

// Named for what a user does, so a test reads as a session. The keymap file has
// its own copies; these are the two the `bind` tests need.
void press(state& s, char32_t codepoint, key_modifiers modifiers = {}) {
	(void)step(s, key_event::of(codepoint, modifiers));
}
void press(state& s, named_key key, key_modifiers modifiers = {}) {
	(void)step(s, key_event::of(key, modifiers));
}

void type(state& s, std::string_view text) {
	for (const char byte : text)
		press(s, static_cast<char32_t>(static_cast<unsigned char>(byte)));
}

std::size_t cursor_of(const state& s) { return s.cursor.byte_offset(); }

// THE ADAPTER, and it lives in a TEST because of where the link graph puts it.
//
// `lesh_runtime` does not link `lesh_leshper` - `lesh` is built on
// `lesh_runtime lesh_syntax lesh_ui` and nothing else - so a builtin cannot call
// a keymap function directly, and making it able to would drag the whole editor
// into every `lesh -c`. `binding_console` is the narrow interface that crosses,
// declared by the runtime and implemented wherever both sides are linked. This
// class is that implementation; the loop will need the same twenty lines when it
// is wired up, and this test proves they are enough.
class leshper_binding_console final : public lesh::runtime::binding_console {
public:
	explicit leshper_binding_console(editing_context& context) : _context(&context) {}

	void keymap_names(std::vector<std::string>& into) const override {
		_context->keymaps().names(into);
	}

	outcome create_keymap(std::string_view name, std::string_view from) override {
		return _context->keymaps().create(name, from) != nullptr ? outcome::ok
		                                                         : outcome::no_such_keymap;
	}

	outcome bind_key(std::string_view name, std::string_view notation,
	                 std::string_view action) override {
		keymap* map = keymap_for(name);
		if (map == nullptr)
			return outcome::no_such_keymap;
		std::string encoded;
		if (!parse_key_notation(notation, encoded))
			return outcome::bad_notation;
		if (!action.empty()) {
			// Bound to something that exists, or the binding is a typo that only
			// shows up as a dead key months later.
			int32_t exists = 0;
			const std::string name_of_action{action};
			if (lesh_action_exists(&_context->actions(), name_of_action.c_str(), &exists)
			        != LESH_OK
			    || exists == 0)
				return outcome::no_such_action;
		}
		map->bind(encoded, action);
		return outcome::ok;
	}

	outcome lookup_key(std::string_view name, std::string_view notation,
	                   std::string& action_out) const override {
		const keymap* map = keymap_for(name);
		if (map == nullptr)
			return outcome::no_such_keymap;
		std::string encoded;
		if (!parse_key_notation(notation, encoded))
			return outcome::bad_notation;
		const std::string* bound = map->action_for(encoded);
		action_out = bound != nullptr ? *bound : std::string{};
		return outcome::ok;
	}

	outcome list_bindings(std::string_view name,
	                      std::vector<std::pair<std::string, std::string>>& into) const override {
		const keymap* map = keymap_for(name);
		if (map == nullptr)
			return outcome::no_such_keymap;
		into.clear();
		for (const keymap::entry& one : map->entries())
			into.emplace_back(render_key_notation(one.keys), one.action);
		return outcome::ok;
	}

private:
	[[nodiscard]] keymap* keymap_for(std::string_view name) const {
		return _context->keymaps().find(name.empty() ? keymap_registry::emacs : name);
	}

	editing_context* _context;
};

// Runs one `bind` command line and answers what it wrote and what it returned.
//
// A redirection of the real descriptors rather than a stub stream, because the
// builtin writes with printf and the point is to test the builtin.
struct bind_run {
	int status = 0;
	std::string output;
};

// The console the next `run_bind` hands its shell. A file-scope pointer HERE
// rather than in the shell (#134 moved the real seam onto `shell_state`) only
// because `run_bind` builds its own shell per call: the guard below installs
// one for the duration of a test and the shell picks it up.
lesh::runtime::binding_console* g_test_console = nullptr;

bind_run run_bind(const std::vector<std::string>& words) {
	std::vector<char*> argv;
	std::vector<std::string> owned = words;
	for (std::string& one : owned)
		argv.push_back(one.data());
	argv.push_back(nullptr);

	const lesh::testing::temp_path scratch;
	const std::string path = scratch.file("out");
	std::fflush(stdout);
	std::fflush(stderr);
	const int saved_out = ::dup(STDOUT_FILENO);
	const int saved_err = ::dup(STDERR_FILENO);
	const int into = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	EXPECT_GE(into, 0);
	::dup2(into, STDOUT_FILENO);
	::dup2(into, STDERR_FILENO);
	::close(into);

	lesh::runtime::shell_state shell;
	shell.set_binding_console(g_test_console);
	lesh::runtime::builtin_result result;
	const bool ran = lesh::runtime::try_run_builtin(shell, argv.data(), result, false);

	std::fflush(stdout);
	std::fflush(stderr);
	::dup2(saved_out, STDOUT_FILENO);
	::dup2(saved_err, STDERR_FILENO);
	::close(saved_out);
	::close(saved_err);

	EXPECT_TRUE(ran) << "bind is not in the handler table";
	std::ifstream in{path};
	std::ostringstream text;
	text << in.rdbuf();
	return bind_run{result.status, text.str()};
}

// Installs a console for the duration of one test and takes it away afterwards,
// so a test that leaves one behind cannot make the next test's `bind` succeed.
class console_guard {
public:
	explicit console_guard(editing_context& context) : _console(context) {
		g_test_console = &_console;
	}
	~console_guard() { g_test_console = nullptr; }

	console_guard(const console_guard&) = delete;
	console_guard& operator=(const console_guard&) = delete;

private:
	leshper_binding_console _console;
};

} // namespace

TEST(UiBind, WithNoLineEditorItSaysSoRatherThanPretending) {
	// A non-interactive shell has no keymaps to mutate, and `bind` in an rc file
	// guarded for both kinds of shell must not end the script - so this is an
	// OPERATIONAL failure and not a usage error.
	g_test_console = nullptr;
	const bind_run ran = run_bind({"bind", "-l"});
	EXPECT_EQ(ran.status, 1);
	EXPECT_NE(ran.output.find("no line editor"), std::string::npos);
}

TEST(UiBind, ListsTheKeymapsItHas) {
	state s;
	const console_guard guard{context_of(s)};
	const bind_run ran = run_bind({"bind", "-l"});
	EXPECT_EQ(ran.status, 0);
	EXPECT_EQ(ran.output,
	          "emacs\npager\nvi_command\nvi_find_char\nvi_insert\n"
	          "vi_operator_pending\nvi_replace_char\nvi_visual\n");
}

TEST(UiBind, BindsAndThenAnswersWhatItBound) {
	state s;
	editing_context& context = context_of(s);
	const console_guard guard{context};

	EXPECT_EQ(run_bind({"bind", "<C-t>", "end_of_line"}).status, 0);
	const bind_run asked = run_bind({"bind", "<C-t>"});
	EXPECT_EQ(asked.status, 0);
	EXPECT_EQ(asked.output, "<C-t> end_of_line\n");

	// And the shell's edit really changed the editor's dispatch.
	type(s, "echo hi");
	press(s, named_key::home);
	press(s, static_cast<char32_t>(0x14));   // Ctrl-T
	EXPECT_EQ(cursor_of(s), 7u);

	// An unbound sequence prints nothing and answers 1, so it is a test.
	// `<C-q>` rather than `<C-y>`: #119 gave `<C-y>` to `yank`, the emacs side of
	// the one kill store.
	const bind_run missing = run_bind({"bind", "<C-q>"});
	EXPECT_EQ(missing.status, 1);
	EXPECT_TRUE(missing.output.empty());
}

TEST(UiBind, TheMinusMOptionSelectsWhichKeymapTheOperandsApplyTo) {
	state s;
	editing_context& context = context_of(s);
	const console_guard guard{context};

	EXPECT_EQ(run_bind({"bind", "-m", "vi_command", "H", "beginning_of_line"}).status, 0);
	EXPECT_EQ(run_bind({"bind", "-m", "vi_command", "H"}).output, "H beginning_of_line\n");
	// The default keymap is emacs, and the binding did not land there.
	EXPECT_EQ(run_bind({"bind", "H"}).status, 1);

	const bind_run nonesuch = run_bind({"bind", "-m", "nonesuch", "H", "undo"});
	EXPECT_EQ(nonesuch.status, 1);
	EXPECT_NE(nonesuch.output.find("no such keymap"), std::string::npos);
}

TEST(UiBind, MinusNCreatesAKeymapAndCopiesOneWhenAsked) {
	state s;
	editing_context& context = context_of(s);
	const console_guard guard{context};

	EXPECT_EQ(run_bind({"bind", "-N", "vi_visual", "vi_command"}).status, 0);
	ASSERT_NE(context.keymaps().find("vi_visual"), nullptr);
	EXPECT_TRUE(*context.keymaps().find("vi_visual")
	            == *context.keymaps().find(keymap_registry::vi_command));

	EXPECT_EQ(run_bind({"bind", "-N", "empty"}).status, 0);
	ASSERT_NE(context.keymaps().find("empty"), nullptr);
	EXPECT_TRUE(context.keymaps().find("empty")->empty());

	const bind_run nonesuch = run_bind({"bind", "-N", "hopeless", "nonesuch"});
	EXPECT_EQ(nonesuch.status, 1);
	EXPECT_EQ(context.keymaps().find("hopeless"), nullptr);
}

TEST(UiBind, TheListingIsReInputtable) {
	// `alias`'s property, and for the same reason: `bind -m emacs > f` and reading
	// `f` back has to rebuild the same table, which is only true if every line is
	// notation the parser accepts.
	state s;
	editing_context& context = context_of(s);
	const console_guard guard{context};

	const bind_run listed = run_bind({"bind"});
	ASSERT_EQ(listed.status, 0);
	ASSERT_FALSE(listed.output.empty());

	keymap rebuilt;
	std::istringstream lines{listed.output};
	std::string written;
	std::string action;
	size_t rows = 0;
	while (lines >> written >> action) {
		std::string encoded;
		ASSERT_TRUE(parse_key_notation(written, encoded)) << written;
		rebuilt.bind(encoded, action);
		++rows;
	}
	EXPECT_EQ(rows, context.keymaps().find(keymap_registry::emacs)->entries().size());
	EXPECT_TRUE(rebuilt == *context.keymaps().find(keymap_registry::emacs));
}

TEST(UiBind, ARefusedCommandLineIsAUsageErrorAndAWrongNameIsNot) {
	state s;
	const console_guard guard{context_of(s)};

	// Usage errors: status 2, the answer every other builtin gives for a command
	// line that is not the shape the utility accepts.
	EXPECT_EQ(run_bind({"bind", "-z"}).status, 2);
	EXPECT_EQ(run_bind({"bind", "-m"}).status, 2);
	EXPECT_EQ(run_bind({"bind", "-l", "extra"}).status, 2);
	EXPECT_EQ(run_bind({"bind", "a", "b", "c"}).status, 2);

	// A name that does not resolve is a failure of the operation, not of the
	// command line.
	const bind_run typo = run_bind({"bind", "<C-t>", "backwrad_char"});
	EXPECT_EQ(typo.status, 1);
	EXPECT_NE(typo.output.find("no such action"), std::string::npos);
	const bind_run garbage = run_bind({"bind", "<Nonesuch>", "undo"});
	EXPECT_EQ(garbage.status, 1);
	EXPECT_NE(garbage.output.find("not a key sequence"), std::string::npos);
}
