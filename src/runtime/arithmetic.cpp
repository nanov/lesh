#include "runtime/arithmetic.h"

#include "substrate/char_utils.h"

#include <string>

namespace lesh::runtime {

namespace {

// SIGNED OVERFLOW IS UNDEFINED BEHAVIOUR, so every operation below that can
// overflow routes through one of these rather than being written out at its site.
// There were SIX such sites - `value * base + digit` in parse_number, `+`, `-`,
// `*`, unary minus, and `/` and `%` at INT64_MIN / -1 - and every one of them was
// reachable from a single line of input: `$((99999999999999999999))` was enough
// (#59). One place rather than six is the lesson #35 and #49 both taught, that N
// sites fixed individually become N subtly different behaviours.
//
// WHAT THE REFERENCE SHELLS DO, measured before choosing, because POSIX defers to
// C's signed-integer rules and so answers nothing:
//
//                                dash                  bash      zsh
//   $((99999999999999999999))    INT64_MAX (clamped)    wrapped   diagnosed, 19 digits kept
//   $((INT64_MAX + 1))           wrapped                wrapped   wrapped
//   $((-INT64_MAX - 2))          wrapped                wrapped   wrapped
//   $((INT64_MAX * 2))           wrapped                wrapped   wrapped
//   $((INT64_MIN / -1))          wrapped                wrapped   wrapped
//
// All three at status 0, and none of them refusing the expression. So:
//
//   - AN OPERATOR WRAPS. All three agree on all four operators and on `/`, and
//     unanimity among the reference shells is the best evidence available where
//     the standard defers. It is also what arithmetic that relies on wrapping -
//     hashing, mixing - is written against. Wrapping is only UB when it is done
//     by overflowing a SIGNED int, which is what the code did; the builtins below
//     produce the same two's-complement value with the overflow defined away.
//   - A LITERAL SATURATES at INT64_MAX. That is dash's answer, in all three bases,
//     and ADR-0001 makes dash the oracle for the floor - dash parses with
//     strtoimax, which clamps and sets ERANGE. It is right on the merits too: an
//     unrepresentable CONSTANT is not an operation that overflowed. It is visible
//     in `$((-9223372036854775808))`, which has no negative literal in it and so
//     is a saturated INT64_MAX negated - dash says -9223372036854775807.
//
// NOTHING HERE FAILS THE EXPRESSION, which is a decision and not an omission. A
// diagnostic would be a FOURTH effect site to gate on `_live` (#56), and the gate
// could not work: the overflow in `$((0 && 99999999999999999999))` happens while
// the skipped operand is PARSED, and #56 requires it to be parsed. An answer that
// cannot report is an answer that cannot report on an operand nobody evaluated.

// True when the operation overflowed. `out` receives the two's-complement result
// either way, so a caller that means to wrap discards the answer.
[[nodiscard]] inline bool add_overflows(int64_t a, int64_t b, int64_t& out) noexcept {
	return __builtin_add_overflow(a, b, &out);
}
[[nodiscard]] inline bool sub_overflows(int64_t a, int64_t b, int64_t& out) noexcept {
	return __builtin_sub_overflow(a, b, &out);
}
[[nodiscard]] inline bool mul_overflows(int64_t a, int64_t b, int64_t& out) noexcept {
	return __builtin_mul_overflow(a, b, &out);
}

// The wrapping forms the operators use: the same computation, answer discarded.
[[nodiscard]] inline int64_t wrap_add(int64_t a, int64_t b) noexcept {
	int64_t out = 0;
	(void)add_overflows(a, b, out);
	return out;
}
[[nodiscard]] inline int64_t wrap_sub(int64_t a, int64_t b) noexcept {
	int64_t out = 0;
	(void)sub_overflows(a, b, out);
	return out;
}
[[nodiscard]] inline int64_t wrap_mul(int64_t a, int64_t b) noexcept {
	int64_t out = 0;
	(void)mul_overflows(a, b, out);
	return out;
}
[[nodiscard]] inline int64_t wrap_neg(int64_t a) noexcept { return wrap_sub(0, a); }

// `/` and `%` overflow in exactly one case, INT64_MIN / -1, whose true quotient is
// one past INT64_MAX. Dividing by -1 IS negation, so the wrapped negation is the
// answer for every operand and not just that one; `a % -1` is 0 for every a. A
// ZERO divisor is not handled here - it is an evaluation error the caller gates on
// `_live` - so these are only ever reached with b non-zero.
[[nodiscard]] inline int64_t wrap_div(int64_t a, int64_t b) noexcept {
	return b == -1 ? wrap_neg(a) : a / b;
}
[[nodiscard]] inline int64_t wrap_mod(int64_t a, int64_t b) noexcept {
	return b == -1 ? 0 : a % b;
}

// Recursive-descent with precedence climbing. The grammar is C's, which POSIX
// requires, so the levels below are C's precedence table read bottom-up.
//
// SHORT-CIRCUITING IS A PARSE WITH ITS EFFECTS OFF, not a subtree left unvisited
// (issue #56). This evaluator computes AS IT PARSES - there is no tree standing
// between the two for `&&` to decline to descend into - so an operand it must
// skip is parsed anyway, with `_live` false: reads answer 0, writes do not
// happen, and a division by zero is not an error.
//
// Building the tree instead was the alternative, and it buys nothing the flag
// does not. The skipped operand has to be PARSED either way, because `$((0 && +))`
// is still a malformed expression - dash, bash and zsh all say so - and the parse
// is the only pass that can find that out. What a tree would cost is an arena in
// evaluate()'s signature; #30 deliberately gave the arithmetic port exactly two
// things, an expression and a variables interface, and every caller would have to
// find an allocator to hand it a node representation nobody reads twice.
//
// What the flag must not do is silence a SYNTAX error, and it does not: only the
// three effect sites consult it - reading a variable, writing one, and dividing
// by zero. `_live` also only ever goes off, never on, so an operand skipped from
// outside stays skipped however its own conditions come out.
class evaluator {
public:
	evaluator(std::string_view text, arithmetic_variables& vars) noexcept
		: _text(text), _vars(vars) {}

