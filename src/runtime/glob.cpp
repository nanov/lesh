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

bool expand_pathnames(buffer_pool& pool, std::string_view word,
                      arena_array<std::string_view>& out) {
	if (!has_pattern_characters(word))
		return false;

	const bool absolute = !word.empty() && word[0] == '/';
	std::vector<std::string> current;
	current.emplace_back(absolute ? "/" : "");

	size_t at = absolute ? 1 : 0;
	while (at <= word.size()) {
		const size_t slash = word.find('/', at);
		const std::string_view component = word.substr(
			at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
		const bool is_last = slash == std::string_view::npos;

		if (!component.empty()) {
			std::vector<std::string> next;
			for (const auto& dir : current) {
				if (has_pattern_characters(component)) {
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
					full.append(component);
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
	if (!word.empty() && word.back() == '/') {
		for (auto& path : current)
			if (path.empty() || path.back() != '/')
				path += '/';
	}

	if (current.empty()) {
		// Nothing matched: POSIX says the word expands to ITSELF, unchanged.
		out.push(intern(pool, word));
		return true;
	}

	for (const auto& path : current)
		out.push(intern(pool, path));
	return true;
}

} // namespace lesh::runtime
