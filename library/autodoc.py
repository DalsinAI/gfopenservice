#!/usr/bin/env python3
"""The autodoc of a library from its source: each comment from a line
"/****** name.library/Function ***" to the next line of asterisks, sorted by
name, after a table of contents, as the NDK's autodocs are laid out.

  python3 library/autodoc.py library/mitchy_lib.c > docs/mitchy.doc

MIT, Copyright (c) 2026 AmiCradle Team."""
import re
import sys


def main(path):
    text = open(path, encoding="latin-1").read()
    entries = {}
    for m in re.finditer(r"^/\*{6} (\S+/\S+) \*+\n(.*?)^\*{20,}\n", text, re.M | re.S):
        lines = [re.sub(r"^\*( {3})?", "", line) for line in m.group(2).rstrip("\n").split("\n")]
        entries[m.group(1)] = "\n".join(lines).rstrip() + "\n"
    if not entries:
        sys.exit("no autodoc entries in " + path)
    out = ["TABLE OF CONTENTS", ""]
    out += sorted(entries)
    for name in sorted(entries):
        out += ["\f" + name, "", entries[name]]
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