	arithmetic_result run() noexcept {
		const int64_t value = parse_comma();
		skip_blanks();
		if (_failed)
			return {0, false, _error, {}, _refused};
		if (_at < _text.size())
			return {0, false, "unexpected character", {}, false};
		return {value, true, nullptr, _unset_name, false};
	}

private:
	std::string_view _text;
	arithmetic_variables& _vars;
	size_t _at = 0;
	bool _failed = false;
	bool _refused = false;
	// False while parsing an operand a short-circuit skipped. See the note above
	// the class: the text is still read, nothing it describes is done.
	bool _live = true;
	const char* _error = nullptr;
	// The first name read that the caller had never set. Only the first, because
	// that is the one `set -u` reports and the evaluation stops mattering after it.
	std::string_view _unset_name;

	[[nodiscard]] int64_t read_variable(std::string_view name) noexcept {
		// An operand that was not evaluated read nothing, so it owes the caller no
		// unset name either: `set -u; echo $((0 && y))` is not a nounset error. zsh,
		// which applies nounset inside arithmetic as lesh does, agrees; dash prints
		// the same 0 for the simpler reason that it never applies nounset there.
		if (!_live)
			return 0;
		if (_unset_name.empty() && !_vars.defined(name))
			_unset_name = name;
		return _vars.get(name);
	}

	void fail(const char* why) noexcept {
		if (!_failed) {
			_failed = true;
			_error = why;
		}
	}

	// Dividing by zero is an evaluation error, not a syntax one, so an operand
	// that is only being parsed does not commit it: `$((0 && 1/0))` is 0 in dash
	// and in zsh. bash reports the error there; dash is the POSIX oracle. True
	// when the caller must abandon what it was computing.
	bool divide_by_zero() noexcept {
		if (!_live)
			return false;
		fail("division by zero");
		return true;
	}

	void skip_blanks() noexcept {
		while (_at < _text.size() && (_text[_at] == ' ' || _text[_at] == '\t' ||
		                              _text[_at] == '\n'))
			++_at;
	}

	[[nodiscard]] char peek(size_t ahead = 0) const noexcept {
		return _at + ahead < _text.size() ? _text[_at + ahead] : '\0';
	}

	bool consume(std::string_view op) noexcept {
		skip_blanks();
		if (_text.compare(_at, op.size(), op) != 0)
			return false;
		// `<` must not swallow the `<` of `<<`, and `=` must not swallow `==`.
		// Checking the longer operators first is not enough on its own, because
		// `<` would still match inside `<=`.
		if (op == "=" && peek(1) == '=')
			return false;
		if ((op == "<" || op == ">") && (peek(1) == '=' || peek(1) == op[0]))
			return false;
		if ((op == "&" || op == "|") && peek(1) == op[0])
			return false;
		_at += op.size();
		return true;
	}

	// Longest-match assignment operators, tried before the plain `=`.
	static constexpr std::string_view kCompound[] = {
		"<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=",
	};

	int64_t parse_comma() noexcept {
		int64_t value = parse_assignment();
		while (consume(",")) value = parse_assignment();
		return value;
	}

