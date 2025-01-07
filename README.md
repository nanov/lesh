# lesh - a mess of a shell

```
      ..               .x+=:.
x .d88"               z`    ^%    .uef^"
 5888R                   .   <k :d88E
 '888R        .u       .@8Ned8" `888E
888R     ud8888.   .@^%8888"   888E .z8k
888R   :888'8888. x88:  `)8b.  888E~?888L
  888R   d888 '88%" 8888N=*8888  888E  888E
  888R   8888.+"     %8"    R88  888E  888E
  888R   8888L        @8Wou 9%   888E  888E
 .888B . '8888c. .+ .888888P`    888E  888E
^*888%   "88888%   `   ^"F     m888N= 888>
   "%       "YP'                 `Y"   888
J88"
@%
:"
```

## NOTES:

- https://zsh.sourceforge.io/Doc/Release/Prompt-Expansion.html#Prompt-Expansion - implement
- https://github.com/AmokHuginnsson/replxx - read line but done right
- https://edw.is/using-lua-with-cpp/ - future lua integration
- https://github.com/rothgar/mastering-zsh/blob/master/docs/helpers/aliases.md - important regarding aliases

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