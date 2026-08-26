#pragma once

// THE HOST, as the editor sees it (#168 Phase B).
//
// ONE DOOR WHERE THERE WERE THREE. Before this, leshper declared `completer`,
// `shell_knowledge` and `history_source` - three abstract interfaces, three
// borrowed pointers hung off the registry and the request token, each with its
// own null-means-nothing rule. All three are `lesh::ui` types now
// (`ui/completion.h`, `ui/shell_knowledge.h`, `ui/history_search.h`), and two of
// them never cross into the editor at all: the history is walked by a reactor
// that is the host's own, and the searcher goes with it.
//
// All three were the same idea said three times: the editor asking the
// side that knows the shell a question it cannot answer itself. So there is one
// interface now, `registry::host` is the one field, and the shapes the questions
// and answers travel in are the effect and event types the rest of the boundary
// already uses.
//
// WHY THE QUESTIONS ARE EFFECT VALUES AND THE ANSWERS ARE EVENT VALUES, on a
// door that is called and returns. Because that is the vocabulary, and a second
// vocabulary for the synchronous half would mean the day one of these moves to a
// worker is the day its callers are rewritten. `want_completion` carries a
// generation for the same reason `worker_request` does, and
// `completion_candidates` is droppable on it, even though today the round trip
// is inside one turn and the generation provably cannot have moved.
//
// TWO THREADS, AND THEY ASK DIFFERENT QUESTIONS. `classify_command` is asked
// from a WORKER - it is `lesh_request_command_kind`'s answer and the highlighter
// is the caller (F-22 put the `$PATH` sweep off the keystroke path on purpose).
// `carry_out` is asked from the LOOP, inside an action, because `lesh_complete`
// answers a count the same call reads back. An implementation therefore has to
// make the first re-entrant and read-only, and may keep the second's scratch in
// a plain member. ADR-0009 is what makes reading shell tables from either
// legal: the shell is the sole writer of its own state and nothing executes
// while the editor turns.
//
// NULL IS NOT AN ERROR. A leshper with no host attached is a leshper embedded in
// something that is not this shell: names classify as unknown and Tab finds
// nothing, which is the ordinary nothing rather than a diagnostic.

#include "leshper/effect.h"
#include "leshper/event.h"

#include <cstdint>
#include <string_view>

namespace lesh::leshper {

class host {
public:
	host() = default;
	virtual ~host() = default;

	host(const host&) = delete;
	host& operator=(const host&) = delete;

	// What a command name IS, as one of abi.h's `LESH_COMMAND_*` values.
	//
	// A `std::uint32_t` and not an enum, because the enum is the HOST's
	// (`ui::command_kind`) and the ABI's constants are what leshper knows the
	// space by. `LESH_COMMAND_UNKNOWN` is the answer for a name no table and no
	// `$PATH` directory holds, and it is also the answer this interface gives
	// when it has nothing to look in - a caller that ignores a status reads the
	// harmless answer rather than a confident wrong one.
	//
	// THE `$PATH` WALK IS BEHIND THIS, which is the change #168 Phase B made.
	// leshper used to do the sweep itself over a `$PATH` the shell lent it: a
	// `stat` per directory, in the editor, off a borrowed `string_view`. The
	// sweep is filesystem knowledge and it is the host's now, whole. What stayed
	// on leshper's side is the MEMO (`command_kind_memo`), because it is a cost
	// cache with the life of one request and it can never change an answer.
	//
	// Const, and asked from a worker: see the header.
	[[nodiscard]] virtual std::uint32_t classify_command(std::string_view name) const = 0;

	// Perform `what` NOW and fill `answer`.
	//
	// False when the host has no completer wired up, which `lesh_complete`
	// reports as LESH_ERR_NOTFOUND and `complete_word` treats as the ordinary
	// nothing Tab on an unmatched prefix is. `answer` is untouched on false.
	//
	// THE ANSWER'S STORAGE IS THE HOST'S and outlives the call: see
	// `completion_candidates` for the lifetime rule (valid until the next
	// `want_completion` this host carries out).
	[[nodiscard]] virtual bool carry_out(const want_completion& what,
	                                     completion_candidates& answer) = 0;
};

} // namespace lesh::leshper
