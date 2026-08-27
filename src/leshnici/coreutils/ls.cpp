#include "leshnici/coreutils/builtins.h"

#include "runtime/diagnostic.h"
#include "substrate/args.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace lesh::leshnici::coreutils {

namespace {

// `-a`, `-l`, `-1` and nothing else. The three that decide WHAT is listed and
// HOW it is shaped; every other letter GNU `ls` has decides sorting, colouring
// or classification, which are opinions rather than the listing, and shipping
// them half-done would be worse than not shipping them.
//
// `-1` AND `-l` BIND DIFFERENT FIELDS on purpose, though `-l` implies one name
// per line: `ls -l -1` and `ls -1 -l` must both be the long form, and two
// letters writing one enum would make the LAST one win - which is exactly the
// tie-break `-L`/`-P` want and exactly the wrong one here.
struct ls_opts {
	bool all = false;        // -a
	bool long_form = false;  // -l
	bool one_per_line = false;  // -1
};

constexpr auto kLs = args::spec<ls_opts>(
	args::option{'a', args::field<&ls_opts::all>}
		.help("list entries whose names begin with a dot"),
	args::option{'l', args::field<&ls_opts::long_form>}
		.help("write the long form: mode, links, owner, group, size and time"),
	args::option{'1', args::field<&ls_opts::one_per_line>}
		.help("write one name per line"));

// One thing to be listed: the name as it will be printed, and the path to stat.
// They differ for an operand (`ls /tmp/x` prints `/tmp/x`) and agree inside a
// directory (`ls /tmp` prints `x`), which is what makes one struct enough for
// both.
struct listing_entry {
	std::string name;
	std::string path;
};

// `drwxr-xr-x`. Written into a caller's buffer because it is called once per
// entry and a returned `std::string` per row is the allocation this file can
// most easily avoid.
void mode_string(mode_t mode, char out[11]) noexcept {
	out[0] = S_ISDIR(mode)  ? 'd'
	       : S_ISLNK(mode)  ? 'l'
	       : S_ISCHR(mode)  ? 'c'
	       : S_ISBLK(mode)  ? 'b'
	       : S_ISFIFO(mode) ? 'p'
	       : S_ISSOCK(mode) ? 's'
	                        : '-';
	static constexpr mode_t bits[9] = {S_IRUSR, S_IWUSR, S_IXUSR,
	                                   S_IRGRP, S_IWGRP, S_IXGRP,
	                                   S_IROTH, S_IWOTH, S_IXOTH};
	static constexpr char letters[9] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
	for (int i = 0; i < 9; ++i)
		out[i + 1] = (mode & bits[i]) != 0 ? letters[i] : '-';
	// setuid, setgid and the sticky bit overwrite the execute column they modify,
	// which is what every `ls` does and what makes `rws` readable at a glance.
	if ((mode & S_ISUID) != 0)
		out[3] = (mode & S_IXUSR) != 0 ? 's' : 'S';
	if ((mode & S_ISGID) != 0)
		out[6] = (mode & S_IXGRP) != 0 ? 's' : 'S';
	if ((mode & S_ISVTX) != 0)
		out[9] = (mode & S_IXOTH) != 0 ? 't' : 'T';
	out[10] = '\0';
}

// The owner's name, or the number when there is none. The passwd and group
// databases are looked up per entry and not cached: a directory listing is
// bounded by what a person will read, and `getpwuid` is already cached by libc.
void owner_name(uid_t uid, std::string& into) {
	if (const struct passwd* pw = ::getpwuid(uid); pw != nullptr && pw->pw_name != nullptr)
		into.assign(pw->pw_name);
	else
		into.assign(std::to_string(static_cast<long long>(uid)));
}

void group_name(gid_t gid, std::string& into) {
	if (const struct group* gr = ::getgrgid(gid); gr != nullptr && gr->gr_name != nullptr)
		into.assign(gr->gr_name);
	else
		into.assign(std::to_string(static_cast<long long>(gid)));
}

// THE TERMINAL'S WIDTH, or 80.
//
// `TIOCGWINSZ` is the only honest source - `$COLUMNS` is the shell's own idea
// and a builtin that read it would disagree with the pipe it is writing into -
// and 80 is the floor every terminal has had since the VT100, which is what
// makes it the right answer when there is no terminal to ask.
[[nodiscard]] std::size_t terminal_width() noexcept {
	struct winsize ws{};
	if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	return 80;
}

// NO COLUMNISATION, and that is a decision rather than an omission. GNU `ls`
// lays names out down-then-across in equal columns, which needs the whole
// listing measured, a column count solved for, and a display width per name that
// is a grapheme question rather than a byte one (#108). A space-separated wrap
// needs none of that, reads the same for the short listings a shell actually
// produces, and cannot be wrong about a name with a combining mark in it.
void write_wrapped(const std::vector<listing_entry>& entries, std::size_t width) {
	std::size_t column = 0;
	for (const listing_entry& one : entries) {
		const std::size_t size = one.name.size();
		if (column != 0 && column + 1 + size > width) {
			std::fputc('\n', stdout);
			column = 0;
		} else if (column != 0) {
			std::fputc(' ', stdout);
			++column;
		}
		std::fwrite(one.name.data(), 1, size, stdout);
		column += size;
	}
	if (column != 0)
		std::fputc('\n', stdout);
}

// The long form. Widths are measured over THIS block and not over the whole
// invocation, which is what `ls -l a_dir b_dir` does: each directory's columns
// line up with each other and neither is padded to fit the other's longest name.
void write_long(const std::vector<listing_entry>& entries) {
	struct row {
		char mode[11];
		std::string links;
		std::string owner;
		std::string group;
		std::string size;
		char when[32];
		const std::string* name;
		bool stat_failed;
	};
	std::vector<row> rows;
	rows.reserve(entries.size());
	std::size_t w_links = 0, w_owner = 0, w_group = 0, w_size = 0;
	for (const listing_entry& one : entries) {
		row r{};
		r.name = &one.name;
		struct stat st{};
		// `lstat`, not `stat`: a symlink is listed as the link it is, which is what
		// makes the `l` in the mode column mean anything.
		if (::lstat(one.path.c_str(), &st) != 0) {
			r.stat_failed = true;
			rows.push_back(std::move(r));
			continue;
		}
		mode_string(st.st_mode, r.mode);
		r.links = std::to_string(static_cast<long long>(st.st_nlink));
		owner_name(st.st_uid, r.owner);
		group_name(st.st_gid, r.group);
		r.size = std::to_string(static_cast<long long>(st.st_size));
		std::tm tm{};
		const std::time_t when = st.st_mtime;
		if (::localtime_r(&when, &tm) != nullptr)
			std::strftime(r.when, sizeof r.when, "%b %e %H:%M", &tm);
		else
			r.when[0] = '\0';
		w_links = std::max(w_links, r.links.size());
		w_owner = std::max(w_owner, r.owner.size());
		w_group = std::max(w_group, r.group.size());
		w_size = std::max(w_size, r.size.size());
		rows.push_back(std::move(r));
	}
	for (const row& r : rows) {
		if (r.stat_failed) {
			// The name is still written: a listing that silently dropped an entry it
			// could not stat would be a listing that lied about the directory.
			std::printf("?????????? %s\n", r.name->c_str());
			continue;
		}
		std::printf("%s %*s %-*s %-*s %*s %s %s\n", r.mode,
		            static_cast<int>(w_links), r.links.c_str(),
		            static_cast<int>(w_owner), r.owner.c_str(),
		            static_cast<int>(w_group), r.group.c_str(),
		            static_cast<int>(w_size), r.size.c_str(),
		            r.when, r.name->c_str());
	}
}

void write_block(const std::vector<listing_entry>& entries, const ls_opts& opts) {
	if (opts.long_form) {
		write_long(entries);
		return;
	}
	// ONE NAME PER LINE IS THE DEFAULT WHEN NOBODY IS WATCHING. POSIX says the
	// multi-column form is for a terminal, and every shell script that pipes `ls`
	// depends on the other answer - `ls | while read f` is only correct because a
	// pipe gets one name per line.
	if (opts.one_per_line || ::isatty(STDOUT_FILENO) == 0) {
		for (const listing_entry& one : entries) {
			std::fwrite(one.name.data(), 1, one.name.size(), stdout);
			std::fputc('\n', stdout);
		}
		return;
	}
	write_wrapped(entries, terminal_width());
}

// Reads one directory into `into`, sorted. Returns false having reported.
[[nodiscard]] bool read_directory(const char* path, const ls_opts& opts,
                                  std::vector<listing_entry>& into) {
	DIR* const dir = ::opendir(path);
	if (dir == nullptr) {
		runtime::report("ls: %s: %s", path, std::strerror(errno));
		return false;
	}
	const std::string_view base{path};
	// `ls /` must not build `//x`, and `ls .` must build `./x` rather than `.x`.
	const bool needs_slash = !base.empty() && base.back() != '/';
	for (;;) {
		errno = 0;
		const struct dirent* entry = ::readdir(dir);
		if (entry == nullptr)
			break;
		const std::string_view name{entry->d_name};
		if (!opts.all && !name.empty() && name.front() == '.')
			continue;
		listing_entry one;
		one.name.assign(name);
		one.path.assign(base);
		if (needs_slash)
			one.path += '/';
		one.path.append(name);
		into.push_back(std::move(one));
	}
	const int failure = errno;
	::closedir(dir);
	if (failure != 0) {
		runtime::report("ls: %s: %s", path, std::strerror(failure));
		return false;
	}
	// Byte order, which is what `LC_COLLATE=C ls` gives and what every test in
	// this tree can predict. A locale-aware collation would make the output depend
	// on the environment the shell happened to start in.
	std::sort(into.begin(), into.end(),
	          [](const listing_entry& a, const listing_entry& b) { return a.name < b.name; });
	return true;
}

} // namespace

