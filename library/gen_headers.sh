#!/bin/sh
# mitchy.library's headers and FD file, made from sfd/mitchy_lib.sfd with
# sfdc (the os32-gcc16 stove has it). Run after changing the SFD; the
# results are kept in the repository so a program needs no sfdc.
#   OS32_GCC16  compiler root holding prefix/ (default ~/AmiCradle/stoves/os32-gcc16)
# MIT, Copyright (c) 2026 AmiCradle Team.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
SFDC=${SFDC:-${OS32_GCC16:-"$HOME/AmiCradle/stoves/os32-gcc16"}/prefix/bin/sfdc}
cd "$HERE"
mkdir -p include/clib include/proto include/inline include/pragmas fd
"$SFDC" -q --mode=clib -o include/clib/mitchy_protos.h sfd/mitchy_lib.sfd
"$SFDC" -q --mode=proto -o include/proto/mitchy.h sfd/mitchy_lib.sfd
"$SFDC" -q --mode=macros -o include/inline/mitchy.h sfd/mitchy_lib.sfd
"$SFDC" -q --mode=pragmas -o include/pragmas/mitchy_pragmas.h sfd/mitchy_lib.sfd
"$SFDC" -q --mode=fd -o fd/mitchy_lib.fd sfd/mitchy_lib.sfd
# sfdc's boilerplate names Amiga, Inc.; these files are ours, MIT.
for f in include/clib/mitchy_protos.h include/pragmas/mitchy_pragmas.h; do
    sed -i 's/Copyright (c) 2001 Amiga, Inc\./Copyright (c) 2026 AmiCradle Team, MIT licence./; s/All Rights Reserved/(made by sfdc from sfd\/mitchy_lib.sfd)/' "$f"
done
