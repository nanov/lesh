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

--- a trailing backslash is incomplete without being an error
echo one\

--- an unterminated here-document is incomplete without being an error [xfail(legacy): legacy has no here-documents]
cat <<EOF
body
