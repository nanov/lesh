#pragma once

// Variables, scopes, aliases and process-wide shell state.
// Redesigned by issue #12; the ownership question is issue #13.
#include "legacy/ast.h"
#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class lesh_scope_variable {
	public:
	virtual ~lesh_scope_variable() = default;
	virtual const char* value(const std::string_view& key) const = 0;
		virtual void set(const std::string_view& key, std::string_view& value) = 0;
};
class lesh_scope_map_variable: public lesh_scope_variable {
private:
	std::unordered_map<std::string, const char*, transparent_string_hash, std::equal_to<>> _data;
public:
	~lesh_scope_map_variable() override {
		for (const auto &it : _data)
			free(const_cast<char*>(it.second));
	}
	const char* value(const std::string_view& key) const override {
		if (const auto &it = _data.find(key); it != _data.end()) {
			if (it->second == nullptr)
				return "";
			return it->second;
		}
		return "";
	}
	void set(const std::string_view& key, std::string_view& value) override {
		_data[std::string(key)] = strdup(value.data());
	}
	size_t size() const { return _data.size(); }
};
class lesh_scope_array_variable: public lesh_scope_variable {
private:
	std::vector<const char *> _data;
	bool try_get_size(const char* t, size_t& res) const {
		char* p = const_cast<char*>(t);
		char c = *p;
		size_t i = 0;
		res = 0;
		while (c != '\0') {
			if (!isdigit(c))
				return false;
			res += (c - '0')*(i++);
			c = *++p;
		}
		return true;
	}
public:
	lesh_scope_array_variable() : lesh_scope_variable() {}
	void push(const char* value) {
		_data.push_back(value);
	}

	const char* value(const std::string_view& key) const override {
		size_t i = 0;
		if (!try_get_size(key.data(), i))
			return "";
		if (i >= _data.size())
			return "";
		const auto v = _data[i];
		if (v == nullptr)
			return "";
		return v;
	}
	void set(const std::string_view& key, std::string_view& value) override {
		size_t i = 0;
		if (!try_get_size(key.data(), i))
			return;
		if (i >= _data.capacity())
			_data.reserve(i+1);
		if (i >= _data.size())
			for (auto c = i; c < _data.size(); c++)
				_data.emplace_back(nullptr);
		_data[i] = const_cast<char*>(value.data());
	}
	size_t size() const { return _data.size(); }
};
class lesh_state_scope {
private:
		const lesh_state_scope* _parent;
		const lesh_state_scope* _exporting_parent;
		const bool _in_export_mode = false;
		std::unordered_map<const std::string, std::unique_ptr<lesh_scope_variable>, transparent_string_hash, std::equal_to<>> _vars;

public:
	lesh_state_scope(lesh_state_scope* parent, lesh_state_scope* exporting_parent) : _parent(parent), _exporting_parent(exporting_parent) {}


	bool try_get_var_value(const std::string_view& var, const std::string_view& key, std::string_view& val) const {
		auto s = this;
		while (s) {
			if (const auto &it = s->_vars.find(var); it != s->_vars.end()) {
				val = it->second->value(key);
				return true;
			}
			s = s->_parent;
		}
		return false;
	}

	bool get_map_values(char* v, size_t len, std::function<void(std::string_view, std::string_view)> callback) const {
		size_t i = 0;
		char* c = const_cast<char*>(v);
		char* lw = c;
		size_t wl = 0;
		std::string_view k;
		char mode = 0;
		for (i = 0; *c != ')' && i < len; c++, i++) {
			if (isspace(*c)) {
				if (mode == 0) {
					lw++;
					continue;
				}

				if (mode == 1 || mode == 2)
					return false;

				if (mode == 3) {
					*c='\0';
					callback(k, std::string_view(lw, wl));
					mode = 0;
				}
			}
			else if (*c == '[') {
				if (mode == 0) {
					mode = 1;
					lw = c + 1;
					wl = 0;
				}
			} else if (*c == ']') {
				if (mode == 1) {
					*c= '\0';
					k = std::string_view(lw, wl);
					mode = 2;
				}
			} else if (*c == '=') {
				if (mode == 2) {
					mode = 3;
					wl = 0;
					lw = c+1;
				}
			} else {
				wl++;
			}
		}
		if (*c != ')')
			return false;

		*c = '\0';
		if (mode== 1 || mode == 2) {
			callback(k, std::string_view());
		} else if (mode == 3) {
			callback(k, std::string_view(lw));
		}
		return true;
	}
	bool get_array_values(char* v, size_t len, std::function<void(char*, size_t)> callback) const {
		size_t i = 0;
		char* c = const_cast<char*>(v);
		char* lw = c;
		size_t word_len = 0;
		for (i = 0; *c != ')' && i < len; c++, i++) {
			if (isspace(*c)) {
				if (word_len == 0) {
					lw++;
					continue;
				}
				*c = '\0';
				callback(lw, word_len);
				lw = c+1;
				word_len = 0;
			} else {
				word_len++;
			}
		}
		if (*c != ')')
			return false;
		if (word_len>0) {
			*c = '\0';
			callback(lw, word_len);
		}
		return true;
	}

