# Autobuild

How LumaVAULT's nightly build (bigguy, 23:00 to 06:00) builds this repository. The form is in lumavault-server
`docs/AUTOBUILD.md`.

## gfopenservice

This repository's part is built every night inside the GFOpenUp pack (DalsinAI/gfopenup's `gfopenup-pack`), from this
repository's main, so it has no build of its own here and the nightly lists it as never. When it needs a build apart
from the pack (a test run, a release of its own), give this section a script, a frequency and what to keep.

- frequency: never
- kind: part
- notes: built every night in gfopenup-pack
