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

--- pipeline passes data [xfail(next): pipelines need fd plumbing plus a shared process group]
echo hello | cat

--- pipeline status is the last command's [xfail(next): same]
true | false

--- pipeline status is the last command's, inverted [xfail(next): same]
false | true

--- single quotes suppress expansion
echo 'literal $HOME'

--- double quotes allow expansion [xfail(legacy): quote handling strips the brackets but never runs expansion on the contents]
echo "quoted $HOME"

--- command substitution [xfail(next): needs the pipeline plumbing]
echo $(echo inner)

--- command substitution with a prefix [xfail: partial expansion around $() is unfinished]
echo pre$(echo inner)

--- expansion of an argument inside a pipeline [xfail(next): same]
echo $HOME | cat

--- expansion with a prefix inside a pipeline [xfail(next): same]
echo pre$HOME | cat

--- pipeline in the new front end [xfail(next): pipelines need fd plumbing plus a shared process group]
echo a | cat

--- command substitution in the new front end [xfail(next): needs the pipeline plumbing above]
echo $(echo inner)
