#include "runtime/builtins.h"

#include "runtime/shell_state.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

// The `prompt` builtin (#157, spec §6.10).
//
// WHAT IS UNDER TEST HERE IS THE BUILTIN AND NOTHING ELSE, and the fake console
// below is what makes that true. `bind`'s tests live in
// `leshper_keymap_tests.cpp` and drive a REAL keymap registry, because what they
// were proving was that twenty adapter lines are enough to cross the link
// boundary. That question is already answered for the prompt in
// `ui_prompt_tests.cpp`, over a real engine. What is left, and what belongs
// here, is the half `bind`'s tests cannot separate: which console verb a given
// command line reaches, how many times, and what the builtin prints when the far
// side refuses. A recording fake answers all three exactly; a real engine would
// answer them through a rendering, which is the wrong side of the seam to be
// reading a builtin's behaviour off.
//
// It also keeps this file leshper-free, which is the same rule the builtin
// itself is written under - `builtins.cpp` includes no leshper header, and a
// test of it that had to would be evidence the door had been left open.

using namespace lesh::runtime;

namespace {

using surface = prompt_console::surface;
using outcome = prompt_console::outcome;

std::string_view name_of(surface which) {
	return which == surface::continuation ? "continuation" : "left";
}

// Every verb the builtin can reach, written down as a string the test can
// compare. A recording of CALLS rather than of resulting state: "`prompt -r`
// resets both surfaces" is a statement about two calls in an order, and a fake
// that only remembered its final state could not tell that apart from one call
// that happened to leave the same state behind.
class fake_prompt_console final : public prompt_console {
public:
	// What the next `set` answers. `ok` unless a test asks for a refusal.
	outcome set_answer = outcome::ok;
	std::string set_error;

	std::vector<std::string> modules{"git", "path", "status"};

	// Every call, in order: "set(left,x)", "text(continuation)", "default(left)".
	//
	// Mutable because two of the verbs are const, and their constness is a
	// statement about the PROMPT rather than about whether the fake saw the
	// question - `text` not recording its call would leave the reading form's
	// tests unable to say which surface was asked.
	mutable std::vector<std::string> calls;

	void module_names(std::vector<std::string>& into) const override {
		_note("names()");
		into = modules;
	}

	outcome clear(surface which) override {
		_note(std::string{"clear("}.append(name_of(which)).append(")"));
		_text[which == surface::continuation].clear();
		return outcome::ok;
	}

	outcome use_default(surface which) override {
		_note(std::string{"default("}.append(name_of(which)).append(")"));
		_text[which == surface::continuation] = "<default>";
		return outcome::ok;
	}

	outcome set(surface which, std::string_view template_text,
	            std::string& error_out) override {
		_note(std::string{"set("}.append(name_of(which)).append(",")
		          .append(template_text).append(")"));
		if (set_answer != outcome::ok) {
			error_out = set_error;
			// ATOMIC ON REFUSAL, which is the promise the header records and the
			// thing a real console owes its caller. The fake keeps it so that a test
			// asserting "the prompt is unchanged" is asserting something the builtin
			// could actually observe.
			return set_answer;
		}
		_text[which == surface::continuation] = std::string{template_text};
		return outcome::ok;
	}

	void text(surface which, std::string& out) const override {
		_note(std::string{"text("}.append(name_of(which)).append(")"));
		out = _text[which == surface::continuation];
	}

	// Seeds a surface without recording a call, for the tests whose subject is
	// the READING form.
	void seed(surface which, std::string_view what) {
		_text[which == surface::continuation] = std::string{what};
	}

private:
	void _note(std::string what) const { calls.push_back(std::move(what)); }

	std::string _text[2];
};

// Runs one `prompt` command line and answers what it wrote and what it returned.
//
// Dispatched through `try_run_builtin` rather than by calling `builtin_prompt`
// directly, so the registry row is on the path: a builtin with a handler and no
// registry entry is a name the command search never reaches (#35), and that is
// exactly the failure a direct call would hide.
//
// The real descriptors are redirected rather than a stream stubbed, because the
// builtin writes with printf and report() writes to stderr - the point is to
// test what a user sees, and both halves of it.
struct prompt_run {
	int status = 0;
	std::string output;
};

prompt_console* g_test_console = nullptr;

prompt_run run_prompt(const std::vector<std::string>& words) {
	std::vector<std::string> owned = words;
	std::vector<char*> argv;
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

	shell_state shell;
	shell.set_prompt_console(g_test_console);
	builtin_result result;
	const bool ran = try_run_builtin(shell, argv.data(), result, false);

	std::fflush(stdout);
	std::fflush(stderr);
	::dup2(saved_out, STDOUT_FILENO);
	::dup2(saved_err, STDERR_FILENO);
	::close(saved_out);
	::close(saved_err);

	EXPECT_TRUE(ran) << "prompt is not in the handler table";
	std::ifstream in{path};
	std::ostringstream text;
	text << in.rdbuf();
	return prompt_run{result.status, text.str()};
}

// Installs a console for the duration of one test and takes it away afterwards,
// so a test that leaves one behind cannot make the next test's `prompt` succeed
// - which is how the no-console case would silently stop being tested.
class console_guard {
public:
	explicit console_guard(fake_prompt_console& console) { g_test_console = &console; }
	~console_guard() { g_test_console = nullptr; }

