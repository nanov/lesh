#pragma once

// THE BUDGETED GIT HEAD READER (#157, spec §6.10): what branch is this, in a
// bounded number of syscalls, with `git` itself as the correctness fallback.
//
// §6.10 puts two rules on the prompt's floor and this file is where they meet.
// **Built-in modules never `exec`** - starship's 10-50 ms is `node --version`
// -class spawns, and a prompt that pays a process per draw has no floor at all -
// so the branch is read the way git wrote it: `.git/HEAD` is a file, a ref is a
// file, and the answer is three `open`s deep. And **anything touching the
// filesystem is budgeted**, because those three files may live on an NFS mount
// or `/mnt/c`, where an `open` is not a memory access and the prompt would
// otherwise wait for it. The budget converts a slow mount into an omitted
// segment rather than a stalled prompt: v1 has no patch-in (that is #156's), so
// a probe that runs out of time answers "no repo here" and the group holding the
// module renders as nothing.
//
// WHY THE FALLBACK EXISTS, AND WHY IT IS AN EXEC. Hand-rolling a ref lookup
// buys the floor and costs the guarantee that we understand every layout git
// can write. Reftable is already real; a future `.git` shape is certain. The
// resolution is not to guess: an unrecognized layout is answered by spawning
// `git branch --show-current`, which is by construction the right answer,
// slowly. **Wrong must be impossible; slow may be rare.** So every step below
// that cannot be read with confidence exits to the fallback rather than
// producing a plausible branch name - a prompt that says `main` when HEAD is
// somewhere else is worse than a prompt that says nothing, and much worse than
// one that took 8 ms once.
//
// That is also why the fallback is not "the same reader, more permissive". It
// is a different implementation - git's own - reached by a route that shares no
// code with the fast path, so a defect in the parsing here cannot also be a
// defect in its own fallback.
//
// WHAT THIS FILE IS NOT. It is not a module, and it holds no prompt machinery:
// no element, no status, no arena, no registry. §6.10's `git` module is a
// function of state into a sink and it will CALL this; keeping the two apart is
// what lets the reader be tested against real directories on disk while the
// composer is tested against fake modules. It also reaches for nothing above
// the substrate - no `shell_knowledge`, no runtime header - because the one
// shell fact it needs is a directory, which the module passes in.
//
// PURE, in the sense that matters for a thing running on the loop thread at
// prompt frequency: no global state, no `chdir`, no environment read, no cached
// answer between calls, and no exception out (an I/O error is an answer here,
// not a throw). Two threads may call it at once on different directories.

#include <cstdint>
#include <string>
#include <string_view>

namespace lesh::leshper::prompt {

// What HEAD says, in the vocabulary a prompt segment renders.
//
// `found` is the group's vote and the only field a caller must consult first:
// false means "render nothing", whatever the reason - not a repo, an
// unrecognized layout with the fallback declined or failed, or the budget spent.
// The distinction between those matters to a test and to nobody else, so it is
// deliberately not in the struct; a module that reported them differently would
// be putting an error message in a prompt.
//
// The three states a `found` answer can be in are readable without a flag for
// each: a branch (`branch` set), a detached HEAD (`detached`, `short_sha` set),
// and an unborn branch (`branch` set, `short_sha` empty - a fresh `git init`
// before its first commit, which is a repo and does have a branch).
struct git_head {
	bool found = false;      // inside a git repo, and the head was read
	bool detached = false;
	std::string branch;      // symbolic HEAD: name after refs/heads/, or the full refname when it is not under refs/heads/; empty when detached
	std::string short_sha;   // detached: always (first 7 hex); symbolic: when the ref resolves (loose or packed); empty for an unborn branch
};

// The knobs, all three of which exist for a reason a test can name.
struct git_probe_options {
	std::uint32_t budget_ms = 50;   // the whole probe's deadline, spawn included
	bool allow_spawn = true;        // false: unrecognized layout answers not-found instead of exec'ing
	const char* git_command = "git";  // what the fallback execs; a test points it at a stub
};

// Reads the head of the repository containing `directory`.
//
// `directory` is used two ways and they are not the same path: the walk for
// `.git` starts there, and the fallback execs `git -C <directory>`. Passing the
// shell's `$PWD` is the intended call. An ABSOLUTE path is expected - the walk
// climbs by trimming components off the string, so a relative path is searched
// only within itself and never above the process's working directory, because
// resolving it would mean reading `getcwd()` and this function reads no process
// state at all.
//
// Never throws. Never blocks longer than `options.budget_ms`, counted from
// entry across every `stat`, every read and both possible spawns. A
// `budget_ms` of 0 is a deadline already past: the answer is not-found and the
// filesystem is not touched.
[[nodiscard]] git_head read_git_head(std::string_view directory, const git_probe_options& options = {});

} // namespace lesh::leshper::prompt
