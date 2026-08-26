#include "substrate/args.h"

#include <cstring>
#include <limits>

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
// The integral converter
// ---------------------------------------------------------------------------
//
// clip.hpp's, taken as the note recommends (Appendix A) and widened to the
// erased field: it reads into the widest integer and the caller range-checks
// against the row's own width. No `strtol`, so no errno, no locale, and no
// nul-terminated-string requirement beyond what argv already gives.
//
// The overflow test is `acc > (limit - digit) / 10` BEFORE the multiply, which
// is the only formulation that never overflows on the way to detecting an
// overflow - the version this replaced in three builtins multiplied first.

struct converted {
	std::uint64_t magnitude = 0;
	bool negative = false;
	bool ok = false;
};

converted to_integer(std::string_view text) noexcept {
	if (text.empty())
		return {};
	converted out;
	std::size_t i = 0;
	if (text[0] == '+' || text[0] == '-') {
		out.negative = text[0] == '-';
		if (text.size() == 1)
			return {};
		i = 1;
	}
	for (; i < text.size(); ++i) {
		const char c = text[i];
		if (c < '0' || c > '9')
			return {};
		const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
		if (out.magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
			return {};
		out.magnitude = out.magnitude * 10 + digit;
	}
	out.ok = true;
	return out;
}

// Does the converted number fit the field it is about to be written into? The
// row knows the width and the signedness; nothing else about the type matters.
bool fits(const converted& n, std::uint8_t width, bool is_signed) noexcept {
	const unsigned bits = static_cast<unsigned>(width) * 8u;
	if (is_signed) {
		const std::uint64_t limit = std::uint64_t{1} << (bits - 1);
		return n.negative ? n.magnitude <= limit : n.magnitude < limit;
	}
	if (n.negative)
		return n.magnitude == 0; // `-0` is the only negative an unsigned field takes
	return bits == 64 || n.magnitude < (std::uint64_t{1} << bits);
}

void store_number(std::byte* at, const converted& n, std::uint8_t width) noexcept {
	// Two's complement, well defined since C++20, and the low `width` bytes of
	// the result are the field's whole object representation.
	const std::uint64_t bits = n.negative ? ~n.magnitude + 1u : n.magnitude;
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
		const converted n = to_integer(std::string_view{argument});
		if (!n.ok || !fits(n, row.width, row.is_signed))
			return error{error_kind::invalid_argument, row.letter};
		store_number(at, n, row.width);
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