	int64_t parse_assignment() noexcept {
		// An assignment needs a NAME on the left, so look ahead rather than
		// parsing a full expression and trying to undo it.
		const size_t save = _at;
		skip_blanks();
		const size_t name_start = _at;
		if (_at < _text.size() &&
		    lesh::string_utils::is_valid_var_name_first_char(
		        static_cast<unsigned char>(_text[_at]))) {
			size_t scan = _at;
			while (scan < _text.size() &&
			       lesh::string_utils::is_valid_var_name_non_first_char(
			           static_cast<unsigned char>(_text[scan])))
				++scan;
			const std::string_view name = _text.substr(name_start, scan - name_start);

			size_t after = scan;
			while (after < _text.size() && (_text[after] == ' ' || _text[after] == '\t'))
				++after;

			for (const auto& op : kCompound) {
				if (_text.compare(after, op.size(), op) == 0) {
					_at = after + op.size();
					const int64_t rhs = parse_assignment();
					const int64_t lhs = read_variable(name);
					const int64_t result = apply_compound(op, lhs, rhs);
					// A readonly variable refuses the write. Failing the whole expression
					// is the only honest answer: `$((x+=1))` that reports a value it did
					// not store would be a lie the caller cannot see through. An
					// assignment that never happened cannot be refused, so a skipped one
					// does not ask.
					if (_live && !_vars.set(name, result)) {
						_refused = true;
						fail("readonly variable");
					}
					return result;
				}
			}
			if (after < _text.size() && _text[after] == '=' &&
			    (after + 1 >= _text.size() || _text[after + 1] != '=')) {
				_at = after + 1;
				const int64_t rhs = parse_assignment();
				if (_live && !_vars.set(name, rhs)) {
					_refused = true;
					fail("readonly variable");
				}
				return rhs;
			}
		}
		_at = save;
		return parse_conditional();
	}

	int64_t apply_compound(std::string_view op, int64_t a, int64_t b) noexcept {
		if (op == "+=") return wrap_add(a, b);
		if (op == "-=") return wrap_sub(a, b);
		if (op == "*=") return wrap_mul(a, b);
		if (op == "/=") { if (b == 0) { divide_by_zero(); return 0; } return wrap_div(a, b); }
		if (op == "%=") { if (b == 0) { divide_by_zero(); return 0; } return wrap_mod(a, b); }
		if (op == "<<=") return static_cast<int64_t>(static_cast<uint64_t>(a) << (b & 63));
		if (op == ">>=") return a >> (b & 63);
		if (op == "&=") return a & b;
		if (op == "^=") return a ^ b;
		if (op == "|=") return a | b;
		return b;
	}

	int64_t parse_conditional() noexcept {
		const int64_t condition = parse_logical_or();
		if (!consume("?"))
			return condition;
		// Exactly one branch runs. Both are parsed, because a malformed branch is a
		// malformed expression whichever way the condition went.
		const bool live = _live;
		if (condition == 0)
			_live = false;
		const int64_t when_true = parse_assignment();
		_live = live;
		if (!consume(":")) {
			fail("expected ':'");
			return 0;
		}
		if (condition != 0)
			_live = false;
		const int64_t when_false = parse_conditional();
		_live = live;
		return condition != 0 ? when_true : when_false;
	}

