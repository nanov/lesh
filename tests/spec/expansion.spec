# Parameter expansion, against dash (the POSIX floor).
#
# xfail markers record what is known broken, with the cause. They are the score,
# not an excuse: a marked case that starts passing is reported as XPASS and fails
# the run until its marker is removed.

--- bare parameter
echo $HOME

--- parameter with prefix
echo pre$HOME

--- parameter followed by a separate word
echo $HOME post

--- braced parameter with suffix
echo ${HOME}x

--- braced parameter with prefix and suffix
echo a${HOME}b

--- braced unset parameter
echo ${NOPE}y

--- parameter with a suffix [xfail: the name scan runs past the parameter and swallows the suffix]
echo a$HOME-b

--- parameter with a dot suffix [xfail: same name-scan defect]
echo a$HOME.b

--- parameter followed by a path separator [xfail: same name-scan defect]
echo $HOME/sub

--- unset parameter with prefix and suffix [xfail: same name-scan defect]
echo x$NOPE-y

--- two parameters in one word [xfail: the second expansion overwrites rather than appends]
echo $HOME$HOME

--- two braced parameters around a literal [xfail: the second expansion overwrites rather than appends]
echo ${HOME}x${HOME}

--- parameters and literals alternating [xfail: both name-scan and accumulation defects]
echo a$HOME-$HOME-b
