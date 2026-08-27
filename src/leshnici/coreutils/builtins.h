#pragma once

// LESHNICI'S COREUTILS BUILTINS (#165).
//
// `ls`, `cat`, `head` and `tail`: the four functions the shipped extension
// set's builtin table wires together. `leshnici.h` is the visible list and
// `install_builtins` the one verb that installs them - `src/main.cpp` includes
// that header, not this one - so nothing outside `coreutils/` needs to know
// that `ls` runs in-process, and nothing below leshnici can find out.
//
// WHY THESE EXIST AT ALL. A shell that draws its own prompt and edits its own
// line still forks `/bin/ls` to answer "what is in this directory", which costs a
// process and a `PATH` walk for a `getdents` and a `write`. The runtime cannot
// hold them - `kBuiltinRegistry` is POSIX's list and #35's `static_assert` is
// what makes it trustworthy - so they arrive through the extension table
// `runtime/builtins.h` declares, borrowed and gated.
//
// WHY IT IS A CALL AND NOT A TABLE ANYONE CAN REACH. Same answer as the prompt
// modules': a shell built by a test, a tool, or `lesh_tests` has the core set and
// only the core set, and which one you have is a fact about the call site rather
// than about the type. A shell that HAS the table still looks at it only under
// `set -o leshnici` - on by default when interactive, off in every script - so
// the POSIX command search a script sees is the one it saw before this file
// existed.
//
// THE FOUR ARE `ls`, `cat`, `head` AND `tail`, and their surface is deliberately
// small: `-a -l -1` for `ls`, no options for `cat`, `-n N` for `head` and `tail`.
// Anything wider is a later ticket rather than a guess made here.

#include "runtime/builtins.h"
#include "runtime/shell_state.h"

namespace lesh::leshnici::coreutils {

// `ls [-a] [-l] [-1] [file...]`. Operands are paths and default to `.`.
[[nodiscard]] runtime::builtin_result builtin_ls(runtime::shell_state& state, char** argv);

// `cat [file...]`. No options; `-` is standard input, and so is no operand.
[[nodiscard]] runtime::builtin_result builtin_cat(runtime::shell_state& state, char** argv);

// `head [-n count] [file...]`. Ten lines by default; a `==> name <==` header
// before each file when there is more than one operand, which is what POSIX
// specifies and what `tail` below does too.
[[nodiscard]] runtime::builtin_result builtin_head(runtime::shell_state& state, char** argv);

// `tail [-n count] [file...]`. The last ten lines by default.
[[nodiscard]] runtime::builtin_result builtin_tail(runtime::shell_state& state, char** argv);

} // namespace lesh::leshnici::coreutils
