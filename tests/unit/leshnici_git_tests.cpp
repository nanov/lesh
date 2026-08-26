// leshnici's `git`: the budgeted HEAD reader and the prompt module over it
// (#157, #163, spec §6.10).
//
// WHY THIS FILE EXISTS APART FROM `leshper_prompt_tests.cpp`. `git` is not a
// built-in module. It is the first resident of `src/leshnici/`, the shipped
// extension set, which sits ABOVE leshper and is installed on an engine by the
// wiring site; the engine's own suite is about an engine that has never heard
// of it. So every case here builds an engine and hands it the set first, and
// that call is the thing under test as much as the branch name is - a template
// naming `git` on an engine without it is refused as an unknown module, which
// is asserted next door.
//
// WHAT IS NOT HERE: the composer's omission rules and the default table's bytes,
// which the COMPILER asserts in `prompt.h`'s `selftest` namespace against
// synthetic facts. What is left for a running test is what a constant expression
// cannot see - a real `.git` on a real disk, a spawn, and a deadline.

#include "leshnici/git_head.h"
#include "leshnici/module_git.h"
#include "leshnici/prompt_modules.h"
#include "ui/prompt/prompt.h"
#include "temp_path.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// --- the git HEAD reader (#157) ---
// ---------------------------------------------------------------------------
//
// §6.10's budgeted `git` module reads `.git/HEAD` by hand and falls back to
// `git` itself when it meets a layout it does not speak. Both halves are tested
// against the REAL filesystem, built by hand, because there is nothing here
// worth faking: the whole content of the fast path is which file is opened next
// and what its bytes mean, and a fake filesystem would be a restatement of this
// file's own assumptions rather than a check on them.
//
// The layouts are built rather than produced by running `git`. Running git
// would make the tests depend on the version installed - `git worktree`'s
// `commondir` and `pack-refs`'s header line have both changed spelling - and
// would test that git agrees with itself. Written by hand, each case says
// exactly which bytes the reader is being held to.
//
// The FALLBACK is exercised with a stub `git`, not the real one: the point
// under test is the spawn's plumbing - argv, the pipe, the exit status, the
// deadline and the kill - and a stub is the only way to drive the paths that
// matter (a command that prints nothing, and one that never returns). One
// stub answers BOTH argv forms, because the fallback is two commands and the
// detached path only exists past the first one's empty output.

namespace {

using lesh::leshnici::git_head;
using lesh::leshnici::git_probe_options;
using lesh::leshnici::read_git_head;
using lesh::ui::prompt::element_status;
using lesh::ui::prompt::engine;
using lesh::ui::prompt::surface_id;
namespace prompt = lesh::ui::prompt;

// The handful of doubles these cases share with the engine's own suite,
// restated here rather than exported: a test double is not an interface, and a
// header holding them would make one out of a few functions that exist to be
// read at their use site. `leshper_prompt_tests.cpp` keeps its own copies for
// the same reason.

// A session in `~/src`, no repo, last command succeeded.
prompt::state quiet() {
	prompt::state facts;
	facts.pwd = "/home/u/src";
	facts.home = "/home/u";
	return facts;
}

// One module, run on its own - through its OWN type-slot grammar. `parse` then
// `render`, which is exactly the pair a placement makes at set time and at
// render time.
struct rendered {
	std::string bytes;
	element_status status = element_status::omitted;
};

rendered run_module(const prompt::module& which, std::string_view type,
                    const prompt::state& facts) {
	prompt::params_blob params;
	prompt::parse_error why;
	EXPECT_TRUE(which.parse(type, params, why)) << type;

	prompt::sink out;
	const int answered = which.render(facts, params, out);
	return rendered{std::string{out.bytes()}, prompt::status_of(answered)};
}

// A template set on the left surface and rendered.
std::string set_and_render(engine& which, std::string_view text, const prompt::state& facts) {
	std::string error;
	EXPECT_TRUE(which.set_template(surface_id::left, text, error)) << text << ": " << error;
	which.render_full(facts);
	return std::string{which.output(surface_id::left)};
}

// A module whose bytes change every call, so a tick that re-invoked `git` would
// be visible beside one that did not. Counts its two verbs separately, which is
// what makes "parsed once, rendered per cause" a pair of numbers.
struct probe_params {
	prompt::fixed_text<32> tag{};
};

class test_module final : public prompt::typed_module<probe_params> {
public:
	test_module(std::string named, std::string label)
		: _named(std::move(named)), _label(std::move(label)) {}

