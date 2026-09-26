# Licensing of Madeira's modifications

This repository is a fork of [DXMT](https://github.com/3Shain/DXMT). **The upstream licence
is unchanged and continues to apply to all upstream code.** See `LICENSE`.

## What is licensed how

| Code | Licence |
|---|---|
| All upstream DXMT code | as in `LICENSE` (MIT) — unchanged |
| Modifications and new files authored for **Madeira** by Will Faust | **GPL-3.0-or-later** |
| Modifications and new files contributed for **Madeira** by 125hz | **GPL-3.0-or-later**, with the Madeira Converter Exception below |

Where a file contains both, the file as a whole may only be distributed under
terms compatible with GPL-3.0-or-later, because the GPL-covered contributions
cannot be separated from it. The underlying upstream code remains available
under MIT **from upstream**, and nothing here withdraws that.

## Why

The intent is that derivatives of this work which are *distributed* remain open
source. MIT permits a proprietary derivative; the GPL does not. MIT is
GPL-compatible, so combining them this way is permitted.

Three limits, stated plainly rather than left implied:

- The GPL constrains **distribution**. It does not restrict private
  modification, internal use, or a separate program that merely invokes this
  one.
- **Modifications published earlier, while this repository presented itself as
  MIT, were granted under MIT. That grant cannot be revoked.** Only
  contributions made from 2026-08-28 onward are GPL-only. Anyone who already has a
  copy keeps their MIT rights to it.
- It cannot stop anyone independently reimplementing the same ideas.

## Contributing

Contributions are accepted under **GPL-3.0-or-later**. See `CONTRIBUTING.md`.

## Additional permission for the Apple Metal Shader Converter (adopted 2026-09-24)

The Madeira-authored modifications and new files in this repository (the
commits by Will Faust) are offered under GPL-3.0-or-later **with** the
following additional permission, reproduced here in full so this grant is
self-contained. Upstream code keeps its own licence and notices and needs
no exception. Prepared 2026-09-16; the copyright holder adopted it on
2026-09-24, as recorded in the adoption line of the top-level Madeira
repository's LICENSE-EXCEPTION.md, before any public push.

The modifications and new files contributed by 125hz (the commits signed off
by 125hz) are offered on the same terms: GPL-3.0-or-later with the
additional permission below.

### Madeira Converter Exception, version 1 (of 2026-09-16; in effect from the adoption recorded in the top-level LICENSE-EXCEPTION.md)

Additional permission under GNU GPL version 3 section 7.

If you modify this Program, or any covered work, by linking or combining it
with the Apple Metal Shader Converter dynamic library
(libmetalirconverter.dylib, in any version) or with Apple's Metal,
Foundation, CoreGraphics, QuartzCore, UIKit, AppKit and related system
frameworks, or with modified versions of those libraries, the licensors of
this Program grant you additional permission to convey the resulting work.
Corresponding Source for a non-source form of such a combination shall
include the source code for the parts of the Program used in the
combination, but need not include the source code of those Apple libraries.
This permission does not extend to those libraries, which remain subject to
Apple's own licence terms. You may remove this additional permission from
copies you convey, as GPL-3.0 section 7 allows.
