#include "leshper/registry.h"

#include "leshper/editor.h"
#include "leshper/keymap.h"
#include "leshper/pager.h"
#include "substrate/assert.h"
#include "substrate/grapheme.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using lesh::leshper::command_kind;
using lesh::leshper::command_kind_memo;
using lesh::leshper::decoration_span;
using lesh::leshper::environment_knowledge;
using lesh::leshper::loop_outcome;
using lesh::leshper::position;
using lesh::leshper::proposal;
using lesh::leshper::shell_knowledge;
using lesh::leshper::virtual_text;

// The C numbers and the C++ enumerators are one space, and this is what keeps
// them one. A reordered enum would otherwise repaint every command name in the
// wrong colour and compile silently.
static_assert(static_cast<std::uint32_t>(command_kind::unknown) == LESH_COMMAND_UNKNOWN);
static_assert(static_cast<std::uint32_t>(command_kind::external) == LESH_COMMAND_EXTERNAL);
static_assert(static_cast<std::uint32_t>(command_kind::builtin) == LESH_COMMAND_BUILTIN);
static_assert(static_cast<std::uint32_t>(command_kind::function) == LESH_COMMAND_FUNCTION);
static_assert(static_cast<std::uint32_t>(command_kind::alias) == LESH_COMMAND_ALIAS);

// The completer speaks the pager's kinds and nothing of its own (#139), so the
// only agreement left to keep is #138's - already asserted beside the pager
// doors below.

// A thread's identity as a plain integer, so no header needs <thread> to hold
// one. Never zero, because zero means "no owner" on a dead handle.
std::uint64_t current_thread_key() noexcept {
	const std::uint64_t key =
		static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
	return key == 0 ? 1 : key;
}

// ---------------------------------------------------------------------------
// Handle validity (ADR-0008: valid for the receiving call, loop thread only).
// ---------------------------------------------------------------------------

bool editor_ok(const lesh_editor* handle) noexcept {
	return lesh::leshper::handle_is_live(handle);
}

bool request_ok(const lesh_request* token) noexcept {
	return lesh::leshper::token_is_live(token);
}

// Every accessor opens with this. In release it costs nothing; in debug it
// turns "stashed the handle" from a stale read into an abort at the call that
// did it.
#define LESH_EDITOR_HANDLE(handle)                                             \
	do {                                                                       \
		LESH_ASSERT(editor_ok(handle));                                        \
		if (!editor_ok(handle))                                                \
			return LESH_ERR_INVAL;                                             \
	} while (0)