	console_guard(const console_guard&) = delete;
	console_guard& operator=(const console_guard&) = delete;
};

} // namespace

TEST(PromptBuiltin, WithNoLineEditorItSaysSoRatherThanPretending) {
	// `bind`'s answer, and it has to be: an rc file guarded for both kinds of
	// shell configures the prompt and the keymaps in the same breath, and a
	// non-interactive `lesh -c` running it must reach the end of the file.
	g_test_console = nullptr;
	const prompt_run ran = run_prompt({"prompt", "{path}"});
	EXPECT_EQ(ran.status, 1);
	EXPECT_NE(ran.output.find("no line editor"), std::string::npos);

	// Every form, not just the setting one: a shell without an editor has no
	// module registry to list and no default to put back either.
	EXPECT_EQ(run_prompt({"prompt"}).status, 1);
	EXPECT_EQ(run_prompt({"prompt", "-l"}).status, 1);
	EXPECT_EQ(run_prompt({"prompt", "-r"}).status, 1);
}

TEST(PromptBuiltin, BareFormWritesTheTemplateTheSurfaceWasSetFrom) {
	fake_prompt_console console;
	const console_guard guard{console};
	console.seed(surface::left, "{path} $ ");
	console.seed(surface::continuation, "> ");

	const prompt_run left = run_prompt({"prompt"});
	EXPECT_EQ(left.status, 0);
	EXPECT_EQ(left.output, "{path} $ \n");

	const prompt_run carried_on = run_prompt({"prompt", "-c"});
	EXPECT_EQ(carried_on.status, 0);
	EXPECT_EQ(carried_on.output, "> \n");

	// And it asked the right surface each time, rather than printing something it
	// happened to have.
	EXPECT_EQ(console.calls, (std::vector<std::string>{"text(left)", "text(continuation)"}));
}

TEST(PromptBuiltin, AnUnsetSurfaceWritesAnEmptyLineRatherThanNothing) {
	// One line per invocation whatever the state, so `prompt | wc -l` is 1 and a
	// script reading the output does not have to tell "empty template" from
	// "command wrote nothing". The default prompt is a TABLE rather than a
	// template, so this is the state a fresh shell is actually in.
	fake_prompt_console console;
	const console_guard guard{console};
	const prompt_run ran = run_prompt({"prompt"});
	EXPECT_EQ(ran.status, 0);
	EXPECT_EQ(ran.output, "\n");
}

TEST(PromptBuiltin, OneOperandSetsTheSurfaceTheOptionsSelected) {
	fake_prompt_console console;
	const console_guard guard{console};

	EXPECT_EQ(run_prompt({"prompt", "x"}).status, 0);
	EXPECT_EQ(run_prompt({"prompt", "-c", "y"}).status, 0);
	EXPECT_EQ(console.calls,
	          (std::vector<std::string>{"set(left,x)", "set(continuation,y)"}));

	// ONE call each, and the operand arrives whole. The assembly verbs are not on
	// this path at all: the builtin hands over a string and the far side parses
	// it, which is the arrangement the template language depends on.
	EXPECT_EQ(run_prompt({"prompt"}).output, "x\n");
	EXPECT_EQ(run_prompt({"prompt", "-c"}).output, "y\n");
}

TEST(PromptBuiltin, ATemplateTheConsoleRefusesIsReportedInTheConsolesOwnWords) {
	fake_prompt_console console;
	const console_guard guard{console};
	console.seed(surface::left, "{path} $ ");
	console.set_answer = outcome::bad_template;
	console.set_error = "unclosed { at byte 4";

	const prompt_run ran = run_prompt({"prompt", "{git"});
	EXPECT_EQ(ran.status, 1);
	// Verbatim after "prompt: ", because the parser is the only thing that can
	// know which byte was wrong and a runtime-side wording could only be vaguer.
	EXPECT_EQ(ran.output, "lesh: prompt: unclosed { at byte 4\n");

	// AND THE PROMPT STILL STANDS. The atomicity is the console's promise, but a
	// builtin that reported the failure and then cleared the surface would break
	// it just as thoroughly, so the reading form is asked afterwards.
	console.set_answer = outcome::ok;
	EXPECT_EQ(run_prompt({"prompt"}).output, "{path} $ \n");
}

