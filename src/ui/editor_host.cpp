#include "ui/editor_host.h"

#include "leshper/abi.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <string_view>

namespace lesh::ui {

namespace {

// The C numbers and the C++ enumerators are one space, and this is what keeps
// them one. A reordered enum would otherwise repaint every command name in the
// wrong colour and compile silently. The assertions were `registry.cpp`'s until
// #168 Phase B took `command_kind` out of leshper; they travelled with the enum,
// because the side that owns the names is the side that has to keep them
// agreeing with the numbers.
static_assert(static_cast<std::uint32_t>(command_kind::unknown) == LESH_COMMAND_UNKNOWN);
static_assert(static_cast<std::uint32_t>(command_kind::external) == LESH_COMMAND_EXTERNAL);
static_assert(static_cast<std::uint32_t>(command_kind::builtin) == LESH_COMMAND_BUILTIN);
static_assert(static_cast<std::uint32_t>(command_kind::function) == LESH_COMMAND_FUNCTION);
static_assert(static_cast<std::uint32_t>(command_kind::alias) == LESH_COMMAND_ALIAS);

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

} // namespace

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

std::uint32_t editor_host::classify_command(std::string_view name) const {
	// No shell attached. `unknown` and not a guess: a `$PATH` sweep with no
	// tables in front of it would paint `ll` as external the moment something on
	// the path happened to be called that, and the caller who ignores a status
	// reads the harmless answer.
	if (_knowledge == nullptr)
		return LESH_COMMAND_UNKNOWN;
	return static_cast<std::uint32_t>(classify_command_name(*_knowledge, name));
}

bool editor_host::carry_out(const leshper::want_completion& what,
                            leshper::completion_candidates& answer) {
	if (_completion == nullptr)
		return false;

	completion_query query;
	// Borrowed from the effect, which borrowed it from the staging area of the
	// action that asked - alive for the whole of this call. An empty buffer with
	// a null pointer is a legal question and `string_view{nullptr, 0}` is not, so
	// the empty case is spelled out.
	query.buffer = what.buffer == nullptr ? std::string_view{}
	                                      : std::string_view{what.buffer, what.length};
	query.cursor = what.cursor;
	_completion->complete(query, _answer);

	answer.computed_against = what.computed_against;
	answer.items = _answer.candidates.empty() ? nullptr : _answer.candidates.data();
	answer.count = _answer.candidates.size();
	answer.replace_from = _answer.replace_from;
	answer.replace_to = _answer.replace_to;
	return true;
}

} // namespace lesh::ui
