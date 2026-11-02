# Restart: Nursery (gfopenservice)

_Written 6 October 2026 at about 23:55 UTC, while all work is paused on @SacredTrees's word (23:28 UTC). Read this first when work resumes; the newest capsule and the live PR list win if they disagree._

## What this repo is

Nursery: gfopenservice.device on the Amiga, the boards that carry services, the paired Cradles on the LAN and the services they run (media.decode/1, doc.render/1, opentls.key/1).

## Where it stands

media.decode/1 draws SVG and decodes icons and animated PNG/GIF; doc.render/1 uses Apache OpenOffice when that is installed. The datatypes lean on it. LICENSE points to a THIRD_PARTY_NOTICES.md that does not exist yet.

## Merged lately

- #4 (427b819, 2026-10-06): Credit who made Nursery: CONTRIBUTORS.md
- #3 (4ea4770, 2026-10-06): media.decode/1: icons, and animated PNG and GIF as video
- #2 (b42d1fe, 2026-10-05): doc.render/1: use Apache OpenOffice when that is the installed office
- #1 (b855d97, 2026-10-05): media.decode/1: SVG drawn by librsvg at the size asked

## Open pull requests

- None.

## Next step

1. Add THIRD_PARTY_NOTICES.md (TweetNaCl, poly1305-donna).
2. TLS key maths to the services card for GFOpenBrowser.

## Waiting on @SacredTrees

- Nothing.

## Who owns it

Datatypes thread; JSC thread for TLS.

## Capsules

Restart capsules for this repo's workstreams, in amicradle's `capjumps/` shelf:

- [`20261006_AmigaChrome_Datatypes_OpenPlay_Restart_Capsule.zip`](https://github.com/DalsinAI/amicradle/tree/main/capjumps)
- [`20261006_AmigaChrome_OpenBrowser_JSC_Restart_Capsule.zip`](https://github.com/DalsinAI/amicradle/tree/main/capjumps)

Team rules that still hold: commits as SacredTrees with no co-author lines; third-party code only on "yes with review" (licence checked, commit and sha256 pinned, fetched at build, never committed); deploys with deploy_dev.py only, on a typed line.
