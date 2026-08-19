#pragma once

// Small vocabulary types shared by everything above the substrate.
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace lesh {

template<typename ... Bases>
struct overload : Bases ...{
    using is_transparent = void;
    using Bases::operator() ... ;
};
struct char_pointer_hash
{
	auto operator()( const char* ptr ) const noexcept
	{
		return std::hash<std::string_view>{}( ptr );
	}
};
using transparent_string_hash = overload<
    std::hash<std::string>,
    std::hash<std::string_view>,
    char_pointer_hash
>;
struct char_iterable {
private:
	char** _stack;
	char** _end;
public:
	char_iterable(char** self, char** end): _stack(self), _end(end) {}
	struct iterator {
	private:
		char** _location;
	public:
		explicit iterator(char** begin) : _location(begin) {}

		char*& operator*() const { return *_location; }

		iterator& operator++() {
			++_location;
			return *this;
		}

		bool operator!=(const iterator& other) const { return _location != other._location; }
	};

	iterator begin() { return iterator(this->_stack); }
	iterator end() { return iterator(this->_end);}
};

} // namespace lesh
