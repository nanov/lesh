#pragma once

#include "leshper/text.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lesh::leshper {

// One replacement of a buffer range, and everything undoing it needs.
//
// fish's `edit_t` (fish 3.7.1 `src/reader.cpp`) is the model the requirements
// cite by name, and the three ideas F-1 says to adopt are all here: the record
// carries the OLD text so undo does not have to re-derive it, it carries the
// cursor so F-4's "undo restores text AND cursor" is a property of the record
// rather than of the code that replays it, and records coalesce and group.
//
// Both texts are owned copies. They have to be: the buffer they were sliced
// from is about to be overwritten, and a view into it would dangle a line later.
struct edit_record {
	position at;              // where the replaced range begins
	std::string removed;      // what was there, restored on undo
	std::string inserted;     // what replaced it, restored on redo
	position cursor_before;   // where the cursor was before the edit
	position cursor_after;    // where it went after

	// A pure insertion of new text at the cursor - what typing produces, and the
	// only shape that coalesces.
	[[nodiscard]] bool is_plain_insertion() const noexcept {
		return removed.empty() && !inserted.empty();
	}

	[[nodiscard]] position end_after_insertion() const noexcept {
		return position::from_byte_offset(at.byte_offset() + inserted.size());
	}

	friend bool operator==(const edit_record& a, const edit_record& b) noexcept {
		return a.at == b.at && a.removed == b.removed && a.inserted == b.inserted
		    && a.cursor_before == b.cursor_before && a.cursor_after == b.cursor_after;
	}
};

// One step of undo: the records that go away together.
//
// A step is usually one record. It is more than one when an edit group is open
// (F-1's `begin_edit_group`), which is how a single user-visible operation that
// touches the buffer twice - accepting a completion that first deletes the
// stem, a bracketed paste that F-6 requires be ONE undo step, an action's
// write-back batch under #92's decision 3 - undoes as one.
struct undo_step {
	std::vector<edit_record> records;

	friend bool operator==(const undo_step& a, const undo_step& b) noexcept {
		return a.records == b.records;
	}
};

// Unlimited linear undo/redo (F-1), over the buffer it is handed.
//
// Linear, not a tree: F-1 says unlimited linear, and a new edit after an undo
// discards the redo tail. That is the behaviour every shell line editor has and
// the one users expect from a prompt.
//
// The history does not own the buffer or the cursor; it is handed both when
// asked to undo. State ownership stays with A-1's one struct, and the history
// stays testable on its own.
class undo_history {
public:
	// Records an edit that has ALREADY been applied to the buffer.
	//
	// Applying and recording are deliberately two calls in one direction: the
	// editor's apply_edit does both, and it is the only caller. Recording an
	// edit that was never applied would produce a history that undoes into a
	// buffer state that never existed, which is the failure mode worth designing
	// against.
	void record(edit_record edit) {
		_redoable.clear();
		if (_group_depth > 0) {
			_open.records.push_back(std::move(edit));
			return;
		}
		if (coalesce_into_last(edit))
			return;
		_steps.push_back(undo_step{{std::move(edit)}});
	}

	// Opens an edit group: every record until the matching end lands in one step.
	//
	// Nestable, because the callers that will want it nest. #92's atomic
	// write-back opens a group around a user action, and an action calling
	// another action (F-15) opens a second inside it; only the outermost close
	// commits the step.
	void begin_group() {
		if (_group_depth == 0)
			_open = undo_step{};
		++_group_depth;
	}

	void end_group() {
		if (_group_depth == 0)
			return;
		--_group_depth;
		if (_group_depth > 0)
			return;
		if (!_open.records.empty())
			_steps.push_back(std::move(_open));
		_open = undo_step{};
	}

	[[nodiscard]] bool group_open() const noexcept { return _group_depth > 0; }

	// True when the next plain insertion would extend the current step rather
	// than starting a new one. Exists so the coalescing rule can be asserted
	// directly (F-4) instead of inferred from a step count.
	[[nodiscard]] bool coalescing() const noexcept { return _coalescing; }

	// Breaks the typing run. Any action that is not a plain insertion calls this,
	// so that typing `ab`, moving the cursor, and typing `cd` is two undo steps
	// and not one - the cursor move happened in between and the user will expect
	// undo to stop there.
	void break_coalescing() noexcept { _coalescing = false; }

	[[nodiscard]] bool can_undo() const noexcept { return !_steps.empty(); }
	[[nodiscard]] bool can_redo() const noexcept { return !_redoable.empty(); }
	[[nodiscard]] size_t step_count() const noexcept { return _steps.size(); }

	// Undoes one step: restores the text and the cursor (F-4). Answers false when
	// there is nothing to undo, so the editor can leave the generation alone.
	bool undo(text_buffer& buffer, position& cursor) {
		if (_steps.empty())
			return false;
		undo_step step = std::move(_steps.back());
		_steps.pop_back();
		// Reverse order: the records of a group were applied front to back, and
		// each one's offsets were computed against the buffer the previous one
		// left behind.
		for (size_t i = step.records.size(); i-- > 0;) {
			const edit_record& edit = step.records[i];
			buffer.replace(edit.at, edit.end_after_insertion(), edit.removed);
			cursor = edit.cursor_before;
		}
		_redoable.push_back(std::move(step));
		_coalescing = false;
		return true;
	}

	bool redo(text_buffer& buffer, position& cursor) {
		if (_redoable.empty())
			return false;
		undo_step step = std::move(_redoable.back());
		_redoable.pop_back();
		for (const edit_record& edit : step.records) {
			buffer.replace(edit.at,
			               position::from_byte_offset(edit.at.byte_offset() + edit.removed.size()),
			               edit.inserted);
			cursor = edit.cursor_after;
		}
		_steps.push_back(std::move(step));
		_coalescing = false;
		return true;
	}

	friend bool operator==(const undo_history& a, const undo_history& b) noexcept {
		return a._steps == b._steps && a._redoable == b._redoable
		    && a._group_depth == b._group_depth && a._open == b._open
		    && a._coalescing == b._coalescing;
	}

private:
	// Coalescing, F-4's "runs of plain typing collapse into one step".
	//
	// The run continues only while every condition holds: this edit is a plain
	// insertion, the last step ended in one, and this one begins exactly where
	// that one ended. Anything else - a deletion, a cursor move, an edit
	// somewhere else in the buffer - ends the run, which is why the editor calls
	// break_coalescing() on every non-inserting action.
	//
	// Deletions deliberately do NOT coalesce: fish coalesces single-character
	// insertions and nothing else, and a run of backspaces collapsing into one
	// step surprises people who expect undo to give the word back a piece at a
	// time.
	bool coalesce_into_last(const edit_record& edit) {
		if (!_coalescing || !edit.is_plain_insertion() || _steps.empty()) {
			_coalescing = edit.is_plain_insertion();
			return false;
		}
		undo_step& last = _steps.back();
		if (last.records.empty()) {
			_coalescing = true;
			return false;
		}
		edit_record& previous = last.records.back();
		if (!previous.is_plain_insertion() || previous.end_after_insertion() != edit.at) {
			_coalescing = true;
			return false;
		}
		previous.inserted += edit.inserted;
		previous.cursor_after = edit.cursor_after;
		return true;
	}

	std::vector<undo_step> _steps;
	std::vector<undo_step> _redoable;
	undo_step _open;
	int _group_depth = 0;
	bool _coalescing = false;
};

} // namespace lesh::leshper