	void typeset_map(const std::string_view& key, const std::string_view& val) {
		char* v = const_cast<char*>(val.data());
		auto s =std::make_unique<lesh_scope_map_variable> ();
		if (*v == '(') {
			auto p = get_map_values(v+1, val.size()-1, [s=s.get()](auto k, auto v) {
				s->set(k, v);
			});
		}
		_vars.emplace(key, std::move(s));
	}
	void typeset(const std::string_view& key, const std::string_view& val) {
		char* v = const_cast<char*>(val.data());
		if (*v == '(') {// array
			auto s =std::make_unique<lesh_scope_array_variable> ();
			auto p = get_array_values(v+1, val.size()-1, [s = s.get()](const char * word, size_t len) {
				auto const ws = new char[len+1];
				memcpy(ws, word, len+1);
				s->push(ws);
			});
			if (p)
				_vars.emplace(key, std::move(s));
		} else { // env variable

		}
	}
};
class lesh_state {
	// https://gist.github.com/ClementNerma/1dd94cb0f1884b9c20d1ba0037bdcde2

private:
	alias_container _aliases;
	buffer_pool _buffer_pool;
	buffer_pool _global_pool;
	lesh_state_scope _lesh_scope;

	char** _envp;
	std::filesystem::path _pwd;
	std::string _display_pwd;
	std::string prompt;
	std::vector<std::filesystem::path> _path_env;
	std::string _home;
	std::unordered_map<std::string_view, std::string_view> _env;

public:
	lesh_state(std::filesystem::path current_path, char** envp) noexcept : _lesh_scope(nullptr, nullptr), _envp(envp), _buffer_pool(BUFFER_POOL_SIZE), _global_pool(0), _aliases() {
		for (auto it = _envp; *it; it++) {
			const auto e = *it;
			std::string_view v = {e};
			const auto idx = v.find('=');
			_env.emplace(v.substr(0, idx), (e +idx + 1));
		}

		load_home_directory();
		set_path(current_path);
		load_env_path();
	}

	[[nodiscard]] const char* pmt() const noexcept { return prompt.c_str(); };
	[[nodiscard]] const std::filesystem::path& pwd() const noexcept { return _pwd; }
	[[nodiscard]] const std::string& home() const noexcept { return _home; }
	[[nodiscard]] const std::string& display_pwd() const noexcept { return _display_pwd; }
	[[nodiscard]] const std::vector<std::filesystem::path>& path_env() const noexcept { return _path_env; }

	buffer_pool& global_pool() noexcept { return _global_pool; }
	buffer_pool& buffer_pool() noexcept { return _buffer_pool; }


	size_t adjust_home(std::string& input, size_t from) {
		size_t added = 0;
		auto portion =  std::string_view(input).substr(from);
		size_t n = 0;
		while((n = portion.find("~", n)) != std::string_view::npos) {
			input.replace(from + n, 1, _home);
			added += _home.size() - 1;
			portion = std::string_view(input).substr(from);
			n += _home.size();
		}
		return  added;
	}

	size_t adjust_home(std::string& input, size_t from, size_t len) {
		size_t added = 0;
		auto portion =  std::string_view(input).substr(from, len);
		size_t n = 0;
		while((n = portion.find("~", n)) != std::string_view::npos) {
			input.replace(from + n, 1, _home);
			len = len - 1 + _home.size();
			added += _home.size() - 1;
			n++;
			portion = std::string_view(input).substr(from, len);
		}
		return  added;
	}

	void adjust_home(std::string &input) noexcept {
		while (true) {
				std::string::size_type n = 0;
				while((n = input.find("~", n)) != std::string::npos) {
						input.replace(n, 1, _home);
				    n+=1;
			 	}
		}
	}

	void set_path() {
		std::filesystem::path h = _home;
		set_path(h);
	}

	void set_path(std::filesystem::path& p) {
		if (!p.compare(_pwd))
			return;

		_pwd = p;
		_display_pwd = std::string(p);
		auto h = _home;
		if (_display_pwd.starts_with(h))
			_display_pwd.replace(0, h.size(), "~");
		prompt = std::string(_display_pwd);
		prompt.append(" > ");
	}

	bool try_get_env(std::string_view key, std::string_view index, std::string_view& val) const {
		if (index.empty())
			return try_get_env(key, val);
		return _lesh_scope.try_get_var_value(key, index, val);
	}
	bool try_get_env(std::string_view key, std::string_view& val) const {
		if (auto it = _env.find(key); it != _env.end()) {
			val = it->second;
			return true;
		}
		return false;
	}

	lesh_state_scope* scope() noexcept { return &_lesh_scope; }

	bool try_get_alias(const char* key, ASTPipe const ** val) const {
		return _aliases.try_get_alias(key, val);
	}

	void normalize_aliases() {
		_aliases.normalize_aliases();
	}
	ASTPipe& emplace_alias(const char* key) {
		return _aliases.emplace_alias(key);
	}
	// void emplace_alias_o(const char* key, const ASTPipe& val, bool normalize_after = false) {
	// 	_aliases.emplace_alias(key, val);
	// 	if (normalize_after)
	// 		_aliases.normalize_aliases();
	// }

private:
	void load_env_path() {
		_path_env.clear();
		std::string_view p;
		if (!try_get_env("PATH", p))
			return;

		_path_env.reserve(20);

		// TODO: use strstr
		const auto pa = std::string(p);
		size_t pr = 0;
		size_t in = pa.find(':');
		while(in != std::string::npos) {
			auto pd = pa.substr(pr, in - pr);
			_path_env.push_back(pd);
			pr = in+1;
			in = pa.find(':', pr);
		}
		_path_env.push_back(pa.substr(pr));
	}

	void load_home_directory() {
		std::string_view home_dir;
		if (!try_get_env("HOME", home_dir))
			home_dir = "/";
		_home = std::string(home_dir);
	}
};
