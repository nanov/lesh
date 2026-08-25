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

--- double quotes allow expansion
echo "quoted $HOME"

--- command substitution
echo $(echo inner)

--- command substitution with a prefix
echo pre$(echo inner)

--- expansion of an argument inside a pipeline
echo $HOME | cat

--- expansion with a prefix inside a pipeline
echo pre$HOME | cat

--- pipeline in the new front end
echo a | cat

--- command substitution in the new front end
echo $(echo inner)
