#pragma once

// The host-side fakes more than one `ui_*` suite needs, in one place.
//
// The same rule `temp_path.h` was written under: a fake copied into a second
// file is a fake that can drift, and the copies here HAD drifted in their
// comments while their code stayed byte-identical - which is the worst of both,
// two things to read and one thing to maintain. What lives here is exactly what
// two or more suites use; a fake with one client stays beside its client, where
// the reader is.
//
// Everything is a class or a function template, so this header adds no
// translation-unit state and no ordering rule.
//
// WHERE TWO COPIES DIFFERED, THE UNION IS HERE. Only `fake_tty` did: the loop
// suite's copy had `close_input`, which is how a test spells a hangup, and the
// proposal suite's did not. Nothing else differed in code - only in the comments
// above it, which is exactly the drift a shared copy prevents.

#include "leshper/registry.h"
#include "leshper/state.h"
#include "ui/history_search.h"
#include "ui/loop.h"
#include "ui/shell_knowledge.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lesh::testing {

// A `shell_knowledge` that is a map, which is what a test has.
//
// It counts its calls, because the memo's whole claim - a name repeated on one
// line is walked once - is a claim about how often this gets asked.
class fake_knowledge final : public ui::shell_knowledge {
public:
	void define(std::string name, ui::command_kind kind) {
		_names.insert_or_assign(std::move(name), kind);
	}

	void set_path(std::string value) {
		_path = std::move(value);
		_has_path = true;
	}
	void unset_path() noexcept { _has_path = false; }

	[[nodiscard]] ui::command_kind classify(std::string_view name) const override {
		asked.push_back(std::string{name});
		const auto found = _names.find(name);
		return found == _names.end() ? ui::command_kind::unknown : found->second;
	}

	[[nodiscard]] bool path(std::string_view& out) const override {
		++path_reads;
		if (!_has_path)
			return false;
		out = _path;
		return true;
	}

	// Every name this was asked about, in order.
	mutable std::vector<std::string> asked;
	mutable int path_reads = 0;

private:
	std::map<std::string, ui::command_kind, std::less<>> _names;
	std::string _path;
	bool _has_path = false;
};

// `$PATH` for the duration of one test, restored - unset included - afterwards.
//
// Only the tests that let the no-shell-attached fallback touch `$PATH` need it;
// everything that goes through a `shell_knowledge` is indifferent to it. It is
// still process-global state, so it is an RAII object rather than two calls.
class scoped_env_path {
public:
	explicit scoped_env_path(const char* value) {
		if (const char* old = ::getenv("PATH")) {
			_had = true;
			_old = old;
		}
		::setenv("PATH", value, 1);
	}
	~scoped_env_path() {
		if (_had)
			::setenv("PATH", _old.c_str(), 1);
		else
			::unsetenv("PATH");
	}

	scoped_env_path(const scoped_env_path&) = delete;
	scoped_env_path& operator=(const scoped_env_path&) = delete;

private:
	bool _had = false;
	std::string _old;
};

// A pipe standing in for a terminal, as everywhere else in the host's tests:
// never the process's own tty. Non-blocking on the read ends, because the loop
// polls before reading and a test that got the poll wrong should fail rather
// than hang the suite.
class fake_tty {
public:
	fake_tty() {
		[&] { ASSERT_EQ(::pipe(_in), 0); }();
		[&] { ASSERT_EQ(::pipe(_out), 0); }();
		::fcntl(_in[0], F_SETFL, O_NONBLOCK);
		::fcntl(_out[0], F_SETFL, O_NONBLOCK);
	}
	~fake_tty() {
		for (int fd : {_in[0], _in[1], _out[0], _out[1]})
			if (fd >= 0)
				::close(fd);
	}

	fake_tty(const fake_tty&) = delete;
	fake_tty& operator=(const fake_tty&) = delete;

	[[nodiscard]] ui::loop_fds fds() const { return ui::loop_fds{_in[0], _out[1]}; }

	void type(std::string_view bytes) const {
		ASSERT_EQ(::write(_in[1], bytes.data(), bytes.size()),
		          static_cast<ssize_t>(bytes.size()));
	}

	// Closing the write end is a HANGUP on the far side, which is how a test
	// spells "the terminal went away".
	void close_input() {
		if (_in[1] >= 0) {
			::close(_in[1]);
			_in[1] = -1;
		}
	}

	// Everything the loop has written since the last call.
	[[nodiscard]] std::string painted() const {
		std::string all;
		char chunk[4096];
		for (;;) {
			const ssize_t n = ::read(_out[0], chunk, sizeof(chunk));
			if (n <= 0)
				break;
			all.append(chunk, static_cast<std::size_t>(n));
		}
		return all;
	}

private:
	int _in[2]{-1, -1};
	int _out[2]{-1, -1};
};

// A history that supersedes the request part-way through its own walk, so the
// cooperative poll has something to notice. The real trigger is the user typing
// while a worker is thinking; here it is the second entry.
class superseding_source final : public ui::history_source {
public:
	superseding_source(leshper::loop_harness& loop, std::vector<std::string> entries)
		: _loop(&loop), _entries(std::move(entries)) {}

	void for_each_newest_first(
		const std::function<bool(std::string_view)>& fn) const override {
		std::size_t seen = 0;
		for (auto it = _entries.rbegin(); it != _entries.rend(); ++it) {
			if (++seen == 2)
				_loop->supersede();
			if (!fn(*it))
				return;
		}
	}

private:
	leshper::loop_harness* _loop;
	std::vector<std::string> _entries;
};

} // namespace lesh::testing
