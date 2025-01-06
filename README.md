# lesh - a mess of a shell


## NOTES:

- https://zsh.sourceforge.io/Doc/Release/Prompt-Expansion.html#Prompt-Expansion - implement
- https://github.com/AmokHuginnsson/replxx - read line but done right
- https://edw.is/using-lua-with-cpp/ - future lua integration

## TODOS:

### Command parsing:

- [X] pipes ( ```ls -lA | grep R```)
- [X] aliases
- [ ] respect brackets (' or ")
  - [X] basic brackets support
  - [X] partial expansion support ( mi'tko' and 'mi'tko )
  - [ ] Support escaping inside brackets
- [ ] full subshell support with partial expanding ie: ```mi$(echo ko)```
- [ ] list expansion : ```mi{tko,la,rovene} -> mitko, mila, mirovene```