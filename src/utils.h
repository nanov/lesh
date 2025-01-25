#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

#define BUFFER_POOL_SIZE (1024*32)
#define SUBSHELL_BUFFER_INITIAL_SIZE 1024

#define VERBOSE_POOL_DATA

// begin - alias shit double hash == https://www.reddit.com/r/cpp_questions/comments/12xw3sn/find_stdstring_view_in_unordered_map_with/ == https://godbolt.org/z/789xv8Eeq
template<typename ... Bases>
struct overload : Bases ...{
    using is_transparent = void;
    using Bases::operator() ... ;
};

struct string_part {
private:
	char* _data = nullptr;
	size_t _size = 0;
public:
	string_part(char* data, size_t size) : _data(data), _size(size) {}
};

class buffer_pool {
	public:
		explicit buffer_pool(size_t size = 1024) : _data(new char[size]) {
			_end = _data + size;
			_current = _data;
		}

		~buffer_pool() {
			delete[] _data;
		}

		bool reallocate(char* at, size_t new_size, char*& result) {
			if (at+new_size > _end) {
#ifdef VERBOSE_POOL_DATA
				lifetime_pool_bytes_allocated += new_size;
#endif
				result = static_cast<char *>(malloc(sizeof(char) * new_size));
				memcpy(result, at, _current - at);
				_current = at;
				return false;
			}
			_current = at + new_size;
			result = at;
#ifdef VERBOSE_POOL_DATA
				lifetime_pool_bytes_allocated += _current - at;
#endif
			return true;
		}
		bool allocate(size_t size, char*& result) {
			if (_current+size > _end) {
#ifdef VERBOSE_POOL_DATA
				lifetime_pool_bytes_allocated += size;
#endif
				result = new char[size];
				return false;
			}
#ifdef VERBOSE_POOL_DATA
			lifetime_pool_bytes_used += size;
#endif
			result = _current;
			_current += size;
			return true;
		}
		void reset(char* to) {
			_current = to;
		}

		char* at() const { return _current; }

#ifdef VERBOSE_POOL_DATA
	  size_t lifetime_pool_bytes_used;
		size_t lifetime_pool_bytes_allocated;
		[[nodiscard]] size_t bytes_used() const {
			return reinterpret_cast<size_t>(_current) - reinterpret_cast<size_t>(_data);
		}
		[[nodiscard]] size_t pool_size() const {
			return reinterpret_cast<size_t>(_end) - reinterpret_cast<size_t>(_data);
		}
#endif
	private:
		char* _data;
		char* _current;
		const char* _end;
};

struct char_pointer_hash{
    auto operator()( const char* ptr ) const noexcept{ return std::hash<std::string_view>{}( ptr );}
};

using transparent_string_hash = overload<
    std::hash<std::string>,
    std::hash<std::string_view>,
    char_pointer_hash
>;

class alias_container {
		using alias_map = std::unordered_map<std::string, std::string, transparent_string_hash, std::equal_to<>>;

	private:
			alias_map _aliases;

	public:
		alias_container(std::initializer_list<std::pair<const std::string, std::string>> l) noexcept : _aliases(alias_map(l)) {
		}

		void define_alias(std::string from, std::string to) {
			_aliases[from] = to;
		}


		alias_container() noexcept {
			_aliases = {};
			_aliases.reserve(10);
		}
		
		bool try_get_expansion(const char* word, std::string& expansion) const {
			if (auto it = _aliases.find(word); it != _aliases.end()) {
				expansion = it->second;
				return true;
			}
			return false;
		}
		bool try_get_expansion(const std::string_view& word, std::string& expansion) const {
			if (auto it = _aliases.find(word); it != _aliases.end()) {
				expansion = it->second;
				return true;
			}
			return false;
		}
};

class lesh_state {
private:
	buffer_pool _buffer_pool;
	buffer_pool _global_pool;

	char** _envp;
	std::filesystem::path _pwd;
	std::string _display_pwd;
	std::string prompt;
	std::vector<std::filesystem::path> _path_env;
	std::string _home;
	alias_container _aliases;
	std::unordered_map<std::string_view, std::string_view> _env;

public:
	lesh_state(std::filesystem::path current_path, char** envp) noexcept : _envp(envp), _buffer_pool(BUFFER_POOL_SIZE), _global_pool(0) {
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
	void add_alias(std::string from, std::string to) {
		_aliases.define_alias(from, to);
	}
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

	bool try_get_env(std::string_view key, std::string_view& val) const {
		if (auto it = _env.find(key); it != _env.end()) {
			val = it->second;
			return true;
		}
		return false;
	}

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


