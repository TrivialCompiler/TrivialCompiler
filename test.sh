#!/bin/bash

ARCH=`arch`
if [ "$ARCH" = "x86_64" ]; then
    RUN="timeout -v 60 qemu-arm"
else
    RUN="timeout -v 60 sh -c"
fi

set -v
rm -rf "$3"

# run program with QEMU
if [ -e "$2" ]; then
    $RUN "$1" < "$2" > "$3"
else
    $RUN "$1" > "$3"
fi

res=$?

if [ -z "$(tail -c 1 "$3")" ]
then
    # newline at eof
    echo "${res}" >> "$3"
else
    # no newline at eof
    echo -en "\n${res}" >> "$3"
fi

diff -w -B -u "$3" "$4"

