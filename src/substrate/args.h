#pragma once

// POSIX option parsing, once, for everything that has options (#148).
//
// The tree had THIRTEEN hand-rolled option loops - 336 lines across builtins.cpp
// and invocation.cpp - and they disagreed on seven axes: seven different
// unknown-option wordings, inconsistent clustering, inconsistent option-argument
// attachment. The research note
// (docs/superpowers/research/2026-08-26-builtin-option-definition.md) catalogues
// every divergence and measured every off-the-shelf candidate. None of them
// clears the tree's gates: no mainstream C++ option library achieves a zero-heap
// parse, because all of them copy argv into a vector<string> first.
//
// WHY THIS LIVES IN THE SUBSTRATE, and not in runtime/ beside the builtins. It
// depends on nothing - argv, the standard library, and a table - which is the
// substrate's entry requirement, and it has three consumers in three different
// modules: runtime's builtins, invocation.cpp's reading of the shell's own
// command line, and leshper's `bind`. The same argument that put grapheme here
// (#108): a type several modules need and none of them owns.
//
// THE SHAPE, in one screen:
//
//     struct cd_opts {
//         cd_mode mode = cd_mode::logical;   // the DEFAULT is a member initializer
//         bool    e    = false;
//     };
//
//     constexpr auto kCd = args::spec<cd_opts>(
//         args::option{'L', args::field<&cd_opts::mode>, cd_mode::logical},
//         args::option{'P', args::field<&cd_opts::mode>, cd_mode::physical},
//         args::option{'e', args::field<&cd_opts::e>});
//
//     const auto r = args::parse(kCd, argv);
//     if (r.err)
//         return {report_option_error("cd", r.err)};
//     for (char** a = r.rest; *a != nullptr; ++a) ...   // the untouched operands
//
// Three properties follow from binding options to FIELDS rather than to a
// bitmask, and each of them was a bug class in the loops this replaces:
//
//   - LAST ONE WINS, FREE. `-L` and `-P` write the same field, so `cd -P -L -PL`
//     is -L for the same reason `x = 1; x = 2` is 2 - no mode-group bookkeeping,
//     no tie-break code to get wrong inside a cluster. cd-p.tst:354 and :365 are
//     the corpus's one real tripwire here (research note S3.5) and this is what
//     answers them.
//   - NO LOOKUP EXISTS. The body reads `o.mode`, not `r.has(0)`. Table positions
//     were magic numbers; a field name is checked by the compiler.
//   - THE DEFAULT IS DECLARED ONCE, as the struct's member initializer, where a
//     reader looking for it will look.
//
// WHAT IS SHARED AND WHAT IS PER-UTILITY. The parse loop is in args.cpp, ONE
// copy, reached through erased rows: each row carries a projection to its bound
// field plus a setter kind, never the field's type. A utility costs a table in
// .rodata and one tiny projection per row, not an instantiated parser - the
// difference the note measured as 77 bytes against clip.hpp's 2,238 per builtin.
//
// WHY `field<&S::m>` AND NOT A BARE `&S::m`. The language forces the wrapper,
// and this was measured rather than assumed. Erasing a bound field needs its
// BYTE OFFSET, and C++23 offers no way to obtain one from a pointer-to-member
// passed as an ordinary function argument: `std::bit_cast` from a member pointer
// is explicitly not a constant expression ([bit.cast]/3), `reinterpret_cast` is
// barred from constant evaluation, and `offsetof` needs a member designator
// rather than a value. Passing the member pointer as a TEMPLATE argument does
// work, and `field<&cd_opts::mode>` is exactly that - a nullary object whose
// type carries the pointer, so it can emit a projection function. One word at
// the declaration, no macro, and the argument order of the row is unchanged.
//
// WHAT IS AND IS NOT A CONSTANT EXPRESSION. `scan` - the whole POSIX grammar,
// which is where every tripwire lives - is constexpr and this header asserts it
// against the note's conformance cases below, so the grammar cannot regress
// without failing the build. `parse` is not, and cannot be under C++23: storing
// into a field through an erased base requires recovering a typed pointer from
// `void*`, which constant evaluation does not permit until C++26's P2738. The
// stores are asserted at runtime instead, in tests/unit/args_tests.cpp, which is
// also where the corpus's last-one-wins cases live.
//
// VOCABULARY (#84): POSIX's words and only POSIX's. An OPTION is `-x`. An
// OPERAND is what follows the options. A SPEC is one utility's table, a ROW is
// one entry in it, and a SETTER KIND is how a row writes its field. Never flag,
// never positional.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace lesh::args {

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------

