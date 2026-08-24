# Input the shell must REFUSE, against dash (the POSIX floor).
#
# This file exists because of #47: every construct below was accepted in silence
# and the input truncated, at status zero. `lesh -c "echo it's"` printed `it`.
# The parser had recorded the defect on the word all along; the check that refuses
# to execute a tree with errors tested only the node's KIND, so a word CARRYING an
# error was not an error.
#
# TWO CHANNELS, ORTHOGONAL. `tree::incomplete()` says more input would complete
# the construct; `tree::has_errors()` says the tree as it stands is defective. An
# unterminated quote is both, and a shell that already holds its whole input has
# nothing to continue - so it diagnoses. A TRAILING BACKSLASH and an UNTERMINATED
# HERE-DOCUMENT are incomplete WITHOUT being defective, and dash runs both at
# status zero: the two cases at the end of this file are what stop the fix from
# being "incomplete implies error", and the here-document one is what #21 relies
# on.
#
# Only the emptiness of stderr is compared, never its text - the wording is
# lesh's own (`lesh: syntax error: unterminated quoted string`), and dash's names
# the same constructs differently.
#
# Every case that asserts a REFUSAL is xfail on the legacy front end. Legacy's
# single-pass parser has no error channel at all: it truncates the input at the
# unterminated construct and reports success, which is the bug in the first place.
# The trailing backslash is the one case legacy already gets right, so it carries
# no marker.

--- an unterminated single quote is a syntax error [xfail(legacy): legacy truncates the input at the quote and reports success]
echo it's

--- an unterminated double quote is a syntax error [xfail(legacy): legacy truncates the input at the quote and reports success]
echo "unterminated

--- an unterminated command substitution is a syntax error [xfail(legacy): legacy has no error channel for an unterminated construct]
echo $(

--- an unterminated backquote is a syntax error [xfail(legacy): legacy has no error channel for an unterminated construct]
echo `

--- an unterminated parameter expansion is a syntax error [xfail(legacy): legacy has no error channel for an unterminated construct]
echo ${x

--- an unterminated arithmetic expansion is a syntax error [xfail(legacy): legacy has no error channel for an unterminated construct]
echo $((1

--- an unterminated assignment prefix is a syntax error [xfail(legacy): legacy runs the rest of the input regardless]
x="abc
echo not reached

--- an unterminated redirection target is a syntax error rather than a bad filename [xfail(legacy): legacy passes the operator through as an argument]
cat > "x

--- an unterminated word in a for list is a syntax error [xfail(legacy): legacy has no for loop]
for i in "a; do echo $i; done

--- an unterminated case pattern is a syntax error [xfail(legacy): legacy has no case clause]
case a in "a) echo matched;; esac

--- an unterminated here-document delimiter is a syntax error [xfail(legacy): legacy has no here-documents]
cat <<"EOF
body
EOF

--- an unterminated quote inside eval kills the shell [xfail(legacy): legacy has no eval]
eval 'echo "x'
echo not reached

--- a syntax error stops the input rather than truncating one command [xfail(legacy): legacy truncates the command and carries on]
echo one
echo "two
echo three

--- a syntax error stops the input read from standard input [stdin] [xfail(legacy): legacy truncates the command and carries on]
echo one
echo $(
echo three

--- a syntax error stops a script read from a file [xfail(legacy): legacy never expands a parameter inside double quotes, so "$TESTEE" is not a command it can run]
d=$(mktemp -d); printf 'echo one\necho "two\necho three\n' > $d/s; "$TESTEE" $d/s; echo "status=$?"

# MALFORMED NESTING THE WORD SCAN CANNOT SEE (#48). Everything above is refused by
# the parser, because the defect is on a word and `has_errors()` finds it. These
# are not: `${x-$((1}` is well formed AT THE COMMAND LEVEL - the word scan counts
# braces, so the `}` closes the parameter expansion and the unterminated `$((`
# inside the default is never lexed until the default is EXPANDED. Every case here
# aborted the shell before this fix, on a stack overflow: the expander stripped
# `$((` and `))` from a segment too short to hold them and re-expanded the same
# bytes until the stack ran out.
#
# So these are refused by the EXPANDER, with the parser's own wording, from the
# same table of phrases. dash refuses them at parse time; the last two cases of
# this section are where that difference is visible, and they are marked as the
# divergences they are rather than left to be discovered.
#
# Unterminated QUOTES inside a default are deliberately NOT refused - the
# second-to-last case says why. That is #42's, not this one's.

--- an unterminated arithmetic expansion inside a parameter default is refused [xfail(legacy): legacy never expands a parameter, so it echoes the braces as literal text]
echo ${x-$((1}

--- an unterminated arithmetic expansion inside a nested default is refused [xfail(legacy): legacy never expands a parameter, so it echoes the braces as literal text]
echo ${x-${y-$((1}}

--- an unterminated arithmetic expansion in an alternate value is refused [xfail(legacy): legacy never expands a parameter, so it echoes the braces as literal text]
x=1; echo ${x+$((1}

--- an unterminated arithmetic expansion in a trim pattern is refused [xfail(legacy): legacy never expands a parameter, so it echoes the braces as literal text]
x=abc; echo ${x#$((1}

--- an unterminated command substitution inside a parameter default is refused [xfail(legacy): legacy never expands a parameter, so it echoes the braces as literal text]
echo ${x-$(}

--- an unterminated backquote inside a parameter default is refused [xfail(legacy): legacy never expands a parameter, so it echoes the braces as literal text]
echo ${x-`}

--- an unterminated parameter expansion inside arithmetic is refused [xfail(legacy): legacy never expands arithmetic, so it echoes the parens as literal text]
echo $((${x-))

--- a malformed expansion in an assignment value is refused [xfail(legacy): legacy never expands a parameter, so the assignment takes the braces as its value]
v=${x-$((1}
echo not reached

--- a malformed expansion in a redirection target is refused [xfail(legacy): legacy never expands a parameter, so it creates a file named after the braces]
cat > ${x-$((1}

--- a TERMINATED arithmetic expansion inside a parameter default still expands [xfail(legacy): legacy never expands a parameter, so it echoes the braces as literal text]
echo ${x-$((1+2))}

--- a TERMINATED command substitution inside a parameter default still expands [xfail(legacy): legacy never expands a parameter, so it echoes the braces as literal text]
echo ${x-`echo hi`}

--- an unterminated quote inside a parameter default is refused [xfail(legacy): legacy has no parameter expansion beyond $name, so it echoes the braces]
echo ${x-'a}

--- a single quote inside a double-quoted parameter default is a byte, not a quote [xfail(legacy): legacy has no parameter expansion beyond $name, so it echoes the braces]
echo "${x-'}"

--- a malformed expansion in a for list stops the shell [xfail(legacy): legacy has no compound commands]
for i in ${x-$((1}; do echo $i; done

--- but a FATAL expansion error in a for list still does not [xfail: #39's remaining gap - `run_for` and `run_case` never consult expander::fatal_error(), so `${x?}` there is reported and then ignored while dash exits 2. Reached only by an error the WORD SCAN cannot see: a malformed nesting is now refused at parse time, and this is not malformed]
for i in ${x?}; do echo $i; done; echo after

--- a default the operator never reaches is refused too [xfail(legacy): legacy has no parameter expansion beyond $name]
x=1; echo ${x-$((1}

--- a trailing backslash is incomplete without being an error
echo one\

--- an unterminated here-document is incomplete without being an error [xfail(legacy): legacy has no here-documents]
cat <<EOF
body