// `ls [-a] [-l] [-1] [file...]`.
//
// THE OPERAND SPLIT IS POSIX'S: everything that is not a directory is listed
// first as a block of its own, then each directory's contents, and a `name:`
// header appears before a directory only when more than one thing was named -
// so `ls dir` gives bytes and `ls dir1 dir2` says which is which.
//
// EXIT STATUS FOLLOWS coreutils: 2 for a command line this will not take, 1 for
// an operand it could not reach, 0 otherwise. The distinction matters to a
// script: a usage error means the script is wrong, and a 1 means the filesystem
// moved underneath it.
runtime::builtin_result builtin_ls(runtime::shell_state&, char** argv) {
	const auto parsed = args::parse(kLs, argv);
	if (parsed.err)
		return {runtime::report_option_error("ls", parsed.err)};
	const ls_opts& opts = parsed.opts;

	std::vector<const char*> operands;
	for (char** a = parsed.rest; *a != nullptr; ++a)
		operands.push_back(*a);
	if (operands.empty())
		operands.push_back(".");

	int status = 0;
	std::vector<listing_entry> loose;      // the operands that are not directories
	std::vector<const char*> directories;
	for (const char* name : operands) {
		struct stat st{};
		if (::stat(name, &st) != 0) {
			runtime::report("ls: %s: %s", name, std::strerror(errno));
			status = 1;
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			directories.push_back(name);
			continue;
		}
		listing_entry one;
		one.name.assign(name);
		one.path.assign(name);
		loose.push_back(std::move(one));
	}

	bool wrote_a_block = false;
	if (!loose.empty()) {
		std::sort(loose.begin(), loose.end(),
		          [](const listing_entry& a, const listing_entry& b) { return a.name < b.name; });
		write_block(loose, opts);
		wrote_a_block = true;
	}
	// The header is owed when the command line named more than one thing, counting
	// the loose files as one block - which is what makes `ls file dir` label the
	// directory and `ls dir` not.
	const bool headers = operands.size() > 1;
	for (const char* path : directories) {
		std::vector<listing_entry> entries;
		if (!read_directory(path, opts, entries)) {
			status = 1;
			continue;
		}
		if (headers) {
			std::printf(wrote_a_block ? "\n%s:\n" : "%s:\n", path);
		}
		write_block(entries, opts);
		wrote_a_block = true;
	}
	return {status};
}

} // namespace lesh::leshnici::coreutils