// Whether the option takes an option-argument. POSIX XBD 12.2 Guideline 7: it
// may be attached (`-dX`) or separate (`-d X`), and a row does not get to
// choose - both spellings are accepted, which is half of what the hand-rolled
// loops disagreed about.
enum class arg : std::uint8_t { none, required };

// How a row writes its bound field. This enumerates the SETTERS, not the
// features: `-L`/`-P` mode groups, `-vvv` counters, `set -o name` sub-options
// and long names are all combinations of a matcher and one of these.
enum class setter : std::uint8_t {
	set_bool,        // `-e` -> field = true
	set_enum_value,  // `-L` -> field = cd_mode::logical
	increment,       // `-v` -> ++field
	store_view,      // `-d X` -> field = string_view into argv, never copied
	store_integral,  // `-n 5` -> field = 5, range-checked against the field
	toggle,          // `-x` -> true, `+x` -> false, the same field
};

enum class error_kind : std::uint8_t {
	none,
	unknown_option,    // no row matches the letter, or `+x` where `-x` only
	missing_argument,  // the last word was an option that needed an argument
	invalid_argument,  // store_integral could not read the argument, or it did not fit
};

// Two bytes, returned by value, and it names the LETTER rather than the word so
// one shared reporter can write every utility's diagnostic (see
// runtime/diagnostic.h's report_option_error).
struct error {
	error_kind kind = error_kind::none;
	char letter = '\0';

	// True when there IS an error, so a call site reads `if (r.err)`.
	constexpr explicit operator bool() const noexcept { return kind != error_kind::none; }
	constexpr bool operator==(const error&) const noexcept = default;
};

static_assert(sizeof(error) == 2, "the error is meant to be two bytes in a register");

// ---------------------------------------------------------------------------
// Binding a field
// ---------------------------------------------------------------------------

namespace detail {

template <class>
struct member_of;

template <class S, class M>
struct member_of<M S::*> {
	using klass = S;
	using type = M;
};

// A distinct address per option struct, so a spec can check at compile time
// that all of its rows bind the SAME struct. Cheaper than carrying the type.
template <class S>
inline constexpr char key = 0;

} // namespace detail

// The projection a row is built from. `binding<&cd_opts::mode>::at(base)` is the
// address of that field inside a `cd_opts` the parser was handed as `void*` -
// the one thing the erased core cannot work out for itself.
template <auto Member>
struct binding {
	using klass = typename detail::member_of<decltype(Member)>::klass;
	using type = typename detail::member_of<decltype(Member)>::type;

	static constexpr std::byte* at(void* base) noexcept {
		// Both casts are the sanctioned ones: `void*` back to the type it came
		// from, and an object's address to `std::byte*` for byte access.
		return reinterpret_cast<std::byte*>(&(static_cast<klass*>(base)->*Member));
	}
};

// `field<&cd_opts::mode>` - see the header comment for why the wrapper exists.
template <auto Member>
inline constexpr binding<Member> field{};

// The tags that pick a setter where the field's type alone cannot.
struct count_t {};
struct toggle_t {};
struct value_t {
	const char* placeholder = "ARG";
};

// `option{'v', field<&o::verbosity>, count}` - each `-v` adds one.
inline constexpr count_t count{};
// `option{'x', field<&o::xtrace>, toggle}` - `-x` sets, `+x` clears. POSIX's
// `set` spells its options both ways and this makes that an ordinary row.
inline constexpr toggle_t toggle{};
// `option{'d', field<&o::delimiter>, value("SEP")}` - takes an option-argument;
// the placeholder is what the usage writer prints.
constexpr value_t value(const char* placeholder = "ARG") noexcept { return value_t{placeholder}; }

