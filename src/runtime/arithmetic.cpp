#include "runtime/arithmetic.h"

#include "substrate/char_utils.h"

#include <string>

namespace lesh::runtime {

namespace {

// Recursive-descent with precedence climbing. The grammar is C's, which POSIX
// requires, so the levels below are C's precedence table read bottom-up.
class evaluator {
public:
	evaluator(std::string_view text, arithmetic_variables& vars) noexcept
		: _text(text), _vars(vars) {}

	arithmetic_result run() noexcept {
		const int64_t value = parse_comma();
		skip_blanks();
		if (_failed)
			return {0, false, _error};
		if (_at < _text.size())
			return {0, false, "unexpected character"};
		return {value, true, nullptr};
	}

private:
	std::string_view _text;
	arithmetic_variables& _vars;
	size_t _at = 0;
	bool _failed = false;
	const char* _error = nullptr;

	void fail(const char* why) noexcept {
		if (!_failed) {
			_failed = true;
			_error = why;
		}
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
					const int64_t lhs = _vars.get(name);
					const int64_t result = apply_compound(op, lhs, rhs);
					_vars.set(name, result);
					return result;
				}
			}
			if (after < _text.size() && _text[after] == '=' &&
			    (after + 1 >= _text.size() || _text[after + 1] != '=')) {
				_at = after + 1;
				const int64_t rhs = parse_assignment();
				_vars.set(name, rhs);
				return rhs;
			}
		}
		_at = save;
		return parse_conditional();
	}

	int64_t apply_compound(std::string_view op, int64_t a, int64_t b) noexcept {
		if (op == "+=") return a + b;
		if (op == "-=") return a - b;
		if (op == "*=") return a * b;
		if (op == "/=") { if (b == 0) { fail("division by zero"); return 0; } return a / b; }
		if (op == "%=") { if (b == 0) { fail("division by zero"); return 0; } return a % b; }
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
		const int64_t when_true = parse_assignment();
		if (!consume(":")) {
			fail("expected ':'");
			return 0;
		}
		const int64_t when_false = parse_conditional();
		return condition != 0 ? when_true : when_false;
	}

	int64_t parse_logical_or() noexcept {
		int64_t value = parse_logical_and();
		while (consume("||")) {
			const int64_t rhs = parse_logical_and();
			value = (value != 0 || rhs != 0) ? 1 : 0;
		}
		return value;
	}
	int64_t parse_logical_and() noexcept {
		int64_t value = parse_bit_or();
		while (consume("&&")) {
			const int64_t rhs = parse_bit_or();
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
			if (consume("+")) value += parse_multiplicative();
			else if (consume("-")) value -= parse_multiplicative();
			else return value;
		}
	}
	int64_t parse_multiplicative() noexcept {
		int64_t value = parse_unary();
		for (;;) {
			if (consume("*")) {
				value *= parse_unary();
			} else if (consume("/")) {
				const int64_t divisor = parse_unary();
				if (divisor == 0) { fail("division by zero"); return 0; }
				value /= divisor;
			} else if (consume("%")) {
				const int64_t divisor = parse_unary();
				if (divisor == 0) { fail("division by zero"); return 0; }
				value %= divisor;
			} else {
				return value;
			}
		}
	}
	int64_t parse_unary() noexcept {
		skip_blanks();
		if (consume("!")) return parse_unary() == 0 ? 1 : 0;
		if (consume("~")) return ~parse_unary();
		if (consume("-")) return -parse_unary();
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
			return _vars.get(_text.substr(start, _at - start));
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
		while (_at < _text.size()) {
			const char c = _text[_at];
			int digit;
			if (c >= '0' && c <= '9') digit = c - '0';
			else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
			else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
			else break;
			if (digit >= base) break;
			value = value * base + digit;
			any = true;
			++_at;
		}
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
