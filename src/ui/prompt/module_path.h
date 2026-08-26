#pragma once

// One built-in prompt module (#157, spec §6.10): the class and its one object.
// See `module.h` for the vocabulary and `modules.h` for the table these sit in.

#include "ui/prompt/module.h"

#include <cstdint>
#include <string_view>

namespace lesh::ui::prompt {

// --- path ------------------------------------------------------------------

struct path_params {
	enum class kind : std::uint8_t {
		relative,   // `$HOME`-contracted, full path outside home. The default.
		absolute,   // uncontracted
		short_,     // the last component only
		initials,   // every component but the last cut to its first byte
		unvowel,    // every component but the last with its vowels removed
	};

	kind which = kind::relative;
};

// Whether `pwd` is `home` or lives under it, BY COMPONENT: `/home/user`
// contracts under `/home/user`, and so does `/home/user/src`, but
// `/home/username` does not. The cheap `starts_with` would have turned the third
// into `~name`, which is a path that does not exist.
[[nodiscard]] constexpr bool under_home(std::string_view pwd, std::string_view home) noexcept {
	return !home.empty() && pwd.size() >= home.size() && pwd.substr(0, home.size()) == home
	       && (pwd.size() == home.size() || pwd[home.size()] == '/');
}

[[nodiscard]] constexpr bool is_ascii_vowel(char one) noexcept {
	switch (one) {
		case 'a': case 'e': case 'i': case 'o': case 'u':
		case 'A': case 'E': case 'I': case 'O': case 'U':
			return true;
		default:
			return false;
	}
}

// The component-reducing forms, over the CONTRACTED path without ever building
// it. `head` is `~` or empty and `rest` is what follows; the two are read as one
// logical string through `byte`, because materializing the join would be a
// `std::string` on the render path for no answer it does not already have.
//
// THE LAST COMPONENT IS ALWAYS WHOLE. That is the entire point of both forms:
// `~/p/g/lesh` and `~/prvt/gthb/lesh` tell you where you are while giving the
// width back to the shell, and a directory whose own name has been eaten tells
// you nothing.
constexpr void emit_reduced_path(sink& out, std::string_view head, std::string_view rest,
                                 bool initials) {
	const std::size_t total = head.size() + rest.size();
	const auto byte = [&](std::size_t i) {
		return i < head.size() ? head[i] : rest[i - head.size()];
	};

	// Where the last component starts: one past the last `/`, or the beginning.
	std::size_t last_start = 0;
	for (std::size_t i = 0; i < total; ++i)
		if (byte(i) == '/')
			last_start = i + 1;

	std::size_t i = 0;
	while (i < total) {
		if (byte(i) == '/') {
			out.append_byte('/');
			++i;
			continue;
		}

		std::size_t end = i;
		while (end < total && byte(end) != '/')
			++end;

		if (i == last_start) {
			for (std::size_t k = i; k < end; ++k)
				out.append_byte(byte(k));
		} else if (initials) {
			out.append_byte(byte(i));
		} else {
			bool wrote = false;
			for (std::size_t k = i; k < end; ++k)
				if (!is_ascii_vowel(byte(k))) {
					out.append_byte(byte(k));
					wrote = true;
				}
			// A COMPONENT THAT WOULD VANISH KEEPS ITS FIRST BYTE. `~/aeiou/x`
			// reducing to `~//x` would be a path that reads as a mistake; one
			// letter is the smallest honest answer.
			if (!wrote)
				out.append_byte(byte(i));
		}
		i = end;
	}
}

class module_path final : public typed_module<path_params> {
public:
	[[nodiscard]] constexpr std::string_view name() const noexcept override { return "path"; }

protected:
	constexpr bool parse(std::string_view type, path_params& out,
	                     parse_error& err) const override {
		using kind = path_params::kind;
		if (type.empty() || type == "relative" || type == "r") { out.which = kind::relative; return true; }
		if (type == "absolute" || type == "a" || type == "f")  { out.which = kind::absolute; return true; }
		if (type == "short" || type == "s")                    { out.which = kind::short_; return true; }
		if (type == "initials" || type == "i")                 { out.which = kind::initials; return true; }
		if (type == "unvowel" || type == "u")                  { out.which = kind::unvowel; return true; }

		err.at = 0;
		err.length = type.size();
		err.what = ": unknown variant";
		return false;
	}

	constexpr int render(const state& facts, const path_params& params, sink& out) const override {
		if (facts.pwd.empty())
			return code(element_status::omitted);

		if (params.which == path_params::kind::absolute) {
			out.append(facts.pwd);
			return code(element_status::ready);
		}

		const bool home = under_home(facts.pwd, facts.home);
		const std::string_view head = home ? std::string_view{"~"} : std::string_view{};
		const std::string_view rest = home ? facts.pwd.substr(facts.home.size()) : facts.pwd;

		switch (params.which) {
			case path_params::kind::relative:
				out.append(head);
				out.append(rest);
				break;
			case path_params::kind::short_: {
				// The last component of the CONTRACTED path, so that home itself is
				// `~` rather than the name of the directory it happens to live in.
				const std::size_t slash = rest.rfind('/');
				if (slash == std::string_view::npos)
					out.append(rest.empty() ? head : rest);
				else if (slash + 1 == rest.size())
					// A trailing slash: the root, or a path somebody wrote with one.
					out.append("/");
				else
					out.append(rest.substr(slash + 1));
				break;
			}
			case path_params::kind::initials:
				emit_reduced_path(out, head, rest, true);
				break;
			case path_params::kind::unvowel:
				emit_reduced_path(out, head, rest, false);
				break;
			case path_params::kind::absolute:
				break;   // answered above
		}
		return code(element_status::ready);
	}
};

// The one object, for the whole process - see `modules.h`.
inline constexpr module_path kModulePath{};

} // namespace lesh::ui::prompt