// ---------------------------------------------------------------------------
// The row
// ---------------------------------------------------------------------------

// A string_view is the widest field a row may bind; the pre-baked bytes of an
// enum value never need more.
inline constexpr std::size_t MAX_BOUND_FIELD = sizeof(std::string_view);

// One entry of a spec, already erased: the letter, the optional long name, a
// projection to the bound field, and how to write it. Declaring a row and
// storing one are the same type, so there is no second representation to keep
// in step.
struct option {
	char letter = '\0';
	setter set = setter::set_bool;
	arg takes = arg::none;
	bool plus = false;      // admits `+x` as well as `-x`
	std::uint8_t width = 0; // sizeof the bound field
	bool is_signed = false; // of an integral field, for the range check
	const char* name = nullptr;        // the long name, or none
	const char* placeholder = nullptr; // usage: the option-argument's name
	const char* help_text = nullptr;
	std::byte* (*at)(void*) noexcept = nullptr;
	const void* bound_to = nullptr; // detail::key<S> of the struct this binds
	// set_enum_value's value, byte-baked at compile time so the core needs no
	// knowledge of the field's type and no assumption about byte order.
	std::array<std::byte, MAX_BOUND_FIELD> value{};

	// --- the declaration forms ---------------------------------------------

	// `option{'e', field<&cd_opts::e>}` - a bool field set by the option's
	// presence, or an integral one named for a later `value(...)`/`count`.
	template <auto Member>
	consteval option(char l, binding<Member> b) : option(l, nullptr, b) {}

	template <auto Member>
	consteval option(char l, const char* long_name, binding<Member>)
		: letter{l}, name{long_name}, at{&binding<Member>::at},
		  bound_to{&detail::key<typename binding<Member>::klass>} {
		using field_type = typename binding<Member>::type;
		static_assert(std::is_same_v<field_type, bool>,
		              "a row with no value and no tag must bind a bool; pass the value "
		              "for an enum field, or count / toggle / value(...) otherwise");
		set = setter::set_bool;
		width = sizeof(bool);
		bake(true);
	}

	// `option{'L', field<&cd_opts::mode>, cd_mode::logical}` - writes a fixed
	// value. Two rows naming one field are a mode group, and last-one-wins is
	// then just the second store.
	template <auto Member, class V>
	consteval option(char l, binding<Member> b, V v) : option(l, nullptr, b, v) {}

	template <auto Member, class V>
	consteval option(char l, const char* long_name, binding<Member>, V v)
		: letter{l}, name{long_name}, at{&binding<Member>::at},
		  bound_to{&detail::key<typename binding<Member>::klass>} {
		using field_type = typename binding<Member>::type;
		width = static_cast<std::uint8_t>(sizeof(field_type));
		if constexpr (std::is_same_v<V, count_t>) {
			static_assert(std::is_integral_v<field_type> && !std::is_same_v<field_type, bool>,
			              "count binds an integral field");
			set = setter::increment;
			is_signed = std::is_signed_v<field_type>;
		} else if constexpr (std::is_same_v<V, toggle_t>) {
			static_assert(std::is_same_v<field_type, bool>, "toggle binds a bool field");
			set = setter::toggle;
			plus = true;
		} else if constexpr (std::is_same_v<V, value_t>) {
			takes = arg::required;
			placeholder = v.placeholder;
			if constexpr (std::is_same_v<field_type, std::string_view>) {
				set = setter::store_view;
			} else {
				static_assert(std::is_integral_v<field_type> || std::is_enum_v<field_type>,
				              "value(...) binds a string_view, an integral or an enum field");
				set = setter::store_integral;
				is_signed = std::is_signed_v<underlying_of<field_type>>;
			}
		} else {
			static_assert(std::is_same_v<V, field_type>,
			              "the value a row writes must have the bound field's own type");
			set = setter::set_enum_value;
			bake(v);
		}
	}

