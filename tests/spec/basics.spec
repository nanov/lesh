# Command execution, pipelines and exit status, against dash.

--- simple command
echo hello

--- multiple arguments
echo one two three

--- exit status of a successful command
true

--- exit status of a failing command
false

--- command not found is 127
definitely_not_a_real_command_xyz

--- pipeline passes data
echo hello | cat

--- pipeline status is the last command's
true | false

--- pipeline status is the last command's, inverted
false | true

--- single quotes suppress expansion
echo 'literal $HOME'

--- double quotes allow expansion [xfail: quote handling strips the brackets but never runs expansion on the contents]
echo "quoted $HOME"

--- command substitution
echo $(echo inner)

--- command substitution with a prefix [xfail: partial expansion around $() is unfinished]
echo pre$(echo inner)

--- expansion of an argument inside a pipeline [xfail: the expanded word replaces the command name instead of becoming an argument]
echo $HOME | cat

--- expansion with a prefix inside a pipeline [xfail: same defect as above]
echo pre$HOME | cat
