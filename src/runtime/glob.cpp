#include "runtime/glob.h"

#include "runtime/pattern.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace lesh::runtime {

namespace {

// Copies a string into the arena so the returned view outlives this call.
std::string_view intern(buffer_pool& pool, std::string_view text) {
	char* block = nullptr;
	pool.allocate(text.empty() ? 1 : text.size(), block, 1);
	if (!text.empty())
		std::memcpy(block, text.data(), text.size());
	return {block, text.size()};
}

// Expands one path component against a directory, appending full paths.
void expand_component(const std::string& dir, std::string_view component,
                      bool is_last, std::vector<std::string>& results) {
	DIR* handle = opendir(dir.empty() ? "." : dir.c_str());
	if (handle == nullptr)
		return;

	std::vector<std::string> matches;
	while (const dirent* entry = readdir(handle)) {
		const std::string_view name{entry->d_name};
		// A leading period must be matched explicitly. `.` and `..` are never
		// returned by a wildcard either, which the same rule covers.
		if (!pattern_match(component, name, /*period_is_special=*/true))
			continue;
		if (!is_last && entry->d_type != DT_DIR && entry->d_type != DT_LNK && entry->d_type != DT_UNKNOWN)
			continue;
		matches.emplace_back(name);
	}
	closedir(handle);

	// POSIX requires the results collated.
	std::sort(matches.begin(), matches.end());
	for (const auto& m : matches) {
		std::string full = dir;
		if (!full.empty() && full.back() != '/')
			full += '/';
		full += m;
		results.push_back(std::move(full));
	}
}

} // namespace

bool expand_pathnames(buffer_pool& pool, std::string_view pattern,
                      std::string_view quote_removed,
                      arena_array<std::string_view>& out) {
	// POSIX 2.13.1's bracket rule is applied HERE rather than at the expander's
	// segment gate, because it is a question about the whole word: an unterminated
	// `[` is an ordinary character, and this is the first place the assembled word
	// exists to be asked about. Returning false is the observable that says no
	// directory was opened - it is the statement before the walk.
	//
	// Asked of the PATTERN form, which is the one that still knows which
	// metacharacters arrived quoted: `a\*b` holds no live `*` and is not a pattern
	// at all, so it costs no directory scan either (#210).
	if (!is_pattern(pattern))
		return false;

	const bool absolute = !pattern.empty() && pattern[0] == '/';
	std::vector<std::string> current;
	current.emplace_back(absolute ? "/" : "");

	size_t at = absolute ? 1 : 0;
	while (at <= pattern.size()) {
		const size_t slash = pattern.find('/', at);
		const std::string_view component = pattern.substr(
			at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
		const bool is_last = slash == std::string_view::npos;

		if (!component.empty()) {
			std::vector<std::string> next;
			for (const auto& dir : current) {
				// The same question again, per component: `[a/b]` holds a `[`
				// and a `]`, but a bracket expression cannot span a `/`, so
				// neither component is a pattern and neither is scanned.
				if (is_pattern(component)) {
					expand_component(dir, component, is_last, next);
				} else {
					// A literal component: extend without a directory scan - a
					// non-last one gets its existence check for free, because the
					// NEXT component's opendir() on a path that isn't there fails
					// and drops this branch. The last component has no next step
					// to catch it, so it is confirmed here with lstat (not stat:
					// a dangling symlink still names a real directory entry and
					// must still expand).
					//
					// A failed lstat must not be read as "does not exist". ENOENT
					// means that; EACCES means an ancestor directory could not be
					// searched, so existence could not be checked AT ALL. POSIX
					// leaves an unconfirmable pattern unexpanded rather than
					// asserting a file exists that lesh was never permitted to
					// look for, so both failures are handled the same way here:
					// this candidate is dropped, and - if nothing else in
					// `current` survives - the word falls through to the
					// unmatched-pattern case below.
					std::string full = dir;
					if (!full.empty() && full.back() != '/')
						full += '/';
					// Quote removal, on this component alone. A literal component is
					// extended by NAME, and the name is the one the matcher would
					// have read had the component been scanned for: `a\*b/*` looks
					// inside the directory called `a*b` (#210). Written in place -
					// unescaping only ever shortens, so the reserved room is exact.
					const size_t base = full.size();
					full.resize(base + component.size());
					full.resize(base + remove_pattern_escapes(component,
					                                          full.data() + base));
					if (is_last) {
						struct stat st;
						if (::lstat(full.c_str(), &st) != 0)
							continue;
					}
					next.push_back(std::move(full));
				}
			}
			current = std::move(next);
		}

		if (is_last)
			break;
		at = slash + 1;
	}

	// A trailing slash is part of the word: `*/` selects directories and keeps the
	// slash, which is how `for d in */` is written.
	if (!pattern.empty() && pattern.back() == '/') {
		for (auto& path : current)
			if (path.empty() || path.back() != '/')
				path += '/';
	}

	if (current.empty()) {
		// Nothing matched: POSIX says the word expands to ITSELF, unchanged - and
		// then quote removal runs on it, which is why this is the quote-removed
		// form rather than the pattern. `echo a\*b` with no such file prints `a*b`,
		// while `x='\Q'; echo *$x` prints `*\Q`: a backslash the USER wrote is
		// removed here and one that arrived in a VALUE was never quoting at all.
		out.push(intern(pool, quote_removed));
		return true;
	}

	for (const auto& path : current)
		out.push(intern(pool, path));
	return true;
}

} // namespace lesh::runtime