	// The help text, so the declaration reads as one expression:
	// `option{'e', field<&cd_opts::e>}.help("fail if PWD cannot be determined")`.
	[[nodiscard]] consteval option help(const char* text) const {
		option copy = *this;
		copy.help_text = text;
		return copy;
	}

private:
	template <class T>
	using underlying_of = typename std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>,
	                                                  std::type_identity<T>>::type;

	// The value's object representation, written at compile time. Doing it here
	// rather than in the core is what keeps the core free of both the field's
	// type and any assumption about byte order.
	template <class V>
	consteval void bake(V v) {
		static_assert(sizeof(V) <= MAX_BOUND_FIELD);
		const auto bytes = std::bit_cast<std::array<std::byte, sizeof(V)>>(v);
		for (std::size_t i = 0; i < sizeof(V); ++i)
			value[i] = bytes[i];
	}
};

// ---------------------------------------------------------------------------
// The spec, and its compile-time validation
// ---------------------------------------------------------------------------

// Declared and deliberately never defined. A malformed table names one of these
// in the compiler's diagnostic, which reads better than a string literal and -
// unlike `throw` - is ill-formed in a constant expression under every policy,
// including the `-fno-exceptions` build the note measured (S8.2).
void malformed_option_spec_duplicate_letter();
void malformed_option_spec_non_alphanumeric_letter();
void malformed_option_spec_duplicate_long_name();
void malformed_option_spec_rows_bind_different_structs();
void malformed_option_spec_more_than_64_rows();
void malformed_option_spec_setter_disagrees_with_option_argument();

template <class T, std::size_t N>
struct spec_table {
	using bound = T;
	std::array<option, N> rows;

	[[nodiscard]] constexpr std::span<const option> view() const noexcept { return rows; }
};

