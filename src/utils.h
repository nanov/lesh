#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

// begin - alias shit double hash == https://www.reddit.com/r/cpp_questions/comments/12xw3sn/find_stdstring_view_in_unordered_map_with/ == https://godbolt.org/z/789xv8Eeq
template<typename ... Bases>
struct overload : Bases ...{
    using is_transparent = void;
    using Bases::operator() ... ;
};


struct char_pointer_hash{
    auto operator()( const char* ptr ) const noexcept{ return std::hash<std::string_view>{}( ptr );}
};

using transparent_string_hash = overload<
    std::hash<std::string>,
    std::hash<std::string_view>,
    char_pointer_hash
>;

template <typename T, const size_t limit>
class hybrid_vector {
	private:
	std::array<T, limit> base_array;
	std::vector<T> heap_vector;
};


class alias_container {
		using alias_map = std::unordered_map<std::string, std::string, transparent_string_hash, std::equal_to<>>;

	private:
			alias_map _aliases;

	public:
		alias_container(std::initializer_list<std::pair<const std::string, std::string>> l) noexcept : _aliases(alias_map(l)) {
		}

		alias_container() noexcept {
			_aliases = {};
			_aliases.reserve(10);
		}
		
		bool try_get_expansion(const std::string_view& word, std::string& expansion) const {
			if (auto it = _aliases.find(word); it != _aliases.end()) {
				expansion = it->second;
				return true;
			}
			return false;
		}
};

