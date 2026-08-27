#pragma once

// LESHNICI'S VISIBLE LIST (#163, #165): the shipped extension set, and the two
// verbs that install it.
//
// `coreutils/` is `ls`, `cat`, `head` and `tail`, behind `set -o leshnici`.
// `prompt/` is `git`, installed on a prompt engine. Both halves arrive by being
// INSTALLED rather than compiled in - a shell or an engine built anywhere else,
// a test's, a tool's, is the bare one - and this header is the one thing
// `src/main.cpp` includes to reach either. Nothing below leshnici is visible
// through it: no `coreutils/builtins.h`, no `prompt/modules.h`, nothing but the
// two forward declarations these signatures need.

namespace lesh::runtime {
class shell_state;
} // namespace lesh::runtime

namespace lesh::ui::prompt {
class engine;
} // namespace lesh::ui::prompt

namespace lesh::leshnici {

// Hands `state` the extension table. Reports and installs nothing when a name
// collides with a core builtin - `shell_state::set_extension_builtins` writes
// the diagnostic, one line per collision, and the shell runs with the core set.
//
// Idempotent: the table is `constexpr` with static storage duration and the
// state holds a view of it, so calling this twice leaves one installation.
void install_builtins(runtime::shell_state& state);

// Registers every prompt module leshnici ships, on `which`, under the names a
// template spells them with. Idempotent, because `register_module` replaces
// (#101): calling it twice on one engine leaves one registration.
void install_prompt_modules(ui::prompt::engine& which);

} // namespace lesh::leshnici
