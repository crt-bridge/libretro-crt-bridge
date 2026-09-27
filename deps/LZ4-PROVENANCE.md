<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Provenance -- `lz4.c` / `lz4.h`

`deps/lz4.c` and `deps/lz4.h` are the same bytes as the copy of lz4 embedded in the `libgm`
library: https://github.com/crt-bridge/libgm

**Origin:** [`github.com/lz4/lz4`](https://github.com/lz4/lz4), tag `v1.10.0`, `lib/` folder.

**License:** BSD-2-Clause for the contents of `lib/` (these two files). The `lz4/lz4`
repository's CLI and test programs are GPL-2.0-or-later and are not embedded here -- only
`lib/lz4.c` and `lib/lz4.h` are.

**SHA-256 fingerprints, verified now:**

| file | SHA-256 |
|---|---|
| `lz4.c` | `9396f7de527bc8435de9c7569fb7998e56545a84b4f3c2d808c0235c01774539` |
| `lz4.h` | `26b82efc53d1570f3b54eef02e9c4764c1ad374ff03cac04e2ced5ea4d4c552f` |

Any mismatch on these fingerprints must stop any further use: third-party code is never
embedded here without knowing exactly what it is.
