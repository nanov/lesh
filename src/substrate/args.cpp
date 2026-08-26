#include "substrate/args.h"

#include "substrate/numeric.h"

#include <cstring>

namespace lesh::args {

namespace {

// ---------------------------------------------------------------------------
// Reading and writing a bound field
// ---------------------------------------------------------------------------
//
// EVERYTHING GOES THROUGH memcpy, and that is not timidity. The core is handed a
// `std::byte*` into a field whose type it deliberately does not know; touching
// it through any other pointer type would be a strict-aliasing violation that no
// sanitizer in the gate reports and that -O2 is entitled to act on. memcpy of a
// trivially copyable object is the one access the standard blesses, and clang
// lowers each of these to the single load or store it looks like.

template <class T>
T load(const std::byte* at) noexcept {
	T value{};
	std::memcpy(&value, at, sizeof(T));
	return value;
}

template <class T>
void store(std::byte* at, T value) noexcept {
	std::memcpy(at, &value, sizeof(T));
}

// `-v` again. The width and signedness come off the row because the field's own
// type does not reach here; a bool field is rejected at compile time, so every
// case below is a real integer.
void increment_field(std::byte* at, std::uint8_t width, bool is_signed) noexcept {
	switch (width) {
	case 1:
		is_signed ? store<std::int8_t>(at, static_cast<std::int8_t>(load<std::int8_t>(at) + 1))
		          : store<std::uint8_t>(at, static_cast<std::uint8_t>(load<std::uint8_t>(at) + 1));
		break;
	case 2:
		is_signed ? store<std::int16_t>(at, static_cast<std::int16_t>(load<std::int16_t>(at) + 1))
		          : store<std::uint16_t>(at,
		                                 static_cast<std::uint16_t>(load<std::uint16_t>(at) + 1));
		break;
	case 4:
		is_signed ? store<std::int32_t>(at, load<std::int32_t>(at) + 1)
		          : store<std::uint32_t>(at, load<std::uint32_t>(at) + 1);
		break;
	default:
		is_signed ? store<std::int64_t>(at, load<std::int64_t>(at) + 1)
		          : store<std::uint64_t>(at, load<std::uint64_t>(at) + 1);
		break;
	}
}

// ---------------------------------------------------------------------------
// The integral option-argument
// ---------------------------------------------------------------------------
//
// THROUGH THE TREE'S ONE NUMERIC PARSER, not through a converter of its own.
// The obvious thing here is clip.hpp's constexpr reader, which the research
// note recommends taking - but substrate/numeric.h already IS that reader, for
// every site in the shell, and tests/unit/builtin_registry_tests.cpp greps the
// tree for anything that reads digits without it. #63 consolidated fifteen
// sites and six idioms after two of them shipped wrong answers a script could
// branch on; adding a sixteenth reader here would be the state that header
// exists to end. So `option_argument` is a numeric_site with a policy row, and
// what is left in this file is the half numeric.h deliberately does not know:
// how wide the FIELD is.
//
// The policy row's range is the widest field a row can bind. The narrower bound
// belongs here because only the row knows it - `-b 256` does not fit the
// uint8_t it binds, and `-b -1` is a well-formed number that an unsigned field
// still has no room for.

bool convert(std::string_view text, std::uint8_t width, bool is_signed,
             std::uint64_t& bits) noexcept {
	const numeric_result read = parse_integer(text, numeric_site::option_argument);
	if (read.status != numeric_parse::ok)
		return false;
	const std::int64_t value = read.value;
	const unsigned field_bits = static_cast<unsigned>(width) * 8u;
	if (is_signed) {
		if (field_bits < 64) {
			const std::int64_t high = (std::int64_t{1} << (field_bits - 1)) - 1;
			if (value < -high - 1 || value > high)
				return false;
		}
	} else {
		if (value < 0)
			return false;
		if (field_bits < 64 &&
		    static_cast<std::uint64_t>(value) > (std::uint64_t{1} << field_bits) - 1)
			return false;
	}
	// Two's complement, well defined since C++20: the low `width` bytes of this
	// are the field's whole object representation.
	bits = static_cast<std::uint64_t>(value);
	return true;
}

void store_number(std::byte* at, std::uint64_t bits, std::uint8_t width) noexcept {
	switch (width) {
	case 1: store<std::uint8_t>(at, static_cast<std::uint8_t>(bits)); break;
	case 2: store<std::uint16_t>(at, static_cast<std::uint16_t>(bits)); break;
	case 4: store<std::uint32_t>(at, static_cast<std::uint32_t>(bits)); break;
	default: store<std::uint64_t>(at, bits); break;
	}
}

// ---------------------------------------------------------------------------
// The setters
// ---------------------------------------------------------------------------

// One row's write. `plus` is the sigil the option was spelled with, which only
// a toggle looks at.
error apply(const option& row, void* into, bool plus, const char* argument) noexcept {
	std::byte* const at = row.at(into);
	switch (row.set) {
	case setter::set_bool:
		store<bool>(at, true);
		break;
	case setter::toggle:
		// `set -x` enables, `set +x` disables. The deviant becomes an ordinary row.
		store<bool>(at, !plus);
		break;
	case setter::set_enum_value:
		// The bytes were baked by the row's consteval constructor, so the core
		// needs neither the enum's type nor a byte-order assumption.
		std::memcpy(at, row.value.data(), row.width);
		break;
	case setter::increment:
		increment_field(at, row.width, row.is_signed);
		break;
	case setter::store_view:
		// A VIEW INTO argv. Nothing is copied and nothing is owned, which is why
		// the whole parse allocates nothing; the operand tail stays the caller's.
		store<std::string_view>(at, std::string_view{argument});
		break;
	case setter::store_integral: {
		std::uint64_t bits = 0;
		if (!convert(std::string_view{argument}, row.width, row.is_signed, bits))
			return error{error_kind::invalid_argument, row.letter};
		store_number(at, bits, row.width);
		break;
	}
	}
	return error{};
}

} // namespace

namespace detail {

// THE PARSE, and there is one of it in the program. The grammar is the constexpr
// stepper in the header - shared with the self-tests, so a change to POSIX
// conformance fails the build before it reaches a test - and everything here is
// the store the grammar cannot do at compile time.
scan_result parse_core(std::span<const option> rows, char** argv, void* into) noexcept {
	cursor c{argv, 1, 1};
	while (true) {
		const step s = next(c, rows);
		if (s.what == step_kind::failed)
			return scan_result{nullptr, s.err};
		if (s.what == step_kind::done)
			return scan_result{argv + c.word, {}};
		if (const error err = apply(rows[s.index], into, s.plus, s.argument))
			return scan_result{nullptr, err};
	}
}

} // namespace detail

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
//
// Compiled once, like the parse, and for the same reason: it is driven by the
// table rather than instantiated per utility. It writes through the sink and
// touches no heap - the only buffer is four bytes of stack for `-x`.
//
// The shape is dash's synopsis line followed by one line per row:
//
//     usage: cd [-L] [-P] [-e] [operand...]
//       -L            resolve `..` against the logical working directory
//
// No `--help` is wired to it: POSIX builtins do not have one, and the `help`
// builtin #148 defers is a later ticket. Today the tests are its only caller.

void write_usage(const sink& out, std::string_view name, std::span<const option> rows) {
	out("usage: ");
	out(name);
	for (const option& row : rows) {
		const char spelled[4] = {' ', '[', '-', row.letter};
		out(std::string_view{spelled, sizeof(spelled)});
		if (row.takes == arg::required) {
			out(" ");
			out(row.placeholder != nullptr ? std::string_view{row.placeholder}
			                               : std::string_view{"ARG"});
		}
		out("]");
	}
	out(" [operand...]\n");

	for (const option& row : rows) {
		const char spelled[4] = {' ', ' ', '-', row.letter};
		out(std::string_view{spelled, sizeof(spelled)});
		if (row.name != nullptr) {
			out(", --");
			out(std::string_view{row.name});
		}
		if (row.takes == arg::required) {
			out(" ");
			out(row.placeholder != nullptr ? std::string_view{row.placeholder}
			                               : std::string_view{"ARG"});
		}
		if (row.help_text != nullptr) {
			out("   ");
			out(std::string_view{row.help_text});
		}
		out("\n");
	}
}

} // namespace lesh::args
