/*
 *

      ..               .x+=:.
x .d88"               z`    ^%    .uef^"
 5888R                   .   <k :d88E
 '888R        .u       .@8Ned8" `888E
  888R     ud8888.   .@^%8888"   888E .z8k
  888R   :888'8888. x88:  `)8b.  888E~?888L
  888R   d888 '88%" 8888N=*8888  888E  888E
  888R   8888.+"     %8"    R88  888E  888E
  888R   8888L        @8Wou 9%   888E  888E
 .888B . '8888c. .+ .888888P`    888E  888E
 ^*888%   "88888%   `   ^"F     m888N= 888>
   "%       "YP'                 `Y"   888
                                      J88"
                                      @%
                                    :"
*/
	/*
┓   ┓ 
┃┏┓┏┣┓
┗┗ ┛┛┗
	*/


#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <sys/unistd.h>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utils.h"
#include "zsh_parser_plus.h"

#include "replxx.hxx"
#include <sol/sol.hpp>

/*
const std::optional<std::string> file_found(const std::vector<std::filesystem::path> &paths, const std::string_view &file) noexcept{
	for (auto p:paths) {
		auto ap = p / file;
		if (std::filesystem::exists(ap))
			return std::string(ap);
	}
	return std::nullopt;
}
*/

// end tools




#include <replxx.h>


#include "lolcat.h"

void print_lesh(double gradient = 0.6) {
	std::stringstream ss;
	ss << "      ..               .x+=:." << std::endl;
	ss << "x .d88\"               z`    ^%    .uef^\"" << std::endl;
	ss << " 5888R                   .   <k :d88E" << std::endl;
	ss << " '888R        .u       .@8Ned8\" `888E" << std::endl;
	ss << "  888R     ud8888.   .@^%8888\"   888E .z8k" << std::endl;
	ss << "  888R   :888'8888. x88:  `)8b.  888E~?888L" << std::endl;
	ss << "  888R   d888 '88%\" 8888N=*8888  888E  888E" << std::endl;
	ss << "  888R   8888.+\"     %8\"    R88  888E  888E" << std::endl;
	ss << "  888R   8888L        @8Wou 9%   888E  888E" << std::endl;
	ss << " .888B . '8888c. .+ .888888P`    888E  888E" << std::endl;
	ss << " ^*888%   \"88888%   `   ^\"F     m888N= 888>" << std::endl;
	ss << "   \"%       \"YP'                 `Y\"   888" << std::endl;
	ss << "                                      J88\"" << std::endl;
	ss << "  v0.01α                              @%" << std::endl;
	ss << "  <nanov/>                          :\"" << std::endl;
	lolfilter(ss, gradient);
}

// NOTICE : the wgole hint system is flawed in replxx - replace with something non allocating
std::vector<std::string> _hint_callback_results = {};
std::vector<std::string> hint_callback(replxx::Replxx& replxx, const std::string &input, int& contextLen, replxx::Replxx::Color &color) {
	_hint_callback_results.clear();
	auto hs = replxx.history_scan();
	while (hs.next()) {
		if (auto& e = hs.get().text();e != input && e.starts_with(input))
			_hint_callback_results.push_back(e);
	}
	color = replxx::Replxx::Color::GRAY;
	return _hint_callback_results;
}

class lesh_lua_api {
	public:
		lesh_lua_api(lesh_state& lesh_state, ZshParserPlus::Parser& parser): _lesh_state(lesh_state), _parser(parser) {}
		void init(sol::state& lua, sol::environment& env) {
			env.create_named("lesh",
			// lua.create_named_table("lesh",
				"set_alias",  [&](const char* k, const char* v) { _parser.set_alias(k, v); },
				"set_alias_lazy",  [&](const char* k, const char* v) { _parser.set_alias_lazy(k, v); },
				"echo",  [&](const char* k) { printf("%s\n", k ); });
		}
private:
	lesh_state &_lesh_state;
	ZshParserPlus::Parser& _parser;
};

int main(int argc, char **argv, char **envp) {
	_hint_callback_results.reserve(10);
	setenv("TERM", "xterm-256color", 1);


	int exit_code = -1;
	// Flush after every std::cout / std:cerr
	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;

	lesh_state state {std::filesystem::current_path(), envp};

	print_lesh();


	auto zsh_parser = ZshParserPlus::Parser(state);
	zsh_parser.init_aliases();

	replxx::Replxx rx;
	rx.bind_key_internal(replxx::Replxx::KEY::UP,  "history_previous");
	rx.bind_key_internal(replxx::Replxx::KEY::DOWN,  "history_next");
	rx.bind_key_internal(replxx::Replxx::KEY::meta(replxx::Replxx::KEY::DOWN),  "hint_previous");
	rx.bind_key_internal(replxx::Replxx::KEY::meta(replxx::Replxx::KEY::UP),  "hint_next");
	rx.set_hint_callback([&](const std::string &input, int& contextLen, replxx::Replxx::Color &color) { return hint_callback(rx, input, contextLen, color); });
	rx.set_max_hint_rows(1);
	std::string history_file = ".lesh_history";
	rx.history_load(history_file);
	std::vector<const char*> p = {"mitko"};
	const char* p2[] = {"mitko", nullptr};

	/*
	// Lua shit!
	auto lua_a = lesh_lua_api(state, zsh_parser);
	auto lua_env = sol::environment(lua, sol::create);
	lua_a.init(lua, lua_env);
	{
		// auto lua_rc = lua.load_file("leshrc.lua");
		auto lua_rc = lua.script_file("leshrc.lua", lua_env, [](lua_State*, sol::protected_function_result pfr) {
			std::cerr << sol::error(pfr).what() << std::endl;
			return pfr;
		});
		if (!lua_rc.valid()) {
			sol::error err{lua_rc};
			std::cerr << err.what() << std::endl;
		}
		auto f = lua_env.get<std::optional<sol::function>>("show");
		if (f.has_value()) {
			f.value()(sol::as_args(p2));
		}
		lua.script("show('hello')", lua_env);
	}
	*/



	while(true) {
		// display the prompt and retrieve input from the user
		char const* cinput{ nullptr };
		// break;

		do {
			cinput = rx.input(state.pmt());
		} while ( ( cinput == nullptr ) && ( errno == EAGAIN ) );

		if (cinput == nullptr) {
			break;
		}

		// TODO: prevent copy
		std::string input = {cinput};

		if (input.empty())
			continue;
		rx.history_add(cinput);

		if (input == "exit") {
			exit_code = 0;
			break;
		}

		zsh_parser.parse_and_execute(input);
	}

	rx.history_sync(history_file);
	return exit_code;
}