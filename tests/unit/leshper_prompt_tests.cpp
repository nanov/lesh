#include "leshper/git_head.h"

#include "temp_path.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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

using lesh::leshper::prompt::git_head;
using lesh::leshper::prompt::git_probe_options;
using lesh::leshper::prompt::read_git_head;

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
class LeshperPromptGit : public ::testing::Test {
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

TEST_F(LeshperPromptGit, SymbolicHeadResolvesThroughLooseRef) {
	const std::string repo = make_repo("plain", "ref: refs/heads/main\n");
	write_text(repo + "/.git/refs/heads/main", sha40('a') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "main");
	EXPECT_EQ(head.short_sha, "aaaaaaa");
}

TEST_F(LeshperPromptGit, BranchNameKeepsSlashesBelowRefsHeads) {
	const std::string repo = make_repo("slashy", "ref: refs/heads/feature/deep/name\n");
	write_text(repo + "/.git/refs/heads/feature/deep/name", sha40('b') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "feature/deep/name");
	EXPECT_EQ(head.short_sha, "bbbbbbb");
}

// Mid-rebase, HEAD names a ref outside `refs/heads/`. Reported whole, because
// shortening it would print something that looks like an ordinary branch.
TEST_F(LeshperPromptGit, RefnameOutsideRefsHeadsIsReportedWhole) {
	const std::string repo = make_repo("rebasing", "ref: refs/rebase-merge/onto\n");
	write_text(repo + "/.git/refs/rebase-merge/onto", sha40('c') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "refs/rebase-merge/onto");
	EXPECT_EQ(head.short_sha, "ccccccc");
}

// --- the fast path: packed refs -------------------------------------------

TEST_F(LeshperPromptGit, PackedRefsMatchExactlyAndSkipHeaderAndPeeledLines) {
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
TEST_F(LeshperPromptGit, LooseRefWinsOverAStalePackedLine) {
	const std::string repo = make_repo("both", "ref: refs/heads/main\n");
	write_text(repo + "/.git/refs/heads/main", sha40('e') + "\n");
	write_text(repo + "/.git/packed-refs", sha40('f') + " refs/heads/main\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.short_sha, "eeeeeee");
}

// A `packed-refs` with no trailing newline on its last line is still a file
// with that ref in it.
TEST_F(LeshperPromptGit, PackedRefsFinalLineNeedsNoNewline) {
	const std::string repo = make_repo("nonl", "ref: refs/heads/last\n");
	write_text(repo + "/.git/packed-refs",
		"# pack-refs with: peeled\n" + sha40('7') + " refs/heads/last");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "last");
	EXPECT_EQ(head.short_sha, "7777777");
}

// --- the fast path: unborn and detached ------------------------------------

TEST_F(LeshperPromptGit, UnbornBranchIsFoundWithNoSha) {
	// `git init` and nothing else: HEAD names a branch, and no ref exists.
	const std::string repo = make_repo("fresh", "ref: refs/heads/main\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_FALSE(head.detached);
	EXPECT_EQ(head.branch, "main");
	EXPECT_TRUE(head.short_sha.empty());
}

TEST_F(LeshperPromptGit, DetachedHeadIsTheObjectNameItself) {
	const std::string repo = make_repo("detached", sha40('d') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_TRUE(head.detached);
	EXPECT_TRUE(head.branch.empty());
	EXPECT_EQ(head.short_sha, "ddddddd");
}

TEST_F(LeshperPromptGit, DetachedHeadAcceptsASha256ObjectName) {
	const std::string repo = make_repo("sha256", std::string(64, '9') + "\n");

	const git_head head = read_git_head(repo);
	EXPECT_TRUE(head.found);
	EXPECT_TRUE(head.detached);
	EXPECT_EQ(head.short_sha, "9999999");
}

// --- discovery -------------------------------------------------------------

TEST_F(LeshperPromptGit, DiscoveryClimbsOutOfANestedDirectory) {
	const std::string repo = make_repo("nested", "ref: refs/heads/trunk\n");
	write_text(repo + "/.git/refs/heads/trunk", sha40('8') + "\n");
	const std::string deep = repo + "/src/leshper/prompt";
	make_dirs(deep);

	const git_head head = read_git_head(deep);
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "trunk");
	EXPECT_EQ(head.short_sha, "8888888");
}

TEST_F(LeshperPromptGit, TrailingSlashesOnTheDirectoryDoNotMatter) {
	const std::string repo = make_repo("slashes", "ref: refs/heads/main\n");

	const git_head head = read_git_head(repo + "///");
	EXPECT_TRUE(head.found);
	EXPECT_EQ(head.branch, "main");
}

// The walk runs to the filesystem root and finds nothing. Not a hand-off to
// the fallback: there is nothing here for `git` to tell us either.
TEST_F(LeshperPromptGit, OutsideAnyRepositoryTheAnswerIsNotFound) {
	const std::string plain = at("no_repo_here/deeper");
	make_dirs(plain);

	const git_head head = read_git_head(plain);
	EXPECT_FALSE(head.found);
	EXPECT_TRUE(head.branch.empty());
	EXPECT_TRUE(head.short_sha.empty());
}

// --- gitfile indirection ---------------------------------------------------

TEST_F(LeshperPromptGit, GitfileWithARelativeGitdirIsFollowed) {
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

TEST_F(LeshperPromptGit, GitfileWithAnAbsoluteGitdirIsFollowed) {
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
TEST_F(LeshperPromptGit, MalformedGitfileIsUnrecognizedRatherThanGuessed) {
	const std::string root = at("bad_gitfile");
	make_dirs(root);
	write_text(root + "/.git", "this is not a gitfile\n");

	git_probe_options options;
	options.allow_spawn = false;
	EXPECT_FALSE(read_git_head(root, options).found);
}

// --- the linked-worktree shape ---------------------------------------------

TEST_F(LeshperPromptGit, LinkedWorktreeReadsHeadLocallyAndRefsFromTheCommonDir) {
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

TEST_F(LeshperPromptGit, ReftableWithoutSpawnIsNotFound) {
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

TEST_F(LeshperPromptGit, UnreadableHeadWithoutSpawnIsNotFound) {
	const std::string repo = make_repo("garbage_head", "who knows what this is\n");

	git_probe_options options;
	options.allow_spawn = false;
	EXPECT_FALSE(read_git_head(repo, options).found);
}

// Short hex is not an object name. `1234567` in HEAD is a file we do not
// understand, and reading it as a detached head would be a plausible lie.
TEST_F(LeshperPromptGit, ShortHexInHeadIsNotADetachedHead) {
	const std::string repo = make_repo("shorthex", "1234567\n");

	git_probe_options options;
	options.allow_spawn = false;
	EXPECT_FALSE(read_git_head(repo, options).found);
}

// --- the spawn fallback ----------------------------------------------------

TEST_F(LeshperPromptGit, FallbackAsksGitAndReportsItsBranch) {
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

TEST_F(LeshperPromptGit, FallbackTurnsEmptyOutputIntoASecondQuestion) {
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
TEST_F(LeshperPromptGit, FallbackWithoutTheShaIsNotFoundRatherThanHalfDetached) {
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

TEST_F(LeshperPromptGit, AMissingGitCommandIsNotFoundAndNotACrash) {
	const std::string repo = make_repo("no_git", "ref: refs/heads/main\n");
	make_dirs(repo + "/.git/reftable");
	const std::string absent = at("definitely_not_here/git");

	git_probe_options options;
	options.git_command = absent.c_str();
	options.budget_ms = kSpawnBudgetMs;
	EXPECT_FALSE(read_git_head(repo, options).found);
}

// --- the budget ------------------------------------------------------------

TEST_F(LeshperPromptGit, ZeroBudgetAnswersWithoutTouchingTheFilesystem) {
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
TEST_F(LeshperPromptGit, AHungFallbackIsKilledAndTheProbeReturns) {
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

} // namespace