#define LESH_REQUEST_HANDLE(token)                                             \
	do {                                                                       \
		LESH_ASSERT(request_ok(token));                                        \
		if (!request_ok(token))                                                \
			return LESH_ERR_INVAL;                                             \
	} while (0)

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// snake_case: a lowercase letter, then lowercase letters, digits, underscores.
//
// Narrow on purpose. The names are what a user types into a binding and what an
// rc file re-sources idempotently; a name space that admits hyphens as well
// would make `delete-backward-word` and `delete_backward_word` two actions that
// look like one, which is the failure worth designing out rather than
// documenting around.
bool is_snake_case(std::string_view name) noexcept {
	if (name.empty() || name[0] < 'a' || name[0] > 'z')
		return false;
	for (const char c : name) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Text geometry, over the staged bytes.
//
// The editor owns this so no binding has to. #108's segmenter is the authority
// on cluster boundaries; the line and word helpers are byte scans, because
// lines and blank-separated words are byte questions and asking Unicode about
// them would be answering a question nobody posed.
// ---------------------------------------------------------------------------

std::size_t clamp_into(std::string_view text, std::size_t offset) noexcept {
	return offset < text.size() ? offset : text.size();
}

bool on_cluster_boundary(std::string_view text, std::size_t offset) noexcept {
	if (offset == 0 || offset >= text.size())
		return true;
	return lesh::grapheme::next_boundary(text, lesh::grapheme::prev_boundary(text, offset))
	    == offset;
}

// Back to the start of the cluster the offset falls inside. Where a cursor
// lands: the cursor rests ON a cluster, never in the middle of one (F-3).
std::size_t snap_back(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	if (on_cluster_boundary(text, offset))
		return offset;
	return lesh::grapheme::prev_boundary(text, offset);
}

// Forward to the end of the cluster the offset falls inside. Where the far end
// of a write range lands, so that a replacement swallows whole clusters and
// cannot leave half of one behind.
std::size_t snap_forward(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	if (on_cluster_boundary(text, offset))
		return offset;
	return lesh::grapheme::next_boundary(text, lesh::grapheme::prev_boundary(text, offset));
}

constexpr bool is_blank(char byte) noexcept {
	return byte == ' ' || byte == '\t' || byte == '\n';
}

std::size_t line_start_of(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	while (offset > 0 && text[offset - 1] != '\n')
		--offset;
	return offset;
}

std::size_t line_end_of(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	while (offset < text.size() && text[offset] != '\n')
		++offset;
	return offset;
}

// Skip trailing blanks, then take the run of non-blanks. The same rule
// editor.cpp's `backward_word_start` uses, and the same placeholder: C-6 makes
// the lexer independently callable so this becomes token-wise, at which point
// both sites change to a call into the syntax layer.
std::size_t prev_word_of(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	while (offset > 0 && is_blank(text[offset - 1]))
		--offset;
	while (offset > 0 && !is_blank(text[offset - 1]))
		--offset;
	return offset;
}

std::size_t next_word_of(std::string_view text, std::size_t offset) noexcept {
	offset = clamp_into(text, offset);
	while (offset < text.size() && is_blank(text[offset]))
		++offset;
	while (offset < text.size() && !is_blank(text[offset]))
		++offset;
	return offset;
}

// --- Word geometry, two families (#119) -------------------------------------
//
// One implementation, a flag apart, because the only difference between vi's
// `w` and vi's `W` is whether punctuation is its own class. Writing them twice
// would be writing the boundary rule twice, and the boundary rule is the part
// that is easy to get subtly wrong.
//
// Everything >= 0x80 is an identifier byte. That is deliberately coarse: it
// keeps a run of non-ASCII text one word, keeps a class run from ever splitting
// a cluster (so the cluster stepping below can be trusted), and asks Unicode no
// question about word breaking that UAX #29's word rules would answer
// differently on a shell prompt than a user expects.

enum class char_class : std::uint8_t { blank, ident, punct };

char_class class_of(char byte, bool blank_separated) noexcept {
	const unsigned char c = static_cast<unsigned char>(byte);
	if (c == ' ' || c == '\t' || c == '\n')
		return char_class::blank;
	if (blank_separated)
		return char_class::ident;   // `W B E`: two classes, blank and not
	if (c >= 0x80)
		return char_class::ident;
	const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
	               || (c >= '0' && c <= '9') || c == '_';
	return word ? char_class::ident : char_class::punct;
}

// Where the NEXT word begins (vi's `w` / `W`).
std::size_t word_start_next(std::string_view text, std::size_t at, bool big) noexcept {
	const std::size_t size = text.size();
	std::size_t p = snap_back(text, at);
	if (p >= size)
		return size;
	const char_class here = class_of(text[p], big);
	if (here != char_class::blank) {
		while (p < size && class_of(text[p], big) == here)
			p = lesh::grapheme::next_boundary(text, p);
	}
	while (p < size && class_of(text[p], big) == char_class::blank)
		p = lesh::grapheme::next_boundary(text, p);
	return p;
}

// Where the word at or before `at` begins (vi's `b` / `B`).
std::size_t word_start_prev(std::string_view text, std::size_t at, bool big) noexcept {
	std::size_t p = snap_back(text, at);
	if (p == 0)
		return 0;
	p = lesh::grapheme::prev_boundary(text, p);
	while (p > 0 && class_of(text[p], big) == char_class::blank)
		p = lesh::grapheme::prev_boundary(text, p);
	if (class_of(text[p], big) == char_class::blank)
		return 0;
	const char_class here = class_of(text[p], big);
	for (;;) {
		if (p == 0)
			return 0;
		const std::size_t back = lesh::grapheme::prev_boundary(text, p);
		if (class_of(text[back], big) != here)
			return p;
		p = back;
	}
}

// The LAST cluster of the next word (vi's `e` / `E`), which is a place the
// cursor rests on rather than one past it - vi's `e` is inclusive and this is
// where that inclusiveness comes from.
std::size_t word_end_next(std::string_view text, std::size_t at, bool big) noexcept {
	const std::size_t size = text.size();
	std::size_t p = snap_back(text, at);
	if (p >= size)
		return size;
	p = lesh::grapheme::next_boundary(text, p);
	while (p < size && class_of(text[p], big) == char_class::blank)
		p = lesh::grapheme::next_boundary(text, p);
	if (p >= size)
		return size;
	const char_class here = class_of(text[p], big);
	while (p < size && class_of(text[p], big) == here)
		p = lesh::grapheme::next_boundary(text, p);
	return lesh::grapheme::prev_boundary(text, p);
}

// --- Line geometry the 2D buffer makes real (F-2) ---------------------------

std::size_t first_nonblank_of(std::string_view text, std::size_t at) noexcept {
	std::size_t p = line_start_of(text, at);
	const std::size_t end = line_end_of(text, at);
	while (p < end && (text[p] == ' ' || text[p] == '\t'))
		++p;
	return p;
}

// The same byte column on the line above / below, clamped to that line's end.
//
// BYTE column, not display column, and that is the honest v1: #123's width
// tables know what a column is on screen and the editor does not consult them
// here. A `j` through a line of CJK lands somewhere defensible rather than
// somewhere correct, and the fix is a call into the layout, which is a ticket
// and not a line.
std::size_t line_up_of(std::string_view text, std::size_t at) noexcept {
	const std::size_t start = line_start_of(text, at);
	if (start == 0)
		return at;   // no line above: vi does nothing rather than going to 0
	const std::size_t above_end = start - 1;
	const std::size_t above_start = line_start_of(text, above_end);
	const std::size_t column = at - start;
	const std::size_t target = above_start + column;
	return snap_back(text, target > above_end ? above_end : target);
}

std::size_t line_down_of(std::string_view text, std::size_t at) noexcept {
	const std::size_t end = line_end_of(text, at);
	if (end >= text.size())
		return at;
	const std::size_t below_start = end + 1;
	const std::size_t below_end = line_end_of(text, below_start);
	const std::size_t column = at - line_start_of(text, at);
	const std::size_t target = below_start + column;
	return snap_back(text, target > below_end ? below_end : target);
}

// --- The one delimiter-matching helper (#99 answer 2) -----------------------
//
// In neither paradigm's idiom on purpose: vi's `i(`/`a(` is its first caller and
// helix's `mi(` is its second, and the difference between them is which slice of
// the answer they take, not how the answer is found.

bool match_pair_in(std::string_view text, std::size_t at, char open, char close,
                   std::size_t& start_out, std::size_t& end_out) noexcept {
	const std::size_t size = text.size();
	if (size == 0)
		return false;
	if (at > size)
		at = size;

	if (open == close) {
		// A quote does not nest, so depth counting finds the wrong pair. Pair the
		// run in order along the LINE - which is also what stops an unbalanced
		// quote three lines up from swallowing everything after it.
		const std::size_t line_begin = line_start_of(text, at);
		const std::size_t line_finish = line_end_of(text, at);
		std::size_t opener = std::string_view::npos;
		for (std::size_t i = line_begin; i < line_finish; ++i) {
			if (text[i] != open)
				continue;
			if (opener == std::string_view::npos) {
				opener = i;
				continue;
			}
			if (at >= opener && at <= i) {
				start_out = opener;
				end_out = i + 1;
				return true;
			}
			opener = std::string_view::npos;
		}
		return false;
	}

	std::size_t opener = 0;
	if (at < size && text[at] == open) {
		opener = at;   // the cursor ON the opener is inside its own pair
	} else {
		std::size_t depth = 0;
		bool found = false;
		for (std::size_t i = at; i-- > 0;) {
			if (text[i] == close) {
				++depth;
			} else if (text[i] == open) {
				if (depth == 0) {
					opener = i;
					found = true;
					break;
				}
				--depth;
			}
		}
		if (!found)
			return false;
	}

	std::size_t depth = 0;
	for (std::size_t i = opener + 1; i < size; ++i) {
		if (text[i] == open) {
			++depth;
		} else if (text[i] == close) {
			if (depth == 0) {
				start_out = opener;
				end_out = i + 1;
				return true;
			}
			--depth;
		}
	}
	return false;   // an opener with no closer is not a pair
}

// ---------------------------------------------------------------------------
// Copy-out, the one shape every reading accessor has.
//
// `*length_out` is the full length whether or not it fit, so a caller that
// asked with a zero capacity learns what to allocate, and a caller that guessed
// too small learns by how much. LESH_ERR_TOOSMALL is not a failure to answer -
// it is the answer, with the bytes withheld.
// ---------------------------------------------------------------------------

std::int32_t copy_out(std::string_view source, char* out, std::size_t capacity,
                      std::size_t* length_out) noexcept {
	if (length_out == nullptr)
		return LESH_ERR_INVAL;
	*length_out = source.size();
	if (source.size() > capacity)
		return LESH_ERR_TOOSMALL;
	if (source.empty())
		return LESH_OK;
	if (out == nullptr)
		return LESH_ERR_INVAL;
	for (std::size_t i = 0; i < source.size(); ++i)
		out[i] = source[i];
	return LESH_OK;
}

// ---------------------------------------------------------------------------
// Classifying a command name (#135; spec §6.7, narrowed by ADR-0009).
//
// The tables are the shell's and come through `shell_knowledge`; the $PATH walk
// is HERE, on leshper's side of the link boundary, because it is the half that
// touches the filesystem and the half the token memoizes. `lesh_leshper` does
// not link `lesh_runtime`, so this file cannot look a name up in `shell_state`
// and does not try to.
// ---------------------------------------------------------------------------

// Longer than any PATH_MAX this runs on. A candidate that would not fit is
// DECLINED rather than truncated, because a truncated path names a different
// file - and answering about a different file is worse than not answering.
constexpr std::size_t kPathBytes = 4096;

// Moved here verbatim from builtin_reactors.cpp, where it was the highlighter's
// private getenv-based guess (#124).
//
// access(X_OK) alone says yes for a DIRECTORY, so `echo /tmp` would paint as a
// command. The mode test is what makes the answer mean "this is a thing exec
// would run".
bool is_executable_file(const char* path) noexcept {
	struct stat info;
	if (::stat(path, &info) != 0)
		return false;
	if (!S_ISREG(info.st_mode))
		return false;
	return ::access(path, X_OK) == 0;
}

// One stat per directory in `path`, in order, first hit wins - the search the
// shell itself would do (POSIX 2.9.1.1).
bool resolves_on_path(std::string_view path, std::string_view name) noexcept {
	char candidate[kPathBytes];
	std::string_view rest = path;
	for (;;) {
		const std::size_t colon = rest.find(':');
		std::string_view dir = colon == std::string_view::npos ? rest : rest.substr(0, colon);
		// POSIX: an empty PATH element means the current directory.
		if (dir.empty())
			dir = std::string_view{"."};
		if (dir.size() + name.size() + 2 <= sizeof(candidate)) {
			std::memcpy(candidate, dir.data(), dir.size());
			candidate[dir.size()] = '/';
			std::memcpy(candidate + dir.size() + 1, name.data(), name.size());
			candidate[dir.size() + 1 + name.size()] = '\0';
			if (is_executable_file(candidate))
				return true;
		}
		if (colon == std::string_view::npos)
			break;
		rest.remove_prefix(colon + 1);
	}
	return false;
}

// A name with a slash is a PATHNAME, not a lookup: POSIX 2.9.1.1 sends it
// straight to the filesystem, past every table. `./configure` is not shadowed by
// an alias called `./configure`, and could not be - no table can hold that name.
bool names_a_pathname(std::string_view name) noexcept {
	return name.find('/') != std::string_view::npos;
}

bool resolves_as_pathname(std::string_view name) noexcept {
	char candidate[kPathBytes];
	if (name.size() >= sizeof(candidate))
		return false;
	std::memcpy(candidate, name.data(), name.size());
	candidate[name.size()] = '\0';
	return is_executable_file(candidate);
}

// The whole resolution, tables then filesystem. No memo, no validity - the ABI
// entry point owns both.
command_kind classify_command_name(const shell_knowledge& shell,
                                   std::string_view name) noexcept {
	if (!names_a_pathname(name)) {
		const command_kind known = shell.classify(name);
		if (known != command_kind::unknown)
			return known;
	} else {
		return resolves_as_pathname(name) ? command_kind::external : command_kind::unknown;
	}
	std::string_view path;
	if (!shell.path(path))
		return command_kind::unknown;
	return resolves_on_path(path, name) ? command_kind::external : command_kind::unknown;
}

// The memo. Linear, because `capacity` is small and the thing it is racing is a
// sweep of the filesystem.
bool memo_find(const command_kind_memo& memo, std::string_view name,
               std::uint32_t& out) noexcept {
	if (name.size() > command_kind_memo::name_capacity)
		return false;
	for (std::uint32_t i = 0; i < memo.used; ++i) {
		const command_kind_memo::entry& one = memo.entries[i];
		if (one.length == name.size() && std::memcmp(one.name, name.data(), name.size()) == 0) {
			out = one.kind;
			return true;
		}
	}
	return false;
}

void memo_store(command_kind_memo& memo, std::string_view name, std::uint32_t kind) noexcept {
	if (name.size() > command_kind_memo::name_capacity || memo.used >= command_kind_memo::capacity)
		return;
	command_kind_memo::entry& one = memo.entries[memo.used];
	one.kind = kind;
	one.length = static_cast<std::uint16_t>(name.size());
	if (!name.empty())
		std::memcpy(one.name, name.data(), name.size());
	++memo.used;
}

// --- The pager (#138), the parts that are not the doors themselves --------

using lesh::leshper::pager_candidate;
using lesh::leshper::pager_decision;
using lesh::leshper::pager_grid;
using lesh::leshper::pager_kind;
using lesh::leshper::pager_state;

// The C numbers and the C++ enumerators are one space, and this is what keeps
// them one - the same guard `command_kind` gets above, for the same reason.
static_assert(static_cast<std::uint32_t>(pager_kind::plain) == LESH_PAGER_PLAIN);
static_assert(static_cast<std::uint32_t>(pager_kind::word) == LESH_PAGER_WORD);
static_assert(static_cast<std::uint32_t>(pager_kind::directory) == LESH_PAGER_DIRECTORY);
static_assert(static_cast<std::uint32_t>(pager_kind::executable) == LESH_PAGER_EXECUTABLE);
static_assert(static_cast<std::uint32_t>(pager_kind::symlink) == LESH_PAGER_SYMLINK);

// The keymap `commit` pushes and `close` pops, by the ONE name that is its
// identity (#117 decision 3). Reached through keymap.h rather than spelled again
// here: a name written in two places is a name that will be misspelled in one,
// and this is the pair where the misspelling would be a pager that opened
// without a keymap and could not be closed.
constexpr std::string_view pager_keymap_name = lesh::leshper::keymap_registry::pager;

bool kind_is_known(std::uint32_t kind) noexcept {
	return kind <= LESH_PAGER_SYMLINK;
}

// The grid the CURRENT terminal size implies. The row question a binding cannot
// answer for itself, which is why `lesh_pager_move` takes an axis rather than a
// candidate count.
//
// THE DEFAULT WIDTH POLICY, and the one place that is a compromise: #108's
// policy lives in the LOOP's options, not in `state`, so the ABI cannot reach
// the one the renderer will use. The two agree for every candidate that has no
// ambiguous-width character in it, which is every command name and every
// pathname on a POSIX system; where they could differ, the cost is a row step
// that moves by a column too many or too few, and the renderer still puts the
// selection on screen. Making them agree means putting the policy on `state`,
// which is a change to what a state IS and belongs with the ticket that gives
// the policy a configuration surface (#101).
pager_grid grid_of(const lesh_editor* editor) noexcept {
	const lesh::leshper::state& target = *editor->target;
	return lesh::leshper::measure_pager(target.pager, target.columns,
	                                    lesh::leshper::pager_row_budget(target.rows));
}

// The one place a candidate becomes buffer text. Staged, cursor after it.
void stage_insertion(lesh_editor* editor, std::string_view with) {
	const std::string_view text{editor->staged};
	const pager_state& pager = editor->target->pager;
	std::size_t begin = clamp_into(text, pager.replace_from.byte_offset());
	std::size_t end = clamp_into(text, pager.replace_to.byte_offset());
	if (end < begin)
		std::swap(begin, end);
	begin = snap_back(text, begin);
	end = snap_forward(text, end);
	editor->staged.replace(begin, end - begin, with.data(), with.size());
	editor->buffer_written = true;
	editor->staged_cursor = begin + with.size();
	editor->cursor_written = true;
}

// Drops the pager AND the layer it pushed. Popping only when the top layer is
// the pager's is what keeps a client that closed the pager from popping a mode
// somebody else had pushed above it.
void close_pager(lesh_editor* editor) {
	lesh::leshper::state& target = *editor->target;
	if (target.pager.open && !target.keymaps.layers.empty()
	    && target.keymaps.layers.back() == pager_keymap_name)
		target.keymaps.pop();
	target.pager.clear();
}
} // namespace

