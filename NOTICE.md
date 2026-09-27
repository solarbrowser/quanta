# Notice

_Last updated: 2026-09-26_

Quanta itself is licensed under the Mozilla Public License 2.0 (see
[`LICENSE`](./LICENSE)). This file lists the third-party software bundled in
this repository (under `third_party/`) and the trademarks referenced by its
documentation, so that attribution and license terms are easy to find in one
place. It does not replace or modify any license below — where this file and
a component's own license text disagree, that component's own license file
(linked below) is authoritative.

## Third-party software

### PCRE2 — `third_party/pcre2/`

- **License:** BSD-3-Clause WITH PCRE2-exception for the basic library (see
  [`third_party/pcre2/LICENCE.md`](./third_party/pcre2/LICENCE.md) for the
  full text and the binary-redistribution exemption it refers to).
- **Copyright:** Basic library: Copyright (c) 1997-2007 University of
  Cambridge; Copyright (c) 2007-2024 Philip Hazel.
- **PCRE2's own JIT compilation support** (the glue between PCRE2 and sljit,
  below) is separately copyrighted: Copyright (c) 2010-2024 Zoltan Herczeg,
  also under the terms in `LICENCE.md`.
- **The Stack-less JIT compiler (sljit)** — `third_party/pcre2/deps/sljit/` —
  is a separate upstream project vendored underneath PCRE2, with its own
  2-clause BSD license file:
  [`third_party/pcre2/deps/sljit/LICENSE`](./third_party/pcre2/deps/sljit/LICENSE).
  **Copyright:** Copyright (c) 2009-2024 Zoltan Herczeg.
- **Used for:** regular expression matching (`RegExp`).

### mimalloc — `third_party/mimalloc/`

- **License:** MIT (see
  [`third_party/mimalloc/LICENSE`](./third_party/mimalloc/LICENSE)).
- **Copyright:** Copyright (c) 2018-2025 Microsoft Corporation, Daan Leijen.
- **Used for:** the general-purpose allocator backing non-GC-cell allocations
  (property storage, string internals, array elements). Not linked into the
  ASan build (see `docs/internals/memory.md` / `docs/contributing/debugging.md`
  for why).

### utf8proc — `third_party/utf8proc/`

Three separate licenses apply, all reproduced in full in
[`third_party/utf8proc/LICENSE.md`](./third_party/utf8proc/LICENSE.md):

- **License:** MIT, for all work on the library since it passed to its current
  maintainers.
  **Copyright:** Copyright (c) 2014-2021 Steven G. Johnson, Jiahao Chen, Tony
  Kelman, Jonas Fonseca, and contributors.
- **License:** MIT, for the original utf8proc library.
  **Copyright:** Copyright (c) 2009, 2013 Public Software Group e.V., Berlin,
  Germany (Jan Behrens and the rest of the Public Software Group).
- **License:** the Unicode data files' own permission notice (not MIT — see
  the license file for its exact terms), covering `utf8proc_data.c`, which is
  derived from Unicode's own data files.
  **Copyright:** Copyright (c) 1991-2007 Unicode, Inc.
  "Unicode" and the Unicode logo are trademarks of Unicode, Inc., and may be
  registered in some jurisdictions.
- **Used for:** Unicode normalization and related string-processing tables.

### minicoro — `third_party/minicoro/`

- **License:** dual-licensed, your choice of Unlicense (public domain) or MIT
  No Attribution (see the license block at the end of
  [`third_party/minicoro/minicoro.h`](./third_party/minicoro/minicoro.h)).
- **Copyright:** Copyright (c) 2021-2023 Eduardo Bart.
- **Note:** the header states that "some of the following assembly code is
  taken from LuaCoco by Mike Pall" for the assembly context-switch path,
  under LuaCoco's own MIT license (Copyright (C) 2004-2016 Mike Pall),
  reproduced inline in that section of the header.
- **Used for:** stackful coroutines (generators/async execution).

## Trademark notice

"JavaScript" is a trademark or registered trademark of Oracle Corporation
(originally registered by Sun Microsystems, which Oracle later acquired) in
the United States and other countries. Quanta implements the ECMAScript
Language Specification (ECMA-262, maintained by Ecma International); this
project's documentation and commit history use "JavaScript"/"JS" and
"ECMAScript" interchangeably in the ordinary, descriptive sense used
throughout the software industry to refer to the language the spec defines —
not to claim any affiliation with, sponsorship by, or endorsement from Oracle
or Ecma International.

This section is provided for informational clarity, not as a legal
determination; if trademark compliance matters for how you distribute or
market a build of Quanta, consult your own counsel.
