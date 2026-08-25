#pragma once

// A scratch directory unique to one test. See issue #60.
//
// tests/unit/ used to build capture and script paths as
// `::testing::TempDir() + "<fixed name>"` - a constant filename in the SHARED
// system temp directory. Two `lesh_tests` processes running at once, one per
// parallel agent's worktree, therefore wrote, read and unlinked each other's
// files: worktree isolation gives each agent its own source, build directory and
// git index, but not its own `/var/folders/.../T/`.
//
// mkdtemp(3) makes a directory whose name no other process can be handed, so a
// site built from one of these cannot collide with a concurrent run no matter
// what it names inside it. Shared rather than reimplemented at each of the 25
// call sites, so site 26 is correct by construction instead of one more copy
// someone has to remember to qualify.
//
// Two things macOS gets particular about, both handled here so nobody re-learns
// them at a call site: ::testing::TempDir() already ends in a slash - callers of
// dir()/file() must not add another - and it is itself reached through a symlink
// (/var -> /private/var), so the directory is canonicalized once, up front, and
// every path this hands out is already in its resolved form.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace lesh::testing {

class temp_path {
public:
	temp_path() {
		std::string pattern = ::testing::TempDir() + "lesh_test_XXXXXX";
		if (::mkdtemp(pattern.data()) == nullptr)
			throw std::runtime_error("temp_path: mkdtemp failed");
		// canonical(), because ::testing::TempDir() is itself reached through a
		// symlink on macOS (/var -> /private/var). A path built from the
		// un-resolved form would not compare equal to one the kernel, or
		// std::filesystem, later hands back resolved.
		_dir = std::filesystem::canonical(pattern).string();
	}

	temp_path(const temp_path&) = delete;
	temp_path& operator=(const temp_path&) = delete;

	~temp_path() {
		// Tolerant of a directory that is already gone: a test that removed its
		// own files, or one that failed an ASSERT partway through, must still
		// reach here without a second failure on top of the first.
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	// The directory itself - already canonical, with no trailing slash.
	[[nodiscard]] const std::string& dir() const { return _dir; }

	// A path for `name` inside the directory. Does not create anything; the
	// caller opens, redirects onto, or mkdir's it as the test needs.
	[[nodiscard]] std::string file(std::string_view name) const {
		return _dir + "/" + std::string{name};
	}

private:
	std::string _dir;
};

} // namespace lesh::testing
