# Vendored foo2zjs sources

These files are copied **unmodified** from foo2zjs by Rick Richardson,
using the GitHub mirror at <https://github.com/koenkooi/foo2zjs>, commit
`e04290de6b7a30d588f3411fd9834618e09f7b9b` (2021-04-26).

| File | Purpose | Author / License |
|---|---|---|
| `foo2zjs.c`, `zjs.h` | PBM to ZjStream encoder | Rick Richardson et al., GPL-2.0-or-later |
| `zjsdecode.c` | ZjStream decoder (used by `make test`) | Rick Richardson, GPL-2.0-or-later |
| `jbig.c`, `jbig.h`, `jbig_ar.c`, `jbig_ar.h` | JBIG-KIT compression | Markus Kuhn, GPL-2.0-or-later |

Only the files needed to build the P1102 encoder are included. Upstream also
ships printer firmware images and vendor ICC color profiles under other
copyrights. None of those are included or needed: the P1102/P1102w keeps its
firmware in flash, and this driver is monochrome.

The full GPL v2 text is in [`../../LICENSE`](../../LICENSE).
