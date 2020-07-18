#!/bin/sh
set -v
rm -rf $3
if [ -e "$2" ]; then
    qemu-arm $1 < $2 > $3
else
    qemu-arm $1 > $3
fi
echo $? >> $3
diff -u $3 $4