	int64_t parse_logical_or() noexcept {
		int64_t value = parse_logical_and();
		while (consume("||")) {
			// A true left operand settles the answer, so the right one is skipped -
			// `$((1 || (x=1)))` must not assign.
			const bool live = _live;
			if (value != 0)
				_live = false;
			const int64_t rhs = parse_logical_and();
			_live = live;
			value = (value != 0 || rhs != 0) ? 1 : 0;
		}
		return value;
	}
	int64_t parse_logical_and() noexcept {
		int64_t value = parse_bit_or();
		while (consume("&&")) {
			// Mirror of `||`: a false left operand settles the answer.
			const bool live = _live;
			if (value == 0)
				_live = false;
			const int64_t rhs = parse_bit_or();
			_live = live;
			value = (value != 0 && rhs != 0) ? 1 : 0;
		}
		return value;
	}
	int64_t parse_bit_or() noexcept {
		int64_t value = parse_bit_xor();
		while (consume("|")) value |= parse_bit_xor();
		return value;
	}
	int64_t parse_bit_xor() noexcept {
		int64_t value = parse_bit_and();
		while (consume("^")) value ^= parse_bit_and();
		return value;
	}
	int64_t parse_bit_and() noexcept {
		int64_t value = parse_equality();
		while (consume("&")) value &= parse_equality();
		return value;
	}
	int64_t parse_equality() noexcept {
		int64_t value = parse_relational();
		for (;;) {
			if (consume("==")) value = value == parse_relational() ? 1 : 0;
			else if (consume("!=")) value = value != parse_relational() ? 1 : 0;
			else return value;
		}
	}
	int64_t parse_relational() noexcept {
		int64_t value = parse_shift();
		for (;;) {
			if (consume("<=")) value = value <= parse_shift() ? 1 : 0;
			else if (consume(">=")) value = value >= parse_shift() ? 1 : 0;
			else if (consume("<")) value = value < parse_shift() ? 1 : 0;
			else if (consume(">")) value = value > parse_shift() ? 1 : 0;
			else return value;
		}
	}
	int64_t parse_shift() noexcept {
		int64_t value = parse_additive();
		for (;;) {
			if (consume("<<"))
				value = static_cast<int64_t>(static_cast<uint64_t>(value) << (parse_additive() & 63));
			else if (consume(">>"))
				value >>= (parse_additive() & 63);
			else return value;
		}
	}
	int64_t parse_additive() noexcept {
		int64_t value = parse_multiplicative();
		for (;;) {
			if (consume("+")) value = wrap_add(value, parse_multiplicative());
			else if (consume("-")) value = wrap_sub(value, parse_multiplicative());
			else return value;
		}
	}
	int64_t parse_multiplicative() noexcept {
		int64_t value = parse_unary();
		for (;;) {
			if (consume("*")) {
				value = wrap_mul(value, parse_unary());
			} else if (consume("/")) {
				const int64_t divisor = parse_unary();
				if (divisor == 0) {
					if (divide_by_zero())
						return 0;
					// Returning would abandon the rest of a skipped operand mid-text, and
					// what was left of it would fail the expression that skipped it.
					value = 0;
				} else {
					value = wrap_div(value, divisor);
				}
			} else if (consume("%")) {
				const int64_t divisor = parse_unary();
				if (divisor == 0) {
					if (divide_by_zero())
						return 0;
					value = 0;
				} else {
					value = wrap_mod(value, divisor);
				}
			} else {
				return value;
			}
		}
	}
	int64_t parse_unary() noexcept {
		skip_blanks();
		if (consume("!")) return parse_unary() == 0 ? 1 : 0;
		if (consume("~")) return ~parse_unary();
		if (consume("-")) return wrap_neg(parse_unary());
		if (consume("+")) return parse_unary();
		return parse_primary();
	}

	int64_t parse_primary() noexcept {
		skip_blanks();
		if (_at >= _text.size()) {
			fail("unexpected end of expression");
			return 0;
		}
		if (consume("(")) {
			const int64_t value = parse_comma();
			if (!consume(")"))
				fail("expected ')'");
			return value;
		}

		const char c = peek();
		if (c >= '0' && c <= '9')
			return parse_number();

		if (lesh::string_utils::is_valid_var_name_first_char(static_cast<unsigned char>(c))) {
			const size_t start = _at;
			while (_at < _text.size() &&
			       lesh::string_utils::is_valid_var_name_non_first_char(
			           static_cast<unsigned char>(_text[_at])))
				++_at;
			// Variables are read WITHOUT `$` inside arithmetic, and an unset or
			// non-numeric one is 0 rather than an error - which is what lets
			// `i=$((i+1))` work without initialising i.
			return read_variable(_text.substr(start, _at - start));
		}

		fail("unexpected character");
		return 0;
	}

	int64_t parse_number() noexcept {
		// C bases, as POSIX requires: 0x hex, leading 0 octal, otherwise decimal.
		int base = 10;
		if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
			base = 16;
			_at += 2;
		} else if (peek() == '0' && peek(1) >= '0' && peek(1) <= '7') {
			base = 8;
			++_at;
		}

		int64_t value = 0;
		bool any = base != 16;  // "0" alone is a valid decimal zero
		bool saturated = false;
		while (_at < _text.size()) {
			const char c = _text[_at];
			int digit;
			if (c >= '0' && c <= '9') digit = c - '0';
			else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
			else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
			else break;
			if (digit >= base) break;
			// A literal too large to represent SATURATES at INT64_MAX, which is dash's
			// answer in each of the three bases. The digits are consumed either way:
			// the literal ends where it ends, and stopping at the one that overflowed
			// would leave the rest of it behind to be parsed as though it were
			// operators, turning a representable answer into a syntax error.
			int64_t scaled = 0;
			if (mul_overflows(value, base, scaled) || add_overflows(scaled, digit, value))
				saturated = true;
			any = true;
			++_at;
		}
		if (saturated)
			value = INT64_MAX;
		if (!any)
			fail("malformed number");
		return value;
	}
};

} // namespace

arithmetic_result evaluate(std::string_view expression,
                           arithmetic_variables& vars) noexcept {
	evaluator e{expression, vars};
	return e.run();
}

} // namespace lesh::runtime