namespace detail {

constexpr bool alphanumeric(char c) noexcept {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

constexpr bool same_name(const char* a, const char* b) noexcept {
	if (a == nullptr || b == nullptr)
		return false;
	return std::string_view{a} == std::string_view{b};
}

template <class T, std::size_t N>
consteval void validate(const std::array<option, N>& rows) {
	if constexpr (N > 64)
		malformed_option_spec_more_than_64_rows();
	for (std::size_t i = 0; i < N; ++i) {
		// XBD 12.2 Guideline 3: an option is a single alphanumeric character.
		if (!alphanumeric(rows[i].letter))
			malformed_option_spec_non_alphanumeric_letter();
		if (rows[i].bound_to != &key<T>)
			malformed_option_spec_rows_bind_different_structs();
		// The setter and the grammar have to agree, or a row would silently
		// read an argument it never stores, or store one it never read.
		const bool wants_argument =
			rows[i].set == setter::store_view || rows[i].set == setter::store_integral;
		if (wants_argument != (rows[i].takes == arg::required))
			malformed_option_spec_setter_disagrees_with_option_argument();
		for (std::size_t j = i + 1; j < N; ++j) {
			// The defect a hand-rolled if-chain hides forever, because the
			// second branch is simply never reached.
			if (rows[j].letter == rows[i].letter)
				malformed_option_spec_duplicate_letter();
			if (same_name(rows[i].name, rows[j].name))
				malformed_option_spec_duplicate_long_name();
		}
	}
}

} // namespace detail

// `constexpr auto kCd = args::spec<cd_opts>(option{...}, option{...});`
//
// The struct is named rather than deduced: it is the thing a reader wants to see
// at the top of the table, and naming it lets the validation say so when a row
// binds a field of some other struct.
template <class T, class... Rows>
[[nodiscard]] consteval spec_table<T, sizeof...(Rows)> spec(const Rows&... rows) {
	const spec_table<T, sizeof...(Rows)> table{{rows...}};
	detail::validate<T>(table.rows);
	return table;
}

// ---------------------------------------------------------------------------
// The grammar
// ---------------------------------------------------------------------------
//
// POSIX XBD 12.2, and the four rules a hand-rolled loop keeps getting separately
// wrong: clustering (`-abc`), an option-argument attached (`-dX`) or separate
// (`-d X`), `--` ending the options, and a LONE `-` being an operand rather than
// an empty option group.
//
// A resumable STEP rather than a loop, so the one grammar serves both the erased
// core in args.cpp and the constexpr self-tests at the bottom of this header.
// There is no second copy of it to drift.

namespace detail {

struct cursor {
	char** argv = nullptr;
	std::size_t word = 1;   // argv index; argv[0] is the utility's own name
	std::size_t letter = 1; // index into argv[word]; 1 is just past the sigil
};

enum class step_kind : std::uint8_t { done, matched, failed };

struct step {
	step_kind what = step_kind::done;
	std::size_t index = 0;          // the row that matched
	bool plus = false;              // it was spelled `+x`
	const char* argument = nullptr; // the option-argument, when the row takes one
	error err{};
};

// Whether the spec admits the `+` sigil AT ALL. It decides whether `+x` is an
// option word or an operand, and the distinction is not cosmetic: `set +x`
// clears an option, while `cd +1` is a chdir into a directory called `+1`, which
// is what dash does and what this tree did before the table replaced its loop.
// So a spec with no toggle row never reads `+` as a sigil, and a spec with one
// reads every `+` word as options - `set +z` is an illegal option, not a file.
[[nodiscard]] constexpr bool admits_plus(std::span<const option> rows) noexcept {
	for (const option& row : rows)
		if (row.plus)
			return true;
	return false;
}

[[nodiscard]] constexpr std::size_t index_of(std::span<const option> rows, char letter) noexcept {
	// A linear scan, which is what zsh and dash do (`strchr` over an optstring).
	// Tables here run to two or three rows and `set`'s to twelve; at that size a
	// scan is a few byte compares in one cache line and beats any hash.
	for (std::size_t i = 0; i < rows.size(); ++i)
		if (rows[i].letter == letter)
			return i;
	return rows.size();
}

// `--name` and `--name=value`, for the rows that declare a long name.
//
// POSIX BUILTINS NEVER DECLARE ONE - XBD 12.2 has no long options, and a `cd
// --logical` that worked here and nowhere else would be a divergence nobody
// asked for. The matcher exists for the ported commands #148 anticipates, and
// a spec that names no long option cannot reach this: `--anything` is then an
// unknown option, exactly as it is today.
//
// An unrecognised long name has no letter to report, so the error carries '\0'
// and the shared reporter drops the letter from its wording. A RECOGNISED one
// reports through its row's letter, which every row has.
[[nodiscard]] constexpr step long_option(cursor& c, std::span<const option> rows,
                                         std::string_view word) noexcept {
	// Explicit views rather than substr(): substr throws out_of_range on a bad
	// position, and the compiler cannot prove this one is good, so it emitted the
	// throw path and __clang_call_terminate into a noexcept substrate function -
	// 400 bytes of unreachable unwinding, found by reading llvm-nm on args.o.
	const std::string_view body{word.data() + 2, word.size() - 2};
	const std::size_t equals = body.find('=');
	const std::string_view name{body.data(),
	                            equals == std::string_view::npos ? body.size() : equals};

	std::size_t i = rows.size();
	for (std::size_t k = 0; k < rows.size(); ++k)
		if (rows[k].name != nullptr && std::string_view{rows[k].name} == name) {
			i = k;
			break;
		}
	if (i == rows.size())
		return step{step_kind::failed, 0, false, nullptr, {error_kind::unknown_option, '\0'}};

	if (rows[i].takes != arg::required) {
		// `--verbose=3` on a row that takes nothing: the value has nowhere to go,
		// and silently dropping it is how a script gets a wrong answer.
		if (equals != std::string_view::npos)
			return step{step_kind::failed, 0, false, nullptr,
			            {error_kind::invalid_argument, rows[i].letter}};
		++c.word;
		return step{step_kind::matched, i, false, nullptr, {}};
	}

	const char* argument = nullptr;
	if (equals != std::string_view::npos) {
		argument = c.argv[c.word] + 2 + equals + 1;
	} else if (c.argv[c.word + 1] != nullptr) {
		argument = c.argv[++c.word];
	} else {
		return step{step_kind::failed, 0, false, nullptr,
		            {error_kind::missing_argument, rows[i].letter}};
	}
	++c.word;
	return step{step_kind::matched, i, false, argument, {}};
}

[[nodiscard]] constexpr step next(cursor& c, std::span<const option> rows) noexcept {
	while (true) {
		char* const w = c.argv[c.word];
		if (w == nullptr)
			return step{};
		const std::string_view word{w};
		if (c.letter == 1) {
			if (word == "--") {
				++c.word; // consumed: the operands begin after it
				return step{};
			}
			// A lone `-` is an operand - cd's OLDPWD, trap's reset action - and a
			// word starting with neither sigil ends the options. POSIX does not
			// permute: everything from here on is an operand.
			const bool sigil = word[0] == '-' || (word[0] == '+' && admits_plus(rows));
			if (word.size() < 2 || !sigil)
				return step{};
			if (word[0] == '-' && word[1] == '-')
				return long_option(c, rows, word);
		}
		if (c.letter >= word.size()) {
			++c.word;
			c.letter = 1;
			continue;
		}

		const bool plus = word[0] == '+';
		const char letter = word[c.letter];
		++c.letter;
		const std::size_t i = index_of(rows, letter);
		if (i == rows.size() || (plus && !rows[i].plus))
			return step{step_kind::failed, 0, false, nullptr, {error_kind::unknown_option, letter}};

		if (rows[i].takes != arg::required)
			return step{step_kind::matched, i, plus, nullptr, {}};

		const char* argument = nullptr;
		if (c.letter < word.size()) {
			argument = w + c.letter; // attached: -dX, and the rest of the word is it
		} else if (c.argv[c.word + 1] != nullptr) {
			argument = c.argv[++c.word]; // separate: -d X
		} else {
			return step{step_kind::failed, 0, false, nullptr,
			            {error_kind::missing_argument, letter}};
		}
		++c.word;
		c.letter = 1;
		return step{step_kind::matched, i, plus, argument, {}};
	}
}

} // namespace detail

// What the grammar alone decides: where the operands begin, and whether the
// option words were well formed. `rest` is null on failure, so an unchecked
// result cannot be iterated.
struct scan_result {
	char** rest = nullptr;
	error err{};
};

// The grammar, with no stores - a constant expression, and the half of the parse
// this header asserts against the note's POSIX cases below.
[[nodiscard]] constexpr scan_result scan(std::span<const option> rows, char** argv) noexcept {
	detail::cursor c{argv, 1, 1};
	while (true) {
		const detail::step s = detail::next(c, rows);
		if (s.what == detail::step_kind::failed)
			return scan_result{nullptr, s.err};
		if (s.what == detail::step_kind::done)
			return scan_result{argv + c.word, {}};
	}
}

template <class T, std::size_t N>
[[nodiscard]] constexpr scan_result scan(const spec_table<T, N>& s, char** argv) noexcept {
	return scan(s.view(), argv);
}

// ---------------------------------------------------------------------------
// The parse
// ---------------------------------------------------------------------------

// The options as the utility declared them, the untouched operand tail, and the
// error. Returned by value; nothing here allocates and nothing here is a copy of
// argv - a `string_view` field points into the caller's own words.
template <class T>
struct result {
	T opts{};
	char** rest = nullptr;
	error err{};