TEST(PromptBuiltin, MinusLWritesTheModuleNamesTheConsoleHas) {
	fake_prompt_console console;
	const console_guard guard{console};
	const prompt_run ran = run_prompt({"prompt", "-l"});
	EXPECT_EQ(ran.status, 0);
	EXPECT_EQ(ran.output, "git\npath\nstatus\n");
	EXPECT_EQ(console.calls, (std::vector<std::string>{"names()"}));

	// The ORDER is the console's, and this asserts the builtin does not impose a
	// second one: a sort here would be an ordering free to disagree with the
	// ABI's, and the fake hands back a deliberately unsorted list to prove it.
	console.modules = {"zeta", "alpha"};
	console.calls.clear();
	EXPECT_EQ(run_prompt({"prompt", "-l"}).output, "zeta\nalpha\n");
}

TEST(PromptBuiltin, MinusRPutsTheDefaultBackOnBothSurfaces) {
	fake_prompt_console console;
	const console_guard guard{console};
	console.seed(surface::left, "configured");
	console.seed(surface::continuation, "configured too");

	const prompt_run ran = run_prompt({"prompt", "-r"});
	EXPECT_EQ(ran.status, 0);
	EXPECT_TRUE(ran.output.empty()) << "a reset that worked has nothing to say";
	// EXACTLY two calls, one per surface. `-r` is the undo for a session's worth
	// of configuration, and a reset that reached only the left prompt would leave
	// a shell in a state no rc file ever put it in.
	EXPECT_EQ(console.calls,
	          (std::vector<std::string>{"default(left)", "default(continuation)"}));

	console.calls.clear();
	EXPECT_EQ(run_prompt({"prompt"}).output, "<default>\n");
	EXPECT_EQ(run_prompt({"prompt", "-c"}).output, "<default>\n");
}

TEST(PromptBuiltin, TheUsageErrorsAndTheStatusTheyShare) {
	fake_prompt_console console;
	const console_guard guard{console};

	// A template is ONE word. `prompt {path} {git}` is an unquoted one, and
	// setting the prompt to the first half is the worst available answer.
	const prompt_run two = run_prompt({"prompt", "{path}", "{git}"});
	EXPECT_EQ(two.status, 2);
	EXPECT_NE(two.output.find("too many operands"), std::string::npos);

	// `-l` reads a registry that has no surface, and `-r` deliberately takes no
	// surface, so neither takes an operand and neither takes `-c`.
	EXPECT_EQ(run_prompt({"prompt", "-l", "x"}).status, 2);
	EXPECT_EQ(run_prompt({"prompt", "-r", "x"}).status, 2);
	EXPECT_EQ(run_prompt({"prompt", "-r", "-c"}).status, 2);
	EXPECT_EQ(run_prompt({"prompt", "-l", "-c"}).status, 2);
	EXPECT_EQ(run_prompt({"prompt", "-l", "-r"}).status, 2);

	// An option that does not exist is a usage error and not an operand, which is
	// what `lesh::args` gives every builtin in the tree for free (#155/#148).
	const prompt_run unknown = run_prompt({"prompt", "-Z"});
	EXPECT_EQ(unknown.status, 2);
	EXPECT_NE(unknown.output.find("Illegal option -Z"), std::string::npos);

	// NOT ONE OF THEM REACHED THE CONSOLE. The command line is read before the
	// console is asked for, so a malformed one is malformed in every shell rather
	// than only in the interactive shell that would have run it.
	EXPECT_TRUE(console.calls.empty());
}

TEST(PromptBuiltin, TheSeparatorEndsTheOptionsSoATemplateMayBeginWithAHyphen) {
	// `--` is XBD 12.2's, and this builtin gets it from `lesh::args` like the
	// rest. Without it there would be no way to set a prompt whose first byte is
	// a hyphen, which is a perfectly ordinary thing to want.
	fake_prompt_console console;
	const console_guard guard{console};
	EXPECT_EQ(run_prompt({"prompt", "--", "-c"}).status, 0);
	EXPECT_EQ(console.calls, (std::vector<std::string>{"set(left,-c)"}));
}
