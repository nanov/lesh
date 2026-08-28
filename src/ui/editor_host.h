#pragma once

// THE HOST SIDE OF `leshper::host` (#168 Phase B).
//
// One object filling the one door the editor has (`leshper/host.h`), over the
// two things the editor used to reach for itself:
//
//   `shell_knowledge` - the shell's alias, function and builtin tables, plus
//   the value of `$PATH`. `classify_command` walks the tables, then the
//   filesystem, and answers one of abi.h's LESH_COMMAND_* numbers. The `$PATH`
//   sweep was `registry.cpp`'s until Phase B; it is here now, with the only
//   `stat` on the highlighting path and beside the tables it follows.
//
//   `completer` - #94's override point, `ui::shell_completer` by default and
//   whatever `provider_bundle::completion` names otherwise. `carry_out` runs
//   it and hands back a VIEW of the answer.
//
// WHO CALLS WHAT, ON WHICH THREAD. `classify_command` is asked from a WORKER:
// it is `lesh_request_command_kind`'s answer and the highlighter is the caller,
// which is the whole reason F-22 put highlighting off the keystroke path. It
// touches no member of this object that is not const, and the tables underneath
// are safe to read by ADR-0009 - the shell owns them and nothing executes while
// the editor turns. `carry_out` is asked from the LOOP, inside an action, and it
// is the one that writes `_answer`.
//
// THE ANSWER'S LIFETIME, stated once and relied on by `completion_candidates`:
// `_answer` is a member, reused across Tabs so its capacity survives, and the
// event handed back points into it. It is valid until the next `carry_out`.
// That is long enough because the only reader is the action that asked, which
// copies each candidate into its own scratch before feeding the pager
// (`offer_completion`, builtin_actions.cpp).

#include "leshper/event.h"
#include "leshper/host.h"
#include "ui/completion.h"
#include "ui/shell_knowledge.h"

#include <cstdint>
#include <string_view>

namespace lesh::ui {

class editor_host final : public leshper::host {
public:
	// Both BORROWED and both allowed to be null. A null `knowledge` is "no shell
	// attached": every name classifies as LESH_COMMAND_UNKNOWN. A null
	// `completion` is "no completer wired up": `lesh_complete` answers
	// LESH_ERR_NOTFOUND, which `complete_word` treats as the ordinary nothing
	// Tab on an unmatched prefix is.
	explicit editor_host(const shell_knowledge* knowledge = nullptr,
	                     const completer* completion = nullptr) noexcept
		: _knowledge(knowledge), _completion(completion) {}

	[[nodiscard]] std::uint32_t classify_command(std::string_view name) const override;

	[[nodiscard]] bool carry_out(const leshper::want_completion& what,
	                             leshper::completion_candidates& answer) override;

private:
	const shell_knowledge* _knowledge;
	const completer* _completion;
	// The address-stable storage the event above points into. Mutable in spirit
	// and in fact: `carry_out` is the only writer and it runs inside a turn.
	completion_result _answer;
};

// The whole resolution, tables then filesystem, as a free function so a test can
// assert on it without a host (#135's `classify_command_name`, moved out of
// `registry.cpp` by #168 Phase B). No memo and no validity checks - the ABI
// entry point owns both, and the memo stayed with it.
[[nodiscard]] command_kind classify_command_name(const shell_knowledge& shell,
                                                 std::string_view name) noexcept;

} // namespace lesh::ui