	constexpr explicit operator bool() const noexcept { return !err; }
};

namespace detail {

// THE WHOLE PARSE LOOP, and there is one of it. Defined in args.cpp so it is
// compiled once for the program rather than once per utility - the property that
// separates this design from every templated option library the note measured.
[[nodiscard]] scan_result parse_core(std::span<const option> rows, char** argv,
                                     void* into) noexcept;

} // namespace detail

// `auto r = args::parse(kCd, argv);`
//
// A thin typed wrapper: it names the struct, hands the core a pointer to it, and
// gets out of the way. `argv` is the utility's own, argv[0] included.
template <class T, std::size_t N>
[[nodiscard]] result<T> parse(const spec_table<T, N>& s, char** argv) noexcept {
	result<T> out;
	const scan_result r = detail::parse_core(s.view(), argv, &out.opts);
	out.rest = r.rest;
	out.err = r.err;
	return out;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

// Where the usage text goes. A pair of words rather than a template, so the
// writer below is compiled once like the parse is, and so a caller can aim it at
// stderr, at a string, or at leshper's pager without any of them appearing here.
struct sink {
	void (*write)(void* context, std::string_view text) = nullptr;
	void* context = nullptr;

	void operator()(std::string_view text) const { write(context, text); }
};

// The synopsis line and one line per row, assembled from the same table the
// parse reads. Heap-free: it writes through the sink and owns nothing.
//
// POSIX builtins do not grow `--help`; this exists for the usage line of a
// diagnostic and for the `help` builtin that #148 leaves to a later ticket, so
// for now the tests are its only caller.
void write_usage(const sink& out, std::string_view name, std::span<const option> rows);

template <class T, std::size_t N>
void write_usage(const sink& out, std::string_view name, const spec_table<T, N>& s) {
	write_usage(out, name, s.view());
}

// ---------------------------------------------------------------------------
// Self-tests
// ---------------------------------------------------------------------------
//
// The grammar's conformance cases, as static_asserts: they cost nothing at
// runtime and cannot be skipped by not running the tests, which is the argument
// builtins.cpp's registry static_assert already makes. The cases are the note's
// POSIX XBD 12.2 suite (S8.6); the ones that assert a STORED value live in
// tests/unit/args_tests.cpp instead, for the reason the header comment gives.

namespace self_test {

enum class mode : std::uint8_t { logical, physical };

struct opts {
	mode m = mode::logical;
	bool e = false;
	bool x = false;
	std::string_view delimiter{};
	int verbosity = 0;
};

inline constexpr auto kProbe = spec<opts>(
	option{'L', field<&opts::m>, mode::logical},
	option{'P', field<&opts::m>, mode::physical},
	option{'e', field<&opts::e>},
	option{'x', field<&opts::x>, toggle},
	option{'v', field<&opts::verbosity>, count},
	option{'d', "delimiter", field<&opts::delimiter>, value("SEP")});

// The scan, over a mutable argv the way a utility receives one.
constexpr scan_result run(std::span<char*> argv) { return scan(kProbe.view(), argv.data()); }

constexpr bool operands_begin_after_a_cluster() {
	char a0[] = "cd", a1[] = "-LP", a2[] = "/tmp", *argv[] = {a0, a1, a2, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 2;
}

constexpr bool an_attached_option_argument_is_consumed() {
	char a0[] = "read", a1[] = "-d:", a2[] = "v", *argv[] = {a0, a1, a2, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 2;
}

constexpr bool a_separate_option_argument_is_consumed() {
	char a0[] = "read", a1[] = "-d", a2[] = ":", a3[] = "v", *argv[] = {a0, a1, a2, a3, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 3;
}

constexpr bool a_cluster_may_end_in_an_argument_taking_option() {
	char a0[] = "read", a1[] = "-vd:", a2[] = "v", *argv[] = {a0, a1, a2, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 2;
}

constexpr bool a_double_dash_ends_the_options() {
	char a0[] = "read", a1[] = "--", a2[] = "-e", *argv[] = {a0, a1, a2, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 2;
}

constexpr bool a_lone_dash_is_an_operand() {
	char a0[] = "cd", a1[] = "-", *argv[] = {a0, a1, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 1;
}

constexpr bool an_unknown_option_names_its_letter() {
	char a0[] = "cd", a1[] = "-Z", *argv[] = {a0, a1, nullptr};
	const auto r = run(argv);
	return r.err == error{error_kind::unknown_option, 'Z'} && r.rest == nullptr;
}

constexpr bool an_unknown_letter_inside_a_cluster_is_the_one_reported() {
	char a0[] = "cd", a1[] = "-LZP", *argv[] = {a0, a1, nullptr};
	const auto r = run(argv);
	return r.err == error{error_kind::unknown_option, 'Z'};
}

constexpr bool a_missing_option_argument_is_an_error() {
	char a0[] = "read", a1[] = "-d", *argv[] = {a0, a1, nullptr};
	const auto r = run(argv);
	return r.err == error{error_kind::missing_argument, 'd'};
}

constexpr bool operands_follow_the_options() {
	char a0[] = "cd", a1[] = "-L", a2[] = "a", a3[] = "b", *argv[] = {a0, a1, a2, a3, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 2;
}

constexpr bool a_plus_word_is_an_option_only_where_a_row_admits_the_sigil() {
	// kProbe has a toggle row, so `+` is a sigil for it and an unknown letter
	// after one is an error.
	char a0[] = "set", a1[] = "+x", *argv[] = {a0, a1, nullptr};
	char b0[] = "set", b1[] = "+L", *bad[] = {b0, b1, nullptr};
	return !run(argv).err && run(bad).err == error{error_kind::unknown_option, 'L'};
}

constexpr bool a_spec_without_a_toggle_reads_a_plus_word_as_an_operand() {
	// `cd +1` is a chdir into a directory named `+1`, not an illegal option.
	static constexpr auto kNoPlus =
		spec<opts>(option{'L', field<&opts::m>, mode::logical},
	               option{'P', field<&opts::m>, mode::physical});
	char a0[] = "cd", a1[] = "+1", *argv[] = {a0, a1, nullptr};
	const auto r = scan(kNoPlus.view(), argv);
	return !r.err && r.rest == argv + 1;
}

constexpr bool no_options_at_all_leaves_the_first_word() {
	char a0[] = "cd", a1[] = "dir", *argv[] = {a0, a1, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 1;
}

constexpr bool an_empty_argv_tail_is_an_empty_operand_list() {
	char a0[] = "pwd", *argv[] = {a0, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 1 && *r.rest == nullptr;
}

constexpr bool an_empty_word_ends_the_options() {
	// `cd ''` - two bytes short of an option group, and cd diagnoses it as an
	// operand rather than reading it as one.
	char a0[] = "cd", a1[] = "", *argv[] = {a0, a1, nullptr};
	const auto r = run(argv);
	return !r.err && r.rest == argv + 1;
}

constexpr bool a_long_name_matches_where_a_row_declares_one() {
	char a0[] = "x", a1[] = "--delimiter", a2[] = ":", a3[] = "v", *argv[] = {a0, a1, a2, a3,
	                                                                          nullptr};
	char b0[] = "x", b1[] = "--delimiter=:", b2[] = "v", *attached[] = {b0, b1, b2, nullptr};
	char c0[] = "x", c1[] = "--nosuch", *unknown[] = {c0, c1, nullptr};
	return !run(argv).err && run(argv).rest == argv + 3 && !run(attached).err &&
	       run(attached).rest == attached + 2 &&
	       run(unknown).err == error{error_kind::unknown_option, '\0'};
}

static_assert(operands_begin_after_a_cluster(), "XBD 12.2 Guideline 5: options may cluster");
static_assert(a_long_name_matches_where_a_row_declares_one(), "and only where one is declared");
static_assert(an_attached_option_argument_is_consumed(), "Guideline 7: -dX");
static_assert(a_separate_option_argument_is_consumed(), "Guideline 7: -d X");
static_assert(a_cluster_may_end_in_an_argument_taking_option(), "Guideline 5 with Guideline 7");
static_assert(a_double_dash_ends_the_options(), "Guideline 10: --");
static_assert(a_lone_dash_is_an_operand(), "a lone - is cd's OLDPWD, not an empty option group");
static_assert(an_unknown_option_names_its_letter(), "the reporter needs the letter");
static_assert(an_unknown_letter_inside_a_cluster_is_the_one_reported(), "not the first letter");
static_assert(a_missing_option_argument_is_an_error(), "Guideline 7 with nothing after it");
static_assert(operands_follow_the_options(), "Guideline 9: no permutation");
static_assert(a_plus_word_is_an_option_only_where_a_row_admits_the_sigil(), "set's +x");
static_assert(a_spec_without_a_toggle_reads_a_plus_word_as_an_operand(), "and cd's +1 is a path");
static_assert(no_options_at_all_leaves_the_first_word(), "");
static_assert(an_empty_argv_tail_is_an_empty_operand_list(), "");
static_assert(an_empty_word_ends_the_options(), "");

} // namespace self_test

} // namespace lesh::args
