# Vendored: libebur128

Source: https://github.com/jiixyj/libebur128
Commit: `67b33abe1558160ed76ada1322329b0e9e058b02` (2021-02-14, the repo's
default-branch HEAD as of this vendoring on 2026-09-02)
License: MIT (see `LICENSE` in this directory -- copied verbatim from the
upstream repo's own `COPYING` file, unmodified).

Files copied unmodified from `ebur128/`: `ebur128.c`, `ebur128.h`,
`queue/sys/queue.h` (a small vendored BSD `<sys/queue.h>` replacement the
library itself ships, since that header isn't available on MSVC/Windows --
not something added for this project). Nothing in this directory has been
edited; only `ebur128.c`'s CMake `#include` path assumes `queue/sys/queue.h`
sits where the upstream repo already puts it, which it does here too.

Used by `Source/MasterMeter.h` for real EBU R128 loudness/true-peak
measurement on the Master page. See `STATUS.md`'s corresponding section for
what's built on top of it and how it was verified.