	mutable int parses = 0;
	mutable int calls = 0;

	std::uint64_t wake = 0;

	[[nodiscard]] std::string_view name() const noexcept override { return _named; }

protected:
	bool parse(std::string_view type, probe_params& out, prompt::parse_error& err) const override {
		++parses;
		if (!out.tag.assign(type)) {
			err.what = ": tag is too long";
			return false;
		}
		return true;
	}

	int render(const prompt::state&, const probe_params& params,
	           prompt::sink& out) const override {
		++calls;
		if (wake != 0)
			out.wake_in(wake);
		out.append(_label);
		out.append(params.tag.view());
		out.append(std::to_string(calls));
		return prompt::code(element_status::ready);
	}

private:
	std::string _named;
	std::string _label;
};

// --- the module itself, before any filesystem ------------------------------

TEST(LeshniciGitModule, OmitsBeforeTouchingTheFilesystem) {
	prompt::state facts = quiet();
	EXPECT_FALSE(facts.fs_allowed);
	EXPECT_EQ(run_module(lesh::leshnici::kModuleGit, "", facts).status, element_status::omitted);

	// Allowed, but with nowhere to look.
	facts.fs_allowed = true;
	facts.pwd = std::string_view{};
	EXPECT_EQ(run_module(lesh::leshnici::kModuleGit, "", facts).status, element_status::omitted);
}

void write_text(const std::string& path, std::string_view text) {
	std::filesystem::create_directories(std::filesystem::path{path}.parent_path());
	std::ofstream out{path, std::ios::binary | std::ios::trunc};
	ASSERT_TRUE(out.good()) << path;
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
	out.close();
	ASSERT_TRUE(out.good()) << path;
}

void make_dirs(const std::string& path) {
	std::error_code ec;
	std::filesystem::create_directories(path, ec);
	ASSERT_FALSE(ec) << path << ": " << ec.message();
}

// A `git` that is two lines of `sh`, answering both forms the fallback sends.
//
// `branch_line` empty means `branch --show-current` prints nothing and exits 0,
// which is how real git says "detached". Every invocation appends its whole
// argv to `args_log`, so a test can assert that `-C <dir>` actually went where
// it was meant to - the fallback's correctness rests on that one flag.
void write_git_stub(const std::string& path, std::string_view branch_line,
                    std::string_view sha_line, int sha_status,
                    const std::string& args_log) {
	std::string script = "#!/bin/sh\n";
	script += "printf '%s\\n' \"$@\" >> '" + args_log + "'\n";
	script += "for a in \"$@\"; do\n";
	script += "\tif [ \"$a\" = \"--show-current\" ]; then\n";
	if (!branch_line.empty())
		script += "\t\tprintf '%s\\n' '" + std::string{branch_line} + "'\n";
	script += "\t\texit 0\n";
	script += "\tfi\n";
	script += "\tif [ \"$a\" = \"rev-parse\" ]; then\n";
	if (!sha_line.empty())
		script += "\t\tprintf '%s\\n' '" + std::string{sha_line} + "'\n";
	script += "\t\texit " + std::to_string(sha_status) + "\n";
	script += "\tfi\n";
	script += "done\n";
	script += "exit 1\n";
	write_text(path, script);
	ASSERT_EQ(::chmod(path.c_str(), 0755), 0) << path;
}

std::string read_text(const std::string& path) {
	std::ifstream in{path, std::ios::binary};
	return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Forty hex digits with `seed` repeated, so a test's expected short sha is
// readable at the call site instead of being a slice of a magic constant.
std::string sha40(char seed) { return std::string(40, seed); }

// No TearDown: `temp_path`'s destructor removes the tree, which reaches even a
// case that died on an ASSERT partway through building a layout.
class LeshniciGit : public ::testing::Test {
protected:
	[[nodiscard]] std::string at(std::string_view relative) const {
		return _tmp.file(relative);
	}

	// A plain repository at `<tmp>/<name>` with the given HEAD contents.
	[[nodiscard]] std::string make_repo(std::string_view name, std::string_view head) {
		const std::string root = at(name);
		make_dirs(root + "/.git/refs/heads");
		write_text(root + "/.git/HEAD", head);
		return root;
	}

	// EVERY CASE THAT ACTUALLY SPAWNS OVERRIDES THE 50 ms DEFAULT, and the
	// reason is the test process rather than the shell. Under ASan/UBSan/LSan a
	// `posix_spawn` of `/bin/sh` costs 40-60 ms here - the sanitized binary's
	// loader work, then a shell script on top of it - so the shipped budget
	// would SIGKILL the stub mid-flight and every fallback assertion would then
	// pass or fail by the machine's load rather than by the code. That is not a
	// claim about the default being wrong: it is measured against real `git` in
	// a release binary, and this constant is the same budget scaled to an
	// instrumented one. The deadline itself is under test in its own two cases.
	static constexpr std::uint32_t kSpawnBudgetMs = 5000;

	lesh::testing::temp_path _tmp;
};

// --- the fast path: symbolic HEAD -----------------------------------------

TEST_F(LeshniciGit, SymbolicHeadResolvesThroughLooseRef) {
	const std::string repo = make_repo("plain", "ref: refs/heads/main\n");
	write_text(repo + "/.git/refs/heads/main", sha40('a') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "main");
	EXPECT_EQ(head.short_sha, "aaaaaaa");
}

TEST_F(LeshniciGit, BranchNameKeepsSlashesBelowRefsHeads) {
	const std::string repo = make_repo("slashy", "ref: refs/heads/feature/deep/name\n");
	write_text(repo + "/.git/refs/heads/feature/deep/name", sha40('b') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "feature/deep/name");
	EXPECT_EQ(head.short_sha, "bbbbbbb");
}

// Mid-rebase, HEAD names a ref outside `refs/heads/`. Reported whole, because
// shortening it would print something that looks like an ordinary branch.
TEST_F(LeshniciGit, RefnameOutsideRefsHeadsIsReportedWhole) {
	const std::string repo = make_repo("rebasing", "ref: refs/rebase-merge/onto\n");
	write_text(repo + "/.git/refs/rebase-merge/onto", sha40('c') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "refs/rebase-merge/onto");
	EXPECT_EQ(head.short_sha, "ccccccc");
}

// --- the fast path: packed refs -------------------------------------------

TEST_F(LeshniciGit, PackedRefsMatchExactlyAndSkipHeaderAndPeeledLines) {
	const std::string repo = make_repo("packed", "ref: refs/heads/main\n");
	// No loose ref at all - this is a freshly `pack-refs`'d repository. The
	// neighbours are the point: a prefix match answers `main2`, a suffix match
	// answers `origin/main`, and a line-kind mistake answers the peeled sha.
	write_text(repo + "/.git/packed-refs",
		"# pack-refs with: peeled fully-peeled sorted \n"
		+ sha40('1') + " refs/heads/main2\n"
		+ sha40('2') + " refs/heads/mainline\n"
		+ sha40('3') + " refs/tags/v1.0\n"
		"^" + sha40('4') + "\n"
		+ sha40('5') + " refs/heads/main\n"
		+ sha40('6') + " refs/remotes/origin/main\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "main");
	EXPECT_EQ(head.short_sha, "5555555");
}

// The loose file is git's own resolution order, and it matters after a branch
// moves: `pack-refs` leaves the packed line behind and a new loose file wins.
TEST_F(LeshniciGit, LooseRefWinsOverAStalePackedLine) {
	const std::string repo = make_repo("both", "ref: refs/heads/main\n");
	write_text(repo + "/.git/refs/heads/main", sha40('e') + "\n");
	write_text(repo + "/.git/packed-refs", sha40('f') + " refs/heads/main\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.short_sha, "eeeeeee");
}

// A `packed-refs` with no trailing newline on its last line is still a file
// with that ref in it.
TEST_F(LeshniciGit, PackedRefsFinalLineNeedsNoNewline) {
	const std::string repo = make_repo("nonl", "ref: refs/heads/last\n");
	write_text(repo + "/.git/packed-refs",
		"# pack-refs with: peeled\n" + sha40('7') + " refs/heads/last");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "last");
	EXPECT_EQ(head.short_sha, "7777777");
}

// --- the fast path: unborn and detached ------------------------------------

TEST_F(LeshniciGit, UnbornBranchIsFoundWithNoSha) {
	// `git init` and nothing else: HEAD names a branch, and no ref exists.
	const std::string repo = make_repo("fresh", "ref: refs/heads/main\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "main");
	EXPECT_TRUE(head.short_sha.empty());
}

TEST_F(LeshniciGit, DetachedHeadIsTheObjectNameItself) {
	const std::string repo = make_repo("detached", sha40('d') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_TRUE(head.detached);
	EXPECT_TRUE(head.branch.empty());
	EXPECT_EQ(head.short_sha, "ddddddd");
}

TEST_F(LeshniciGit, DetachedHeadAcceptsASha256ObjectName) {
	const std::string repo = make_repo("sha256", std::string(64, '9') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_TRUE(head.detached);
	EXPECT_EQ(head.short_sha, "9999999");
}

// --- discovery -------------------------------------------------------------

TEST_F(LeshniciGit, DiscoveryClimbsOutOfANestedDirectory) {
	const std::string repo = make_repo("nested", "ref: refs/heads/trunk\n");
	write_text(repo + "/.git/refs/heads/trunk", sha40('8') + "\n");
	const std::string deep = repo + "/src/ui/prompt";
	make_dirs(deep);

	const git_head head = read_git_head(deep);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "trunk");
	EXPECT_EQ(head.short_sha, "8888888");
}

TEST_F(LeshniciGit, TrailingSlashesOnTheDirectoryDoNotMatter) {
	const std::string repo = make_repo("slashes", "ref: refs/heads/main\n");

	const git_head head = read_git_head(repo + "///");
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "main");
}

// The walk runs to the filesystem root and finds nothing. Not a hand-off to
// the fallback: there is nothing here for `git` to tell us either.
TEST_F(LeshniciGit, OutsideAnyRepositoryTheAnswerIsNotFound) {
	const std::string plain = at("no_repo_here/deeper");
	make_dirs(plain);

	const git_head head = read_git_head(plain);
	EXPECT_FALSE(head.found);
	EXPECT_TRUE(head.branch.empty());
	EXPECT_TRUE(head.short_sha.empty());
}

// --- gitfile indirection ---------------------------------------------------

TEST_F(LeshniciGit, GitfileWithARelativeGitdirIsFollowed) {
	// The submodule shape: the working tree's `.git` is a file pointing into
	// the superproject's `.git/modules/`.
	const std::string root = at("super");
	make_dirs(root + "/.git/modules/sub/refs/heads");
	write_text(root + "/.git/modules/sub/HEAD", "ref: refs/heads/feature\n");
	write_text(root + "/.git/modules/sub/refs/heads/feature", sha40('a') + "\n");
	make_dirs(root + "/sub");
	// Relative to the directory holding the `.git` FILE, not to the cwd.
	write_text(root + "/sub/.git", "gitdir: ../.git/modules/sub\n");

	const git_head head = read_git_head(root + "/sub");
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "feature");
	EXPECT_EQ(head.short_sha, "aaaaaaa");
}

TEST_F(LeshniciGit, GitfileWithAnAbsoluteGitdirIsFollowed) {
	const std::string root = at("super_abs");
	const std::string gitdir = root + "/.git/modules/sub";
	make_dirs(gitdir + "/refs/heads");
	write_text(gitdir + "/HEAD", "ref: refs/heads/other\n");
	write_text(gitdir + "/refs/heads/other", sha40('b') + "\n");
	make_dirs(root + "/sub");
	write_text(root + "/sub/.git", "gitdir: " + gitdir + "\n");

	const git_head head = read_git_head(root + "/sub");
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "other");
	EXPECT_EQ(head.short_sha, "bbbbbbb");
}

// A `.git` file that is not the one format a `.git` file has. Unrecognized, so
// with the spawn declined the answer is not-found rather than a guess.
TEST_F(LeshniciGit, MalformedGitfileIsUnrecognizedRatherThanGuessed) {
	const std::string root = at("bad_gitfile");
	make_dirs(root);
	write_text(root + "/.git", "this is not a gitfile\n");

	git_probe_options options;
	options.allow_spawn = false;
	EXPECT_FALSE(read_git_head(root, options).found);
}

// --- the linked-worktree shape ---------------------------------------------

TEST_F(LeshniciGit, LinkedWorktreeReadsHeadLocallyAndRefsFromTheCommonDir) {
	const std::string main_root = at("wt_main");
	const std::string common = main_root + "/.git";
	make_dirs(common + "/refs/heads");
	write_text(common + "/HEAD", "ref: refs/heads/main\n");
	// The refs live once, in the common dir, and they are packed there.
	write_text(common + "/packed-refs",
		"# pack-refs with: peeled fully-peeled sorted \n"
		+ sha40('1') + " refs/heads/main\n"
		+ sha40('2') + " refs/heads/side\n");

	const std::string private_dir = common + "/worktrees/side";
	make_dirs(private_dir);
	write_text(private_dir + "/HEAD", "ref: refs/heads/side\n");
	write_text(private_dir + "/commondir", "../..\n");

	const std::string linked = at("wt_side");
	make_dirs(linked);
	write_text(linked + "/.git", "gitdir: " + private_dir + "\n");

	// HEAD from the PRIVATE dir - the linked worktree is on `side` - and the
	// sha from the COMMON dir's packed-refs, where the ref actually is.
	const git_head side = read_git_head(linked);
	EXPECT_TRUE(side.found);
	EXPECT_FALSE(side.detached);
	EXPECT_EQ(side.branch, "side");
	EXPECT_EQ(side.short_sha, "2222222");

	// The main worktree is unmoved by any of it, which is the other half of
	// "HEAD is local": a commondir read backwards would report `main` above.
	const git_head main_head = read_git_head(main_root);
	EXPECT_TRUE(main_head.found);
	EXPECT_EQ(main_head.branch, "main");
	EXPECT_EQ(main_head.short_sha, "1111111");
}

// --- layouts this reader refuses to guess at -------------------------------

TEST_F(LeshniciGit, ReftableWithoutSpawnIsNotFound) {
	const std::string repo = make_repo("reftable_repo", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	// A ref that would resolve if the reader ignored the reftable dir. It must
	// not be reported: with a reftable store the loose file is not the truth.
	write_text(repo + "/.git/refs/heads/main", sha40('a') + "\n");

	git_probe_options options;
	options.allow_spawn = false;
	const git_head head = read_git_head(repo, options);
	EXPECT_FALSE(head.found);
	EXPECT_TRUE(head.branch.empty());
}

TEST_F(LeshniciGit, UnreadableHeadWithoutSpawnIsNotFound) {
	const std::string repo = make_repo("garbage_head", "who knows what this is\n");

	git_probe_options options;
	options.allow_spawn = false;
	EXPECT_FALSE(read_git_head(repo, options).found);
}

// Short hex is not an object name. `1234567` in HEAD is a file we do not
// understand, and reading it as a detached head would be a plausible lie.
TEST_F(LeshniciGit, ShortHexInHeadIsNotADetachedHead) {
	const std::string repo = make_repo("shorthex", "1234567\n");

	git_probe_options options;
	options.allow_spawn = false;
	EXPECT_FALSE(read_git_head(repo, options).found);
}

// --- the spawn fallback ----------------------------------------------------

TEST_F(LeshniciGit, FallbackAsksGitAndReportsItsBranch) {
	const std::string repo = make_repo("fallback_branch", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string stub = at("git_stub_branch.sh");
	const std::string log = at("git_stub_branch.args");
	write_git_stub(stub, "from-the-stub", "abc1234", 0, log);

	git_probe_options options;
	options.git_command = stub.c_str();
	options.budget_ms = kSpawnBudgetMs;
	const git_head head = read_git_head(repo, options);

	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "from-the-stub");
	EXPECT_TRUE(head.short_sha.empty());

	// The argv is load-bearing: without `-C <directory>` the stub - and the
	// real git - would answer about whatever directory the shell happens to be
	// in, which at prompt time is a different repository often enough.
	const std::string args = read_text(log);
	EXPECT_EQ(args, "-C\n" + repo + "\nbranch\n--show-current\n");
}

TEST_F(LeshniciGit, FallbackTurnsEmptyOutputIntoASecondQuestion) {
	const std::string repo = make_repo("fallback_detached", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string stub = at("git_stub_detached.sh");
	const std::string log = at("git_stub_detached.args");
	// Nothing from `branch --show-current`, which is how git says detached.
	write_git_stub(stub, "", "beefca7", 0, log);

	git_probe_options options;
	options.git_command = stub.c_str();
	options.budget_ms = kSpawnBudgetMs;
	const git_head head = read_git_head(repo, options);

	EXPECT_TRUE(head.found);
	EXPECT_TRUE(head.detached);
	EXPECT_TRUE(head.branch.empty());
	EXPECT_EQ(head.short_sha, "beefca7");

	// Both commands, in order, both with the directory.
	const std::string args = read_text(log);
	EXPECT_EQ(args, "-C\n" + repo + "\nbranch\n--show-current\n"
	                "-C\n" + repo + "\nrev-parse\n--short\nHEAD\n");
}

// Half an answer is worse than none: `found + detached` with no sha renders a
// segment's literals around nothing.
TEST_F(LeshniciGit, FallbackWithoutTheShaIsNotFoundRatherThanHalfDetached) {
	const std::string repo = make_repo("fallback_half", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string stub = at("git_stub_half.sh");
	const std::string log = at("git_stub_half.args");
	write_git_stub(stub, "", "", 128, log);

	git_probe_options options;
	options.git_command = stub.c_str();
	options.budget_ms = kSpawnBudgetMs;
	const git_head head = read_git_head(repo, options);
	EXPECT_FALSE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_TRUE(head.short_sha.empty());
}

TEST_F(LeshniciGit, AMissingGitCommandIsNotFoundAndNotACrash) {
	const std::string repo = make_repo("no_git", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string absent = at("definitely_not_here/git");

	git_probe_options options;
	options.git_command = absent.c_str();
	options.budget_ms = kSpawnBudgetMs;
	EXPECT_FALSE(read_git_head(repo, options).found);
}

// --- the budget ------------------------------------------------------------

TEST_F(LeshniciGit, ZeroBudgetAnswersWithoutTouchingTheFilesystem) {
	const std::string repo = make_repo("budget_zero", "ref: refs/heads/main\n");
	write_text(repo + "/.git/refs/heads/main", sha40('a') + "\n");

	git_probe_options options;
	options.budget_ms = 0;

	const auto started = std::chrono::steady_clock::now();
	const git_head head = read_git_head(repo, options);
	const auto elapsed = std::chrono::steady_clock::now() - started;

	// A repository that resolves perfectly well with any budget at all.
	EXPECT_TRUE(read_git_head(repo).found);
	EXPECT_FALSE(head.found);
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 200);
}

// The one that would hang a prompt. The stub sleeps far past the budget; the
// probe must give up, SIGKILL the child, reap it, and return. The sanitized
// build is half the assertion here - an unreaped child's pipe and buffers show
// up as a leak - and the elapsed bound is the other half.
TEST_F(LeshniciGit, AHungFallbackIsKilledAndTheProbeReturns) {
	const std::string repo = make_repo("budget_hang", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string stub = at("git_stub_hang.sh");
	write_text(stub, "#!/bin/sh\nsleep 5\necho too-late\n");
	ASSERT_EQ(::chmod(stub.c_str(), 0755), 0);

	git_probe_options options;
	options.git_command = stub.c_str();
	// Comfortably past the spawn cost above, nowhere near the stub's five
	// seconds: what is being timed is the deadline, not the loader.
	options.budget_ms = 500;

	const auto started = std::chrono::steady_clock::now();
	const git_head head = read_git_head(repo, options);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count();

	EXPECT_FALSE(head.found);
	// Generous: the bound that matters is "well under the stub's five seconds",
	// and a loaded machine under three sanitizers is not a stopwatch.
	EXPECT_LT(elapsed, 1500) << "the probe waited " << elapsed << "ms";
}

// ---------------------------------------------------------------------------
// --- git through the composer, and the gate it guards (#157) ---
// ---------------------------------------------------------------------------
//
// The reader has its own cases above; these two are about what the COMPOSER
// does with it, which is a different question and the one §6.10's performance
// floor rests on.

TEST_F(LeshniciGit, APlacedGitSegRendersTheBranchItIsStandingIn) {
	const std::string repo = make_repo("composed", "ref: refs/heads/topic\n");
	write_text(repo + "/.git/refs/heads/topic", sha40('d') + "\n");

	// A `git` seg, PLACED, against a real repository: the group votes ready, brings
	// its literal with it, and the branch appears.
	//
	// PLACED RATHER THAN SHIPPED, since the owner's ruling on #157 took `git` out
	// of the default table - the default is a path and an arrow now, and a user who
	// wants a branch asks for one. What is asserted is unchanged and so is its
	// strength: the same reader, the same real `.git`, the same composer, and the
	// group here is the runtime form of the `seg` the table used to hold, which
	// means this now exercises the ABI's two-phase group as well.
	engine which;
	lesh::leshnici::install_prompt_modules(which);
	which.clear(surface_id::left);
	ASSERT_TRUE(which.open_group(surface_id::left));
	which.add_literal(surface_id::left, " on ");
	ASSERT_TRUE(which.add_module(surface_id::left, "git", ""));
	ASSERT_TRUE(which.close_group(surface_id::left));

	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.home = std::string_view{};
	// The one fact that lets a budgeted module touch the disk at all.
	facts.fs_allowed = true;
	which.render_full(facts);

	const std::string_view left = which.output(surface_id::left);
	EXPECT_NE(left.find(" on topic"), std::string_view::npos) << left;

	// And the same placement outside a repository says nothing at all about git -
	// the seg's literal vanishing with the module, which the compile-time selftests
	// check against synthetic facts and this checks against a real filesystem.
	const std::string bare = at("not_a_repo");
	make_dirs(bare);
	prompt::state elsewhere = facts;
	elsewhere.pwd = bare;
	which.render_full(elsewhere);
	EXPECT_EQ(which.output(surface_id::left).find(" on "), std::string_view::npos)
		<< which.output(surface_id::left);
}

// THE GATE #157 NAMES, and the one that would be expensive to get wrong: a
// spinner ticking beside `git` must not drag `git` along with it. §6.10 is
// explicit - "a tick that animates a spinner re-invokes the spinner alone and
// `git`'s slot is memcpy'd" - and the cost of the defect is a `.git/HEAD` read,
// possibly on an NFS mount, ten times a second.
//
// The assertion is made against the FILESYSTEM rather than against a call
// counter, because a counter would only prove the engine did not call the
// function this test registered. Moving the branch on disk under the running
// prompt proves the bytes came from the slot: had the tick re-read anything, it
// would have read `two`.
TEST_F(LeshniciGit, ATickSplicesGitsSlotAndOnlyANewPromptRereadsIt) {
	const std::string repo = make_repo("ticking", "ref: refs/heads/one\n");
	write_text(repo + "/.git/refs/heads/one", sha40('e') + "\n");
	write_text(repo + "/.git/refs/heads/two", sha40('f') + "\n");

	test_module spinner{"spinner", "|"};
	spinner.wake = 3;

	engine which;
	lesh::leshnici::install_prompt_modules(which);
	which.register_module("spinner", &spinner);
	which.clear(surface_id::left);
	which.add_module(surface_id::left, "git", "");
	which.add_literal(surface_id::left, " ");
	which.add_module(surface_id::left, "spinner", "");

	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.fs_allowed = true;
	facts.tick = 0;
	which.render_full(facts);
	EXPECT_EQ(which.output(surface_id::left), "one |1");
	EXPECT_EQ(which.next_wake(), 3u);

	// The branch moves, under a prompt that is already on screen.
	write_text(repo + "/.git/HEAD", "ref: refs/heads/two\n");

	facts.tick = 3;
	EXPECT_TRUE(which.render_tick(facts));
	// The spinner advanced; `git` did not. Its slot was spliced whole.
	EXPECT_EQ(which.output(surface_id::left), "one |2");

	// A NEW PROMPT is the cause that re-reads it - the only one that does
	// (§6.10's three reasons, and `git` answers exactly one of them).
	facts.tick = 4;
	which.render_full(facts);
	EXPECT_EQ(which.output(surface_id::left), "two |3");
}

// --- groups, against a real repository -------------------------------------

TEST_F(LeshniciGit, ATemplatesGroupVanishesWithItsModule) {
	const std::string repo = make_repo("templated", "ref: refs/heads/topic\n");
	write_text(repo + "/.git/refs/heads/topic", sha40('a') + "\n");
	const std::string bare = at("templated_not_a_repo");
	make_dirs(bare);

	engine which;
	lesh::leshnici::install_prompt_modules(which);
	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.home = std::string_view{};
	facts.fs_allowed = true;

	prompt::state elsewhere = facts;
	elsewhere.pwd = bare;

	// §6.10's own example, spelled in the language for the first time.
	EXPECT_EQ(set_and_render(which, "{path}( on {git})> ", facts), repo + " on topic> ");
	which.render_full(elsewhere);
	EXPECT_EQ(which.output(surface_id::left), bare + "> ");

	// The same thing said with slots instead of a group, which is what the
	// desugaring means: one styled span, the affixes inside it, the style
	// covering both.
	EXPECT_EQ(set_and_render(which, "{path}{git:magenta:: on :}> ", facts),
	          repo + "\x1b[35m on topic\x1b[0m> ");
	which.render_full(elsewhere);
	EXPECT_EQ(which.output(surface_id::left), bare + "> ");
}

TEST_F(LeshniciGit, GroupsNestAndTheInnerOneRendersOnlyIfTheOuterSurvived) {
	const std::string repo = make_repo("nested_template", "ref: refs/heads/deep\n");
	write_text(repo + "/.git/refs/heads/deep", sha40('b') + "\n");
	const std::string bare = at("nested_not_a_repo");
	make_dirs(bare);

	engine which;
	lesh::leshnici::install_prompt_modules(which);
	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.home = std::string_view{};
	facts.fs_allowed = true;
	facts.status = 3;

	// NESTING IS THE TEMPLATE LANGUAGE'S - the ABI's verb stream still refuses it,
	// having no way to say which group a close belongs to - and THE VOTE RECURSES
	// THROUGH IT. A child reports `ready` or `omitted`, and a nested group reports
	// exactly what a placement does; there is no third answer, because there is no
	// third kind of thing. So the outer group here shows when `git` speaks or when
	// the inner group does.
	EXPECT_EQ(set_and_render(which, "( on {git} ([{status}]))", facts), " on deep [3]");

	// Outside a repository `git` says nothing, and the inner group carries the
	// outer one on `$?` alone - the literal " on " and the space with it, because
	// they are decorations of a group that IS being shown.
	prompt::state elsewhere = facts;
	elsewhere.pwd = bare;
	which.render_full(elsewhere);
	EXPECT_EQ(which.output(surface_id::left), " on  [3]");

	// THE RULE THE MODEL CHANGE CORRECTED, and it is worth being explicit about
	// because the old engine answered the other way. A group used to be stamped
	// with a kind and only DIRECT module children were counted, so a nested group
	// could never vote - which made `(({git}))` a prompt that rendered nothing for
	// ever, whatever the repository, with nothing in the spelling to say so. Now
	// wrapping a placement in redundant parentheses changes nothing at all, which
	// is the only answer a reader can predict.
	EXPECT_EQ(set_and_render(which, "( on ({status}))", elsewhere), " on 3");
	EXPECT_EQ(set_and_render(which, "(({git}))", facts), "deep");
	EXPECT_EQ(set_and_render(which, "(({git}))", elsewhere), "");

	// And with nothing inside that can speak, the whole nest still vanishes: the
	// recursion carries an omission up as faithfully as it carries a readiness.
	EXPECT_EQ(set_and_render(which, "( on ({git} ({git})))", elsewhere), "");
}

TEST_F(LeshniciGit, PuttingAColourOnAPlacementDoesNotChangeHowItVotes) {
	// THE TRAP THE MANUAL SMOKE CAUGHT, and the reason a desugared placement is
	// stamped a module rather than a group: `( on {git})` works, so
	// `( on {git:magenta})` has to work, or adding one word to a prompt that was
	// fine would silently empty it - a group with nothing in it that can vote can
	// never be shown, and nothing about the spelling says so.
	const std::string repo = make_repo("coloured", "ref: refs/heads/tinted\n");
	write_text(repo + "/.git/refs/heads/tinted", sha40('c') + "\n");
	const std::string bare = at("coloured_not_a_repo");
	make_dirs(bare);

	engine which;
	lesh::leshnici::install_prompt_modules(which);
	prompt::state facts = quiet();
	facts.pwd = repo;
	facts.home = std::string_view{};
	facts.fs_allowed = true;

	EXPECT_EQ(set_and_render(which, "{path}( on {git})> ", facts), repo + " on tinted> ");
	EXPECT_EQ(set_and_render(which, "{path}( on {git:magenta})> ", facts),
	          repo + " on \x1b[35mtinted\x1b[0m> ");
	// With its affixes inside the group as well - a placement carrying a style and
	// two affixes is still one placement to the group around it.
	EXPECT_EQ(set_and_render(which, "{path}({git:magenta:: on :!})> ", facts),
	          repo + "\x1b[35m on tinted!\x1b[0m> ");

	// And all three vanish together outside a repository.
	prompt::state elsewhere = facts;
	elsewhere.pwd = bare;
	EXPECT_EQ(set_and_render(which, "{path}( on {git})> ", elsewhere), bare + "> ");
	EXPECT_EQ(set_and_render(which, "{path}( on {git:magenta})> ", elsewhere), bare + "> ");
	EXPECT_EQ(set_and_render(which, "{path}({git:magenta:: on :!})> ", elsewhere), bare + "> ");
}

} // namespace