// ===========================================================================
// The ABI, implemented.
// ===========================================================================

extern "C" {

// --- Registration ----------------------------------------------------------

int32_t lesh_action_register(lesh_registry* registry, const char* name,
                             lesh_action_fn fn, void* userdata) {
	if (registry == nullptr || name == nullptr || fn == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view given{name};
	// A dot name is minted by the registry and by nobody else: it is the
	// original, and an original you can overwrite is not one.
	if (!given.empty() && given[0] == '.')
		return LESH_ERR_REFUSED;
	if (!is_snake_case(given))
		return LESH_ERR_INVAL;

	const lesh_registry::action_entry entry{fn, userdata};
	const auto found = registry->actions.find(given);
	if (found == registry->actions.end()) {
		// First definition: it is also the unshadowable original, forever.
		registry->actions.emplace(std::string{given}, entry);
		registry->actions.emplace("." + std::string{given}, entry);
		return LESH_OK;
	}
	found->second = entry;  // #101: re-sourcing an rc file replaces, idempotently
	return LESH_OK;
}

int32_t lesh_action_exists(lesh_registry* registry, const char* name, int32_t* out) {
	if (registry == nullptr || name == nullptr || out == nullptr)
		return LESH_ERR_INVAL;
	*out = registry->actions.find(std::string_view{name}) != registry->actions.end() ? 1 : 0;
	return LESH_OK;
}

int32_t lesh_action_invoke(lesh_editor* editor, const char* name,
                           const lesh_invocation* invocation) {
	LESH_EDITOR_HANDLE(editor);
	if (name == nullptr || invocation == nullptr || editor->registry == nullptr)
		return LESH_ERR_INVAL;
	// #92's ceiling. A wrapper that delegates to the thing it wrapped is one
	// frame; a wrapper that delegates to itself is the bug this catches.
	constexpr int ceiling = 64;
	if (editor->depth >= ceiling)
		return LESH_ERR_RECURSION;

	const auto found = editor->registry->actions.find(std::string_view{name});
	if (found == editor->registry->actions.end())
		return LESH_ERR_NOTFOUND;

	// The SAME handle, so the callee stages into the caller's staging area: a
	// wrapper delegating to `.accept_line` is one undo entry and one generation
	// bump, not two.
	++editor->depth;
	const std::int32_t status = found->second.fn(editor, invocation, found->second.userdata);
	--editor->depth;
	return status;
}

int32_t lesh_reactor_register(lesh_registry* registry, const char* name,
                              uint32_t event_mask, lesh_reactor_fn fn, void* userdata) {
	if (registry == nullptr || name == nullptr || fn == nullptr)
		return LESH_ERR_INVAL;
	constexpr std::uint32_t known = LESH_EVENT_BUFFER_CHANGED | LESH_EVENT_CURSOR_MOVED
	                              | LESH_EVENT_SELECTION_CHANGED;
	if (event_mask == 0 || (event_mask & ~known) != 0)
		return LESH_ERR_INVAL;
	const std::string_view given{name};
	if (!given.empty() && given[0] == '.')
		return LESH_ERR_REFUSED;
	if (!is_snake_case(given))
		return LESH_ERR_INVAL;

	registry->reactors[std::string{given}] =
		lesh_registry::reactor_entry{fn, userdata, event_mask};
	return LESH_OK;
}

int32_t lesh_reactor_exists(lesh_registry* registry, const char* name, int32_t* out) {
	if (registry == nullptr || name == nullptr || out == nullptr)
		return LESH_ERR_INVAL;
	*out = registry->reactors.find(std::string_view{name}) != registry->reactors.end() ? 1 : 0;
	return LESH_OK;
}

// --- Styles ----------------------------------------------------------------

int32_t lesh_style_intern(lesh_registry* registry, const char* name, uint32_t* out) {
	if (registry == nullptr || name == nullptr || out == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view given{name};
	if (given.empty())
		return LESH_ERR_INVAL;
	for (std::size_t i = 1; i < registry->styles.size(); ++i) {
		if (registry->styles[i] == given) {
			*out = static_cast<std::uint32_t>(i);
			return LESH_OK;
		}
	}
	registry->styles.emplace_back(given);
	*out = static_cast<std::uint32_t>(registry->styles.size() - 1);
	return LESH_OK;
}

int32_t lesh_style_name(lesh_registry* registry, uint32_t style_id, char* out,
                        size_t capacity, size_t* length_out) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	if (style_id == LESH_STYLE_NONE || style_id >= registry->styles.size())
		return LESH_ERR_NOTFOUND;
	return copy_out(registry->styles[style_id], out, capacity, length_out);
}

// --- Timers (#128 decision 3, #129's `timer` topic) -------------------------

int32_t lesh_timer_start(lesh_registry* registry, uint64_t interval_ms, const char* action,
                         uint64_t* id_out) {
	if (registry == nullptr || action == nullptr || id_out == nullptr)
		return LESH_ERR_INVAL;
	// A zero interval is a busy loop with a name, and there is no reading of
	// "every zero milliseconds" that a caller wants and cannot get from the
	// ordinary turn.
	if (interval_ms == 0)
		return LESH_ERR_INVAL;

	const std::string_view given{action};
	if (!is_snake_case(given))
		return LESH_ERR_INVAL;

	// The name is NOT resolved here. A timer armed before its action is
	// registered is legal, and re-registering the action replaces what the timer
	// runs - the same late-binding rule a key follows.
	//
	// AND THE SCHEDULE IS NOT KEPT HERE EITHER (#168). What leaves is one
	// `arm_timer` effect carrying the whole declaration; the host puts a due
	// instant beside it and owns the table from then on. All this side keeps is
	// the id, so that stopping the same timer twice can still be told apart from
	// stopping a live one.
	const uint64_t id = ++registry->next_timer_id;
	registry->armed_timers.push_back(id);
	registry->pending.push_back(lesh::leshper::arm_timer{
		id, interval_ms, lesh::leshper::intern_timer_action(*registry, given)});
	*id_out = id;
	return LESH_OK;
}

int32_t lesh_timer_stop(lesh_registry* registry, uint64_t id) {
	if (registry == nullptr)
		return LESH_ERR_INVAL;
	for (auto it = registry->armed_timers.begin(); it != registry->armed_timers.end(); ++it) {
		if (*it != id)
			continue;
		registry->armed_timers.erase(it);
		registry->pending.push_back(lesh::leshper::disarm_timer{id});
		return LESH_OK;
	}
	// Reported rather than silently accepted: stopping a timer twice means the
	// caller has lost track of an id, and an id it has lost track of may now
	// belong to somebody else's timer.
	return LESH_ERR_NOTFOUND;
}

// --- Editor state ----------------------------------------------------------

int32_t lesh_buffer_length(lesh_editor* editor, size_t* out) {
	LESH_EDITOR_HANDLE(editor);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = editor->staged.size();
	return LESH_OK;
}

int32_t lesh_buffer_get(lesh_editor* editor, char* out, size_t capacity, size_t* length_out) {
	LESH_EDITOR_HANDLE(editor);
	return copy_out(editor->staged, out, capacity, length_out);
}

int32_t lesh_buffer_read(lesh_editor* editor, size_t from, size_t to, char* out,
                         size_t capacity, size_t* length_out) {
	LESH_EDITOR_HANDLE(editor);
	const std::string_view text{editor->staged};
	const std::size_t begin = clamp_into(text, from);
	const std::size_t end = clamp_into(text, to);
	if (end <= begin)
		return copy_out({}, out, capacity, length_out);
	return copy_out(text.substr(begin, end - begin), out, capacity, length_out);
}

int32_t lesh_buffer_replace(lesh_editor* editor, size_t from, size_t to,
                            const char* bytes, size_t length) {
	LESH_EDITOR_HANDLE(editor);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;

	// Clamp, then snap outward. A binding hands us byte offsets it got from
	// somewhere - a regex match, a variable a user typed into - and the ABI's
	// promise is that no such offset can leave a grapheme cluster in halves.
	const std::string_view text{editor->staged};
	std::size_t begin = clamp_into(text, from);
	std::size_t end = clamp_into(text, to);
	if (end < begin)
		std::swap(begin, end);
	begin = snap_back(text, begin);
	end = snap_forward(text, end);

	editor->staged.replace(begin, end - begin, bytes == nullptr ? "" : bytes, length);
	editor->buffer_written = true;
	// The cursor follows the edit to where the replacement ends, which is what
	// every one of the ten built-ins wants and what `apply_edit` already does on
	// the enum path. An action that wants it elsewhere says so afterwards.
	editor->staged_cursor = begin + length;
	editor->cursor_written = true;
	return LESH_OK;
}

int32_t lesh_buffer_set(lesh_editor* editor, const char* bytes, size_t length) {
	LESH_EDITOR_HANDLE(editor);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	editor->staged.assign(bytes == nullptr ? "" : bytes, length);
	editor->buffer_written = true;
	// Deliberately NOT moved: `$BUFFER=...` in the lesh binding replaces the
	// text and leaves `$CURSOR` where the user put it. It clamps and snaps at
	// commit like every other position.
	return LESH_OK;
}

int32_t lesh_cursor_get(lesh_editor* editor, size_t* out) {
	LESH_EDITOR_HANDLE(editor);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = editor->staged_cursor;
	return LESH_OK;
}

int32_t lesh_cursor_set(lesh_editor* editor, size_t offset) {
	LESH_EDITOR_HANDLE(editor);
	editor->staged_cursor = offset;
	editor->cursor_written = true;
	return LESH_OK;
}

// The selection, backed for real (#96, spec §6.3).
//
// Singular, and staying singular until multi-cursor arrives as ADDITIVE plural
// functions - #96 decision 5 and #93's growth rule, in the same breath.
//
// The region the getter reports is the derived one, `[min(anchor, head),
// max(anchor, head))`, over the STAGED anchor and the STAGED cursor: an action
// that has moved the cursor is looking at the selection its own motion made,
// which is the whole of what a helix-mode motion needs to see.
int32_t lesh_selection_get(lesh_editor* editor, size_t* start_out, size_t* end_out,
                           int32_t* active_out) {
	LESH_EDITOR_HANDLE(editor);
	if (start_out == nullptr || end_out == nullptr || active_out == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view text{editor->staged};
	const std::size_t anchor = snap_back(text, editor->staged_anchor);
	const std::size_t head = snap_back(text, editor->staged_cursor);
	// Reported whether or not the region is live, because the anchor outlives
	// deactivation (emacs's mark) and a binding asking where the mark is deserves
	// an answer. The flag is the separate question, and it is the one that says
	// whether the range means anything.
	*start_out = anchor < head ? anchor : head;
	*end_out = anchor < head ? head : anchor;
	*active_out = editor->staged_selection_active ? 1 : 0;
	return LESH_OK;
}

// Sets the region to `[start, end)` and activates it. The head lands on `end`,
// because the head IS the cursor - there is nowhere else for it to go, and a
// setter that left the cursor behind would leave the state describing a
// different region than the one it was just handed.
//
// `start > end` is not an error and is not swapped: the pair is a direction, and
// a backward selection (helix's, vi's `o`-flipped one) is the reason the model
// stores an anchor and a head rather than a sorted pair. The derived range comes
// out sorted either way.
//
// Both endpoints clamp and snap BACK to a cluster start, not outward the way
// lesh_buffer_replace snaps: a selection endpoint is a cursor-like position that
// rests on a cluster, where a replacement's range must swallow whole clusters.
int32_t lesh_selection_set(lesh_editor* editor, size_t start, size_t end) {
	LESH_EDITOR_HANDLE(editor);
	const std::string_view text{editor->staged};
	editor->staged_anchor = snap_back(text, start);
	editor->staged_cursor = snap_back(text, end);
	editor->staged_selection_active = true;
	editor->selection_written = true;
	editor->cursor_written = true;
	return LESH_OK;
}

// Deactivates the region and KEEPS the anchor, which is what state::
// drop_selection does and for the reason it gives: emacs's mark survives
// `deactivate-mark`. A binding that wants the mark moved says where.
int32_t lesh_selection_clear(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	editor->staged_selection_active = false;
	editor->selection_written = true;
	return LESH_OK;
}

int32_t lesh_generation(lesh_editor* editor, uint64_t* out) {
	LESH_EDITOR_HANDLE(editor);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = editor->target->gen.value();
	return LESH_OK;
}

int32_t lesh_position_move(lesh_editor* editor, size_t from, lesh_motion motion, size_t* out) {
	LESH_EDITOR_HANDLE(editor);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view text{editor->staged};
	const std::size_t at = clamp_into(text, from);
	switch (motion) {
	case LESH_MOTION_PREV_CLUSTER:
		*out = lesh::grapheme::prev_boundary(text, at);
		return LESH_OK;
	case LESH_MOTION_NEXT_CLUSTER:
		*out = lesh::grapheme::next_boundary(text, at);
		return LESH_OK;
	case LESH_MOTION_LINE_START:
		*out = line_start_of(text, at);
		return LESH_OK;
	case LESH_MOTION_LINE_END:
		*out = line_end_of(text, at);
		return LESH_OK;
	case LESH_MOTION_PREV_WORD:
		*out = prev_word_of(text, at);
		return LESH_OK;
	case LESH_MOTION_NEXT_WORD:
		*out = next_word_of(text, at);
		return LESH_OK;
	case LESH_MOTION_BUFFER_START:
		*out = 0;
		return LESH_OK;
	case LESH_MOTION_BUFFER_END:
		*out = text.size();
		return LESH_OK;
	case LESH_MOTION_LINE_FIRST_NONBLANK:
		*out = first_nonblank_of(text, at);
		return LESH_OK;
	case LESH_MOTION_LINE_UP:
		*out = line_up_of(text, at);
		return LESH_OK;
	case LESH_MOTION_LINE_DOWN:
		*out = line_down_of(text, at);
		return LESH_OK;
	case LESH_MOTION_WORD_START_NEXT:
		*out = word_start_next(text, at, false);
		return LESH_OK;
	case LESH_MOTION_WORD_START_PREV:
		*out = word_start_prev(text, at, false);
		return LESH_OK;
	case LESH_MOTION_WORD_END_NEXT:
		*out = word_end_next(text, at, false);
		return LESH_OK;
	case LESH_MOTION_BLANK_WORD_START_NEXT:
		*out = word_start_next(text, at, true);
		return LESH_OK;
	case LESH_MOTION_BLANK_WORD_START_PREV:
		*out = word_start_prev(text, at, true);
		return LESH_OK;
	case LESH_MOTION_BLANK_WORD_END_NEXT:
		*out = word_end_next(text, at, true);
		return LESH_OK;
	}
	return LESH_ERR_INVAL;  // an enumerator from a newer header than this build
}

// --- The delimiter pair (#99 answer 2), over the staged text ----------------

int32_t lesh_match_pair(lesh_editor* editor, size_t at, uint32_t open, uint32_t close,
                        size_t* start_out, size_t* end_out) {
	LESH_EDITOR_HANDLE(editor);
	if (start_out == nullptr || end_out == nullptr)
		return LESH_ERR_INVAL;
	// ASCII only, and refused rather than guessed at: a multi-byte pair - the
	// typographic quotes, the CJK brackets - is a question no consumer has asked,
	// and answering it wrongly now would fix the wrong answer in place.
	if (open == 0 || open > 0x7F || close == 0 || close > 0x7F)
		return LESH_ERR_INVAL;
	std::size_t begin = 0;
	std::size_t end = 0;
	if (!match_pair_in(editor->staged, at, static_cast<char>(open), static_cast<char>(close),
	                   begin, end))
		return LESH_ERR_NOTFOUND;
	*start_out = begin;
	*end_out = end;
	return LESH_OK;
}

int32_t lesh_span_at(lesh_editor* editor, size_t at, lesh_span which,
                     size_t* start_out, size_t* end_out) {
	LESH_EDITOR_HANDLE(editor);
	if (start_out == nullptr || end_out == nullptr)
		return LESH_ERR_INVAL;
	bool big = false;
	switch (which) {
	case LESH_SPAN_WORD:
		big = false;
		break;
	case LESH_SPAN_BLANK_WORD:
		big = true;
		break;
	default:
		return LESH_ERR_INVAL;   // an enumerator from a newer header than this build
	}
	const std::string_view text{editor->staged};
	std::size_t p = snap_back(text, at);
	if (p >= text.size()) {
		*start_out = text.size();
		*end_out = text.size();
		return LESH_OK;
	}
	const char_class here = class_of(text[p], big);
	std::size_t begin = p;
	while (begin > 0) {
		const std::size_t back = lesh::grapheme::prev_boundary(text, begin);
		if (class_of(text[back], big) != here)
			break;
		begin = back;
	}
	std::size_t end = p;
	while (end < text.size() && class_of(text[end], big) == here)
		end = lesh::grapheme::next_boundary(text, end);
	*start_out = begin;
	*end_out = end;
	return LESH_OK;
}

// --- Loop outcomes ---------------------------------------------------------

namespace {

std::int32_t request_outcome(lesh_editor* editor, loop_outcome which, std::int32_t status) {
	editor->outcome = static_cast<std::uint8_t>(which);
	editor->exit_status = status;
	return LESH_OK;
}

} // namespace

int32_t lesh_accept_line(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	return request_outcome(editor, loop_outcome::accept_line, 0);
}

int32_t lesh_cancel_line(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	return request_outcome(editor, loop_outcome::cancel_line, 0);
}

int32_t lesh_exit(lesh_editor* editor, int32_t status) {
	LESH_EDITOR_HANDLE(editor);
	return request_outcome(editor, loop_outcome::exit, status);
}

int32_t lesh_recursive_edit(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	return request_outcome(editor, loop_outcome::recursive_edit, 0);
}

int32_t lesh_undo(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	// History movement is not a staged write, and mixing the two in one call is
	// not a thing the history can mean: undo restores the buffer to a state the
	// staged edits were never applied to.
	if (editor->buffer_written)
		return LESH_ERR_REFUSED;
	lesh::leshper::state& target = *editor->target;
	if (target.undo_one()) {
		target.gen.bump();
		editor->staged.assign(target.buffer.text());
		editor->staged_cursor = target.cursor.byte_offset();
		editor->cursor_written = false;
		// The staging area is re-synced from the state history just restored,
		// selection included: an action that undoes and then reads the selection
		// must see the one that came back, not the one it started the call with.
		editor->staged_anchor = target.selection_anchor().byte_offset();
		editor->staged_selection_active = target.selection_active();
		editor->selection_written = false;
	}
	return LESH_OK;  // nothing to undo is not an error
}

int32_t lesh_redo(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	if (editor->buffer_written)
		return LESH_ERR_REFUSED;
	lesh::leshper::state& target = *editor->target;
	if (target.redo_one()) {
		target.gen.bump();
		editor->staged.assign(target.buffer.text());
		editor->staged_cursor = target.cursor.byte_offset();
		editor->cursor_written = false;
		editor->staged_anchor = target.selection_anchor().byte_offset();
		editor->staged_selection_active = target.selection_active();
		editor->selection_written = false;
	}
	return LESH_OK;
}

int32_t lesh_push_input(lesh_editor* editor, const char* bytes, size_t length) {
	LESH_EDITOR_HANDLE(editor);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	editor->pushed_input.append(bytes == nullptr ? "" : bytes, length);
	return LESH_OK;
}

// --- Modes, the keymap stack, and the operator slot (#119) ------------------
//
// WRITTEN THROUGH TO THE STATE, not staged. See the note in abi.h: a mode is
// not in the undo history, and dispatch has to be able to read the stack back
// the instant the action returns to decide whether an operator is now pending.

int32_t lesh_mode_get(lesh_editor* editor, char* out, size_t capacity, size_t* length_out) {
	LESH_EDITOR_HANDLE(editor);
	return copy_out(editor->target->keymaps.mode(), out, capacity, length_out);
}

int32_t lesh_mode_set(lesh_editor* editor, const char* name) {
	LESH_EDITOR_HANDLE(editor);
	if (name == nullptr || *name == '\0')
		return LESH_ERR_INVAL;
	// A name no keymap has is ACCEPTED. `bind` may create it later, and a mode
	// naming a table that does not exist dispatches to nothing - which
	// resolve_keys already survives, skipping the layer rather than failing.
	editor->target->keymaps.set_mode(std::string_view{name});
	return LESH_OK;
}

int32_t lesh_keymap_push(lesh_editor* editor, const char* name) {
	LESH_EDITOR_HANDLE(editor);
	if (name == nullptr || *name == '\0')
		return LESH_ERR_INVAL;
	editor->target->keymaps.push(std::string_view{name});
	return LESH_OK;
}

int32_t lesh_keymap_pop(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	// Refused rather than silently ignored at the base: a mode is not something
	// one can pop out of, and an action that thinks it popped one has lost track
	// of its own pushes.
	return editor->target->keymaps.pop() ? LESH_OK : LESH_ERR_REFUSED;
}

int32_t lesh_pending_operator_get(lesh_editor* editor, char* out, size_t capacity,
                                  size_t* length_out) {
	LESH_EDITOR_HANDLE(editor);
	return copy_out(editor->target->keymaps.pending_operator, out, capacity, length_out);
}

int32_t lesh_pending_operator_set(lesh_editor* editor, const char* action) {
	LESH_EDITOR_HANDLE(editor);
	if (action == nullptr || *action == '\0') {
		editor->target->keymaps.pending_operator.clear();
		return LESH_OK;
	}
	if (!is_snake_case(std::string_view{action}))
		return LESH_ERR_INVAL;
	editor->target->keymaps.pending_operator.assign(action);
	return LESH_OK;
}

// --- The pending numeric argument (#119) ------------------------------------

int32_t lesh_numeric_argument_set(lesh_editor* editor, int64_t value) {
	LESH_EDITOR_HANDLE(editor);
	editor->target->keymaps.pending_count = value;
	editor->target->keymaps.has_pending_count = true;
	return LESH_OK;
}

int32_t lesh_numeric_argument_clear(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	editor->target->keymaps.clear_count();
	return LESH_OK;
}

// --- The kill store (#99 answer 3) ------------------------------------------
//
// Straight to the state, like the mode and for a related reason: a kill is not
// a buffer write and has no diff to commit. The DELETION that accompanies it is
// an ordinary staged write and commits with everything else, so `dw` is still
// one undo entry - undoing it puts the text back in the buffer and deliberately
// leaves the register holding it, which is what every editor with a register
// does and what makes `u` then `p` a way to duplicate a word.

int32_t lesh_kill_set(lesh_editor* editor, const char* key, const char* bytes,
                      size_t length, uint32_t flags) {
	LESH_EDITOR_HANDLE(editor);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	constexpr std::uint32_t known = LESH_KILL_LINEWISE;
	if ((flags & ~known) != 0)
		return LESH_ERR_INVAL;
	editor->target->kills.put(key == nullptr ? lesh::leshper::kill_store::unnamed
	                                         : std::string_view{key},
	                          std::string_view{bytes == nullptr ? "" : bytes, length}, flags);
	return LESH_OK;
}

int32_t lesh_kill_get(lesh_editor* editor, const char* key, char* out, size_t capacity,
                      size_t* length_out, uint32_t* flags_out) {
	LESH_EDITOR_HANDLE(editor);
	const auto* entry = editor->target->kills.get(
		key == nullptr ? lesh::leshper::kill_store::unnamed : std::string_view{key});
	if (entry == nullptr)
		return LESH_ERR_NOTFOUND;
	if (flags_out != nullptr)
		*flags_out = entry->flags;
	return copy_out(entry->text, out, capacity, length_out);
}

// --- The last change, for `.` (#99 answer 4) --------------------------------

int32_t lesh_last_change_keys(lesh_editor* editor, char* out, size_t capacity,
                              size_t* length_out) {
	LESH_EDITOR_HANDLE(editor);
	const lesh::leshper::change_replay& record = editor->target->repeat;
	if (!record.recorded())
		return LESH_ERR_NOTFOUND;
	return copy_out(record.keys, out, capacity, length_out);
}

int32_t lesh_last_change_replayable(lesh_editor* editor, int32_t* out) {
	LESH_EDITOR_HANDLE(editor);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	const lesh::leshper::state& target = *editor->target;
	*out = target.repeat.replayable(target.keymaps.mode()) ? 1 : 0;
	return LESH_OK;
}

// --- The request token -----------------------------------------------------

int32_t lesh_request_buffer_length(const lesh_request* request, size_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->buffer.size();
	return LESH_OK;
}

int32_t lesh_request_buffer(const lesh_request* request, char* out, size_t capacity,
                            size_t* length_out) {
	LESH_REQUEST_HANDLE(request);
	return copy_out(request->buffer, out, capacity, length_out);
}

int32_t lesh_request_cursor(const lesh_request* request, size_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->cursor;
	return LESH_OK;
}

int32_t lesh_request_selection(const lesh_request* request, size_t* start_out,
                               size_t* end_out, int32_t* active_out) {
	LESH_REQUEST_HANDLE(request);
	if (start_out == nullptr || end_out == nullptr || active_out == nullptr)
		return LESH_ERR_INVAL;
	*start_out = request->selection_start;
	*end_out = request->selection_end;
	*active_out = request->selection_active ? 1 : 0;
	return LESH_OK;
}

int32_t lesh_request_generation(const lesh_request* request, uint64_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->computed_against.value();
	return LESH_OK;
}

int32_t lesh_request_event_kind(const lesh_request* request, uint32_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->event_kind;
	return LESH_OK;
}

int32_t lesh_request_superseded(const lesh_request* request, int32_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr)
		return LESH_ERR_INVAL;
	*out = request->superseded != nullptr
	       && request->superseded->load(std::memory_order_relaxed) ? 1 : 0;
	return LESH_OK;
}

int32_t lesh_request_command_kind(const lesh_request* request, const char* name,
                                  size_t length, uint32_t* out) {
	LESH_REQUEST_HANDLE(request);
	if (out == nullptr || (name == nullptr && length != 0))
		return LESH_ERR_INVAL;
	*out = LESH_COMMAND_UNKNOWN;
	if (length == 0)
		return LESH_OK;

	const std::string_view given{name, length};
	// A NUL inside the name would truncate the candidate handed to stat(2), and
	// a truncated name is a different name. The buffer is bytes and may contain
	// one, so this is a real input and not a theoretical one.
	if (given.find('\0') != std::string_view::npos)
		return LESH_OK;

	if (memo_find(request->command_kinds, given, *out))
		return LESH_OK;

	// The documented fallback when no shell is attached: empty tables, and the
	// process environment's PATH. A file-scope instance rather than one per
	// call, because it holds nothing and constructing a vtable pointer per
	// command name is work for no answer.
	static const environment_knowledge kEnvironment;
	const shell_knowledge& shell =
		request->knowledge != nullptr ? *request->knowledge : kEnvironment;

	*out = static_cast<std::uint32_t>(classify_command_name(shell, given));
	memo_store(request->command_kinds, given, *out);
	return LESH_OK;
}

int32_t lesh_emit_span(lesh_request* request, size_t start, size_t end, uint32_t style_id) {
	LESH_REQUEST_HANDLE(request);
	if (request->spans == nullptr)
		return LESH_ERR_INVAL;
	const std::string_view text{request->buffer};
	std::size_t begin = clamp_into(text, start);
	std::size_t stop = clamp_into(text, end);
	if (stop < begin)
		std::swap(begin, stop);
	// Copied at the call site, into storage the loop owns: the worker's arena
	// dies with the request (#90) and nothing it allocated is retained.
	request->spans->push_back(decoration_span{begin, stop, style_id});
	return LESH_OK;
}

int32_t lesh_emit_virtual_text(lesh_request* request, size_t at, const char* bytes,
                               size_t length) {
	LESH_REQUEST_HANDLE(request);
	if (request->texts == nullptr)
		return LESH_ERR_INVAL;
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	request->texts->push_back(
		virtual_text{clamp_into(request->buffer, at), std::string(bytes == nullptr ? "" : bytes, length)});
	return LESH_OK;
}

int32_t lesh_emit_virtual_text_styled(lesh_request* request, size_t at, const char* bytes,
                                      size_t length, uint32_t style_id) {
	LESH_REQUEST_HANDLE(request);
	if (request->texts == nullptr)
		return LESH_ERR_INVAL;
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	// An id nobody interned is not refused. The registry is the loop's and this
	// runs on a worker, which has no business looking it up; a batch carrying a
	// nonsense id renders unstyled, which is the same failure an unthemed name
	// already has.
	request->texts->push_back(
		virtual_text{clamp_into(request->buffer, at),
	                 std::string(bytes == nullptr ? "" : bytes, length), style_id});
	return LESH_OK;
}

int32_t lesh_propose(lesh_request* request, uint32_t kind, const char* bytes, size_t length) {
	LESH_REQUEST_HANDLE(request);
	if (request->proposals == nullptr)
		return LESH_ERR_INVAL;
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	if (kind != LESH_PROPOSAL_AUTOSUGGESTION && kind != LESH_PROPOSAL_COMPLETION &&
	    kind != LESH_PROPOSAL_HISTORY_MATCH)
		return LESH_ERR_INVAL;
	request->proposals->push_back(
		proposal{kind, std::string(bytes == nullptr ? "" : bytes, length)});
	return LESH_OK;
}

// --- Proposals, from the action side (#133, F-25) --------------------------

int32_t lesh_proposal_read(lesh_editor* editor, uint32_t kind, size_t index, char* out,
                           size_t capacity, size_t* length_out) {
	LESH_EDITOR_HANDLE(editor);
	if (length_out == nullptr)
		return LESH_ERR_INVAL;
	*length_out = 0;
	// WHAT IS ON SCREEN, reached through the handle's own `target` (#144). The
	// walk - emission order across the applied batches, and within one batch the
	// order the reactor proposed in - is `applied_proposals::find`, so the rule
	// abi.h documents has one implementation and the pager's 0, 1, 2 and the
	// autosuggester's single candidate at 0 come out of the same code.
	const proposal* const found = editor->target->proposals.find(kind, index);
	if (found == nullptr)
		return LESH_ERR_NOTFOUND;
	return copy_out(found->bytes, out, capacity, length_out);
}

// --- Completion, from the action side (#139, F-28/F-30) --------------------

int32_t lesh_complete(lesh_editor* editor, size_t* count_out) {
	LESH_EDITOR_HANDLE(editor);
	if (count_out == nullptr)
		return LESH_ERR_INVAL;
	*count_out = 0;
	editor->completion.clear();
	editor->completion_ran = false;
	if (editor->registry == nullptr || editor->registry->completion == nullptr)
		return LESH_ERR_NOTFOUND;

	// THE STAGED BUFFER, not the target's. An action may have edited before it
	// asked - a `complete_word` bound after an `end_of_line`, or a vi operator
	// that pushed a key - and completing text the user can no longer see would be
	// the class of bug staging exists to prevent. The staging area is what
	// `lesh_position_move` reads too, for exactly this reason (#133).
	lesh::leshper::completion_query query;
	query.buffer = editor->staged;
	query.cursor = editor->staged_cursor;
	editor->registry->completion->complete(query, editor->completion);
	editor->completion_ran = true;
	*count_out = editor->completion.candidates.size();
	return LESH_OK;
}

int32_t lesh_completion_range(lesh_editor* editor, size_t* from_out, size_t* to_out) {
	LESH_EDITOR_HANDLE(editor);
	if (!editor->completion_ran)
		return LESH_ERR_NOTFOUND;
	if (from_out != nullptr)
		*from_out = editor->completion.replace_from;
	if (to_out != nullptr)
		*to_out = editor->completion.replace_to;
	return LESH_OK;
}

int32_t lesh_completion_candidate(lesh_editor* editor, size_t index, char* out,
                                  size_t capacity, size_t* length_out, uint32_t* kind_out) {
	LESH_EDITOR_HANDLE(editor);
	if (length_out == nullptr)
		return LESH_ERR_INVAL;
	*length_out = 0;
	if (!editor->completion_ran || index >= editor->completion.candidates.size())
		return LESH_ERR_NOTFOUND;
	const lesh::leshper::candidate& one = editor->completion.candidates[index];
	// The KIND FIRST, so a caller that got LESH_ERR_TOOSMALL still knows what it
	// was about to add and can ask again with a bigger buffer.
	if (kind_out != nullptr)
		*kind_out = static_cast<std::uint32_t>(one.kind);
	return copy_out(one.text, out, capacity, length_out);
}

int32_t lesh_proposal_dismiss(lesh_editor* editor, uint32_t kind) {
	LESH_EDITOR_HANDLE(editor);
	// Requested, never performed - see the header. The loop reads this once the
	// action's writes have been committed, so an action that dismisses and then
	// changes its mind leaves the screen as it found it.
	editor->dismissed_kind = kind;
	editor->dismiss_requested = true;
	return LESH_OK;
}

// --- The pager (#138, F-28 to F-30, spec §6.9) ------------------------------
//
// WRITTEN THROUGH TO THE STATE, like the mode and the keymap stack, and for the
// same two reasons: the pager is not in the undo history, and dispatch reads the
// keymap stack back the instant the action returns. The one exception is the one
// thing that touches the buffer - the insertion below - which stages exactly as
// `lesh_buffer_replace` stages, so A-12's "through the accepting action, as one
// undo entry" holds without the pager being privileged.

int32_t lesh_pager_open(lesh_editor* editor, size_t from, size_t to) {
	LESH_EDITOR_HANDLE(editor);
	close_pager(editor);

	const std::string_view text{editor->staged};
	std::size_t begin = clamp_into(text, from);
	std::size_t end = clamp_into(text, to);
	if (end < begin)
		std::swap(begin, end);
	pager_state& pager = editor->target->pager;
	pager.replace_from = position::from_byte_offset(snap_back(text, begin));
	pager.replace_to = position::from_byte_offset(snap_forward(text, end));
	return LESH_OK;
}

int32_t lesh_pager_add(lesh_editor* editor, const char* bytes, size_t length,
                       uint32_t kind) {
	LESH_EDITOR_HANDLE(editor);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	if (!kind_is_known(kind))
		return LESH_ERR_INVAL;
	pager_candidate one;
	one.text.assign(bytes == nullptr ? "" : bytes, length);
	one.kind = static_cast<pager_kind>(kind);
	editor->target->pager.candidates.push_back(std::move(one));
	return LESH_OK;
}

int32_t lesh_pager_commit(lesh_editor* editor, uint32_t* outcome_out) {
	LESH_EDITOR_HANDLE(editor);
	pager_state& pager = editor->target->pager;
	lesh::leshper::pager_refilter(pager);

	// What the user has typed of the span, read from the STAGED bytes: an action
	// that edited before committing is completing what it wrote.
	const std::string_view text{editor->staged};
	const std::size_t begin = clamp_into(text, pager.replace_from.byte_offset());
	const std::size_t end = clamp_into(text, pager.replace_to.byte_offset());
	const std::string_view typed =
		end > begin ? text.substr(begin, end - begin) : std::string_view{};

	const pager_decision decided = lesh::leshper::decide_pager(pager, typed);
	std::uint32_t outcome = LESH_PAGER_NOTHING;
	switch (decided.what) {
		case pager_decision::kind::nothing:
			close_pager(editor);
			break;
		case pager_decision::kind::insert:
			// F-30: the pager never opens. The insertion is staged, so the
			// common prefix and the accepted candidate reach the buffer down the
			// identical path.
			stage_insertion(editor, decided.text);
			close_pager(editor);
			outcome = LESH_PAGER_INSERTED;
			break;
		case pager_decision::kind::open:
			pager.open = true;
			pager.selected = 0;
			pager.scroll_row = 0;
			editor->target->keymaps.push(pager_keymap_name);
			outcome = LESH_PAGER_OPENED;
			break;
	}
	if (outcome_out != nullptr)
		*outcome_out = outcome;
	return LESH_OK;
}

int32_t lesh_pager_accept(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	const pager_candidate* one = lesh::leshper::pager_selected(editor->target->pager);
	if (one == nullptr)
		return LESH_ERR_NOTFOUND;
	std::string with;
	lesh::leshper::pager_insertion(*one, with);
	stage_insertion(editor, with);
	close_pager(editor);
	return LESH_OK;
}

int32_t lesh_pager_close(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	close_pager(editor);
	return LESH_OK;
}

int32_t lesh_pager_status(lesh_editor* editor, int32_t* open_out, size_t* count_out,
                          size_t* selected_out) {
	LESH_EDITOR_HANDLE(editor);
	const pager_state& pager = editor->target->pager;
	if (open_out != nullptr)
		*open_out = pager.showing() ? 1 : 0;
	if (count_out != nullptr)
		*count_out = pager.matching.size();
	if (selected_out != nullptr)
		*selected_out = pager.selected;
	return LESH_OK;
}

int32_t lesh_pager_range(lesh_editor* editor, size_t* from_out, size_t* to_out) {
	LESH_EDITOR_HANDLE(editor);
	const pager_state& pager = editor->target->pager;
	if (!pager.open)
		return LESH_ERR_NOTFOUND;
	if (from_out != nullptr)
		*from_out = pager.replace_from.byte_offset();
	if (to_out != nullptr)
		*to_out = pager.replace_to.byte_offset();
	return LESH_OK;
}

int32_t lesh_pager_selected(lesh_editor* editor, char* out, size_t capacity,
                            size_t* length_out, uint32_t* kind_out) {
	LESH_EDITOR_HANDLE(editor);
	const pager_candidate* one = lesh::leshper::pager_selected(editor->target->pager);
	if (one == nullptr) {
		if (length_out != nullptr)
			*length_out = 0;
		return LESH_ERR_NOTFOUND;
	}
	if (kind_out != nullptr)
		*kind_out = static_cast<std::uint32_t>(one->kind);
	return copy_out(one->text, out, capacity, length_out);
}

int32_t lesh_pager_move(lesh_editor* editor, int64_t by, uint32_t axis) {
	LESH_EDITOR_HANDLE(editor);
	pager_state& pager = editor->target->pager;
	if (!pager.showing())
		return LESH_ERR_NOTFOUND;
	if (axis != LESH_PAGER_BY_CANDIDATE && axis != LESH_PAGER_BY_ROW)
		return LESH_ERR_INVAL;

	const pager_grid grid = grid_of(editor);
	// A row is as wide as the grid is, which is a question about the terminal
	// the binding is not holding. One column - the answer at an unknown size -
	// makes a row move a candidate move, which is the honest degradation.
	const std::int64_t step = axis == LESH_PAGER_BY_ROW
	                              ? static_cast<std::int64_t>(std::max<std::uint16_t>(1, grid.columns))
	                              : 1;
	lesh::leshper::pager_move(pager, by * step);
	lesh::leshper::pager_reveal(pager, grid);
	return LESH_OK;
}

int32_t lesh_pager_filter_push(lesh_editor* editor, const char* bytes, size_t length) {
	LESH_EDITOR_HANDLE(editor);
	if (bytes == nullptr && length != 0)
		return LESH_ERR_INVAL;
	pager_state& pager = editor->target->pager;
	if (!pager.open)
		return LESH_ERR_NOTFOUND;
	if (length == 0)
		return LESH_OK;
	pager.filter.append(bytes, length);
	lesh::leshper::pager_refilter(pager);
	lesh::leshper::pager_reveal(pager, grid_of(editor));
	return LESH_OK;
}

int32_t lesh_pager_filter_pop(lesh_editor* editor) {
	LESH_EDITOR_HANDLE(editor);
	pager_state& pager = editor->target->pager;
	if (!pager.open)
		return LESH_ERR_NOTFOUND;
	if (!lesh::leshper::pager_filter_pop(pager))
		return LESH_ERR_NOTFOUND;
	lesh::leshper::pager_reveal(pager, grid_of(editor));
	return LESH_OK;
}

} // extern "C"

// ===========================================================================
// The loop side.
// ===========================================================================

namespace lesh::leshper {
namespace {

// Diffs the staged bytes against the buffer and answers the ONE replacement
// that explains the difference: common prefix off the front, common suffix off
// the back, whatever is left in the middle.
//
// One replacement is the whole point. An action that edited six times, or set
// the buffer wholesale, or edited and changed its mind, all arrive here as one
// record - which is what makes "one undo entry, one generation bump" (#92,
// A-12) a property of the commit rather than of the action's manners.
struct difference {
	std::size_t from = 0;
	std::size_t to = 0;
	std::string_view inserted;
	bool any = false;
};

difference diff_of(std::string_view was, std::string_view now) noexcept {
	if (was == now)
		return difference{};
	std::size_t prefix = 0;
	const std::size_t shortest = was.size() < now.size() ? was.size() : now.size();
	while (prefix < shortest && was[prefix] == now[prefix])
		++prefix;
	std::size_t suffix = 0;
	while (suffix < shortest - prefix && was[was.size() - 1 - suffix] == now[now.size() - 1 - suffix])
		++suffix;
	difference d;
	d.from = prefix;
	d.to = was.size() - suffix;
	d.inserted = now.substr(prefix, now.size() - suffix - prefix);
	d.any = true;
	return d;
}

} // namespace

bool handle_is_live(const editor_handle* handle) noexcept {
	return handle != nullptr && handle->live() && handle->target != nullptr
	    && handle->owner_thread == current_thread_key();
}

bool token_is_live(const request_token* token) noexcept {
	return token != nullptr && token->live() && token->owner_thread == current_thread_key();
}

action_result loop_harness::invoke(state& target, std::string_view name,
                                   const invocation& how) {
	action_result result;

	const auto found = _registry->actions.find(name);
	if (found == _registry->actions.end()) {
		result.status = LESH_ERR_NOTFOUND;
		return result;
	}

	const generation before = target.gen;
	const position cursor_before = target.cursor;

	// The handle is a member, reused: dispatch happens once per keystroke and
	// N-2 wants the hot path free of allocation the loop did not have to do.
	_handle.target = &target;
	_handle.registry = _registry;
	_handle.staged.assign(target.buffer.text());
	_handle.staged_cursor = target.cursor.byte_offset();
	_handle.buffer_written = false;
	_handle.cursor_written = false;
	_handle.staged_anchor = target.selection_anchor().byte_offset();
	_handle.staged_selection_active = target.selection_active();
	_handle.selection_written = false;
	_handle.pushed_input.clear();
	// What is on screen, for an accepting action to read (#133), needs no field
	// here: it is `target.proposals`, and `target` is set above (#144).
	_handle.dismissed_kind = 0;
	_handle.dismiss_requested = false;
	// Cleared per call and not per Tab: an action that did not ask for a
	// completion must not read the last one's candidates back (#139). `clear`
	// keeps the vectors' capacity, which is the point of the handle being a
	// member at all.
	_handle.completion.clear();
	_handle.completion_ran = false;
	_handle.outcome = static_cast<std::uint8_t>(loop_outcome::none);
	_handle.exit_status = 0;
	_handle.depth = 0;
	_handle.owner_thread = current_thread_key();
	_handle.call_token = ++_registry->calls;

	lesh_invocation crossing{};
	// The registry's own key: already NUL-terminated, stable for the call, and
	// not a copy that could disagree with the name dispatch actually matched.
	crossing.action_name = found->first.c_str();
	crossing.keys = how.keys.empty() ? nullptr : how.keys.data();
	crossing.keys_length = how.keys.size();
	crossing.numeric_argument = how.numeric_argument;
	crossing.has_numeric_argument = how.has_numeric_argument ? 1 : 0;

	result.status = found->second.fn(&_handle, &crossing, found->second.userdata);

	// The action is done, so the handle is dead from here: an accessor called on
	// it now fires the debug assertion, which is the half of "valid only for the
	// receiving call" that a check can catch. What follows is the loop reading
	// its own staging area, which it owns and which no handle guards.
	_handle.call_token = 0;
	_handle.owner_thread = 0;

	const difference change = diff_of(target.buffer.text(), _handle.staged);

	// F-4's coalescing rule, decided from what the action DID rather than from
	// which action it was. The loop has no names to go by and does not need any:
	// a typing run is a run of KEYSTROKES, and the commit can recognise one
	// without being told which action produced it.
	//
	// A keystroke leaves three marks together, and all three are checked here
	// because a block write can wear any two of them:
	//
	//   it is a plain insertion at one point - nothing removed;
	//   it is ONE grapheme cluster - a key produces one, F-3's unit;
	//   the bytes ARE the key that dispatched the action (`how.keys`) - which is
	//   `self_insert`'s whole definition, and is what a candidate cannot fake.
	//
	// The third is the one #146 turns on. An accepted autosuggestion is shaped
	// exactly like a plain insertion at the cursor - #121 said the same of a
	// paste - so length was the only thing separating them, and length fails at
	// the case that matters: accepting `gitk` over `git` writes a single `k`.
	// What never coincides is the text: an accept arrives under `<Right>` or
	// `<Tab>`, which contribute no bytes at all (`encoded_keys_as_text`), and an
	// accept bound to a printable would still have to suggest exactly that
	// character to be mistaken for it.
	//
	// AND THE RUN IS BROKEN ON BOTH SIDES, #121's ruling applied here. Breaking
	// only before stops a block write from extending the preceding run;
	// apply_edit's record() would then re-arm coalescing behind it - a plain
	// insertion is a plain insertion to the history - and the next typed
	// character would fold into the acceptance. Accepting is its own undo step,
	// full stop.
	const bool typed_keystroke =
		change.any && change.from == change.to && !change.inserted.empty()
		&& change.inserted == how.keys
		&& lesh::grapheme::next_boundary(change.inserted, 0) == change.inserted.size();
	if (!typed_keystroke)
		target.undo.break_coalescing();

	if (change.any) {
		// Where the cursor ends up, recorded on the undo entry so F-4's "undo
		// restores text AND cursor" survives the trip. `staged_cursor` is the
		// cursor the action left behind - moved by every write to the end of the
		// replacement, which is where every built-in wants it, and moved again by
		// any explicit lesh_cursor_set. An action that only replaced the whole
		// buffer moved it nowhere, and it stays where the user had it, clamped.
		const position landing =
			position::from_byte_offset(snap_back(_handle.staged, _handle.staged_cursor));
		apply_edit(target, position::from_byte_offset(change.from),
		           position::from_byte_offset(change.to), change.inserted, &landing);
		// The second half of the both-sides break. See above.
		if (!typed_keystroke)
			target.undo.break_coalescing();
		result.buffer_changed = true;
	} else if (_handle.cursor_written) {
		target.cursor = position::from_byte_offset(snap_back(target.buffer.text(),
		                                                     _handle.staged_cursor));
	}

	// The selection, committed AFTER the edit, and only when the action wrote
	// one. An action that did not touch it has had its anchor carried across the
	// edit by apply_edit's marker rules already; an action that did wrote offsets
	// against the staged text, which is the text the buffer now holds, so the
	// staged anchor is the one that means what the action meant.
	if (_handle.selection_written) {
		target.set_selection(
			position::from_byte_offset(snap_back(target.buffer.text(), _handle.staged_anchor)),
			_handle.staged_selection_active);
	}

	if (!_handle.pushed_input.empty())
		target.pending.injected += _handle.pushed_input;

	// The dismissal, honoured after the commit (#133). The WHOLE batch goes, not
	// the proposal alone: the drawn half of a suggestion is its virtual text, and
	// a dismissal that left that on screen would have dismissed nothing the user
	// can see - so `dismiss` takes the decorations too, in one call rather than
	// in two a caller has to remember to pair.
	//
	// AND IT ASKS FOR A REPAINT (#144). A dismissal that dropped something
	// changed what is on screen while changing neither the buffer nor the cursor,
	// so the rule below - which asks by comparing those two - would have left the
	// suggestion painted until something else happened to redraw.
	if (_handle.dismiss_requested
	    && target.proposals.dismiss(_handle.dismissed_kind, target.marks))
		result.produced.push_back(render_request{});

	result.outcome = static_cast<loop_outcome>(_handle.outcome);
	result.exit_status = _handle.exit_status;
	// EMITTED, NOT LATCHED (#168). The outcome an action requested leaves as an
	// effect, which is the one channel out of a turn: `invoke_action` folds
	// `produced` into what `step` returns, so a bound `accept_line` reaches the
	// host down the same path a repaint does, and the host is spared reaching
	// back into the editor for it. `result.outcome` stays as the answer to "what
	// did this call ask for", which is what a direct caller of `invoke` reads.
	switch (result.outcome) {
		case loop_outcome::none:
			break;
		case loop_outcome::accept_line:
			result.produced.push_back(line_accepted{});
			break;
		case loop_outcome::cancel_line:
			result.produced.push_back(line_cancelled{});
			break;
		case loop_outcome::exit:
			result.produced.push_back(end_of_file{result.exit_status});
			break;
		case loop_outcome::recursive_edit:
			result.produced.push_back(recursive_edit_request{});
			break;
	}
	result.cursor_moved = target.cursor != cursor_before;
	_handle.target = nullptr;
	// The handle is dead, and so is what it was pointing an ABI reader at. The
	// candidates went to the pager while the action was running; what is cleared
	// here is the flag that would let a stashed handle read them back. (The
	// borrowed `applied` pointer this also used to null is gone - #144 moved the
	// proposal view onto `state`, where every dispatch path already points.)
	_handle.completion_ran = false;

	// The same rule the enum path follows: an action that changed nothing asks
	// for nothing, a mutation asks the reactors to recompute, and a bare cursor
	// move asks only for a redraw (A-10).
	if (target.gen != before || result.cursor_moved) {
		result.produced.push_back(render_request{});
		if (target.gen != before)
			result.produced.push_back(worker_request{target.gen});
	}
	return result;
}

std::vector<reactor_batch> loop_harness::react(const state& target, std::uint32_t kinds) {
	std::vector<reactor_batch> batches;
	_superseded.store(false, std::memory_order_relaxed);

	for (const auto& [name, entry] : _registry->reactors) {
		if ((entry.event_mask & kinds) == 0)
			continue;

		reactor_batch batch;
		batch.reactor = name;
		batch.computed_against = target.gen;
		batch.event_kind = kinds;

		request_token token;
		token.buffer.assign(target.buffer.text());
		token.cursor = target.cursor.byte_offset();
		// The derived region, snapshotted with everything else (#96). Reported
		// even when inactive, on the same reasoning lesh_selection_get gives: the
		// anchor outlives deactivation and the flag is the separate question.
		{
			const std::size_t anchor = target.selection_anchor().byte_offset();
			const std::size_t head = token.cursor;
			token.selection_start = anchor < head ? anchor : head;
			token.selection_end = anchor < head ? head : anchor;
			token.selection_active = target.selection_active();
		}
		token.computed_against = target.gen;
		token.event_kind = kinds;
		token.superseded = &_superseded;
		token.knowledge = _registry->knowledge;
		token.spans = &batch.spans;
		token.texts = &batch.texts;
		token.proposals = &batch.proposals;
		token.owner_thread = current_thread_key();
		token.call_token = ++_registry->calls;

		batch.status = entry.fn(&token, entry.userdata);

		token.call_token = 0;
		batches.push_back(std::move(batch));
	}
	return batches;
}

// The one applier (#144). See the argument in registry.h.
std::uint32_t intern_timer_action(registry& reg, std::string_view name) {
	for (std::size_t i = 0; i < reg.timer_actions.size(); ++i) {
		if (reg.timer_actions[i] == name)
			return static_cast<std::uint32_t>(i);
	}
	reg.timer_actions.emplace_back(name);
	return static_cast<std::uint32_t>(reg.timer_actions.size() - 1);
}

std::string_view timer_action_name(const registry& reg, std::uint32_t handle) noexcept {
	if (handle >= reg.timer_actions.size())
		return {};
	return reg.timer_actions[handle];
}

bool apply_batch(state& target, reactor_batch& batch) {
	// N-4, and the only place it is decided. There is no other applier and no
	// other way in, so a stale batch is not rejected here so much as it has
	// nowhere else to go.
	if (!(batch.computed_against == target.gen))
		return false;
	// THE EMITTING REACTOR IS THE NAMESPACE, on both halves (ADR-0008): a new
	// batch from a reactor replaces that reactor's decorations and that reactor's
	// offers, and touches nobody else's. Both stores swap rather than copy, so
	// the batch goes back to the pool carrying the storage the layer had.
	target.marks.apply(batch.reactor, batch.spans, batch.texts);
	target.proposals.apply(batch.reactor, batch.proposals);
	return true;
}

} // namespace lesh::leshper
