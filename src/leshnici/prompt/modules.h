#pragma once

// LESHNICI'S PROMPT MODULES (#163).
//
// This is the visible list: what the shipped extension set adds to a prompt
// engine. `leshnici.h` is the single call that adds it - a wiring site includes
// that header, constructs its engine and calls `install_prompt_modules` on it;
// nothing else in the program needs to know that `git` exists, and nothing
// below leshnici can find out.
//
// WHY IT IS A CALL AND NOT A CONSTRUCTOR. The engine's constructor seeds the
// seven built-ins and by design knows nothing above itself (A-11's no-side-door
// rule: everything else arrives through `register_module`). An engine built by
// a test is therefore a bare one - `{git}` on it is an unknown module, refused
// at `set` with the same sentence any other unknown name gets - and an engine
// built by the shell is one `install_prompt_modules` away from the shipped set.
// Which one you have is a fact about the call site rather than about the type.

#include "leshnici/prompt/git.h"
