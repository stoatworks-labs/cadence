# Attributions

Cadence is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Cadence is in that
> script's `names.json` but not in its component lists, so this copy is still
> hand-written; v0.1.0 shipped that way. Finishing the registration and re-running
> the sync is the fix — and note that the script's `--only` flag truncates the
> file rather than filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for
the OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Linked by the offline harness only, from the macOS system, to deflate the PNGs
it writes. Nothing in the shipped plugin uses it.

## Work from elsewhere in the fleet

### macroblock

<https://github.com/stoatworks-labs/macroblock>
Licence: MIT
Copyright: Stoatworks Labs

`source/Audio.{h,cpp}` and `source/Clock.{h,cpp}` are carried over from
macroblock — the FFT-buffer analyser and the host-clock unit measurement.
Copied rather than shared, because the fleet has no common library and a header
shared between two repos by hand is a header that diverges silently.

### afterglow

<https://github.com/stoatworks-labs/afterglow>
Licence: MIT
Copyright: Stoatworks Labs

`source/PassBuffer.{h,cpp}` — the FFGL FBO wrapper with the SDK's colour-texture
leak fixed — and the shape of the ring of recent frames.

### tinsel

<https://github.com/stoatworks-labs/tinsel>
Licence: MIT
Copyright: Stoatworks Labs

The CMake MODULE-plus-submodule layout, the `Diag` logger, the offline harness
shape, `tools/sweep.py` and the release workflow.

## The method

The interlacing, pulldown and deinterlacing behaviour here is implemented from
the published description of how the processes work — 2:3 pulldown, weave, bob,
field blending, motion-adaptive deinterlacing and inverse telecine are decades
old and described in the broadcast engineering literature. No code was taken
from any deinterlacer implementation.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
