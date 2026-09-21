# AGENTS.md — Cadence

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the
short command reference; this is the *why*. Read "What is actually verified"
before you tell anybody this works.

---

## What the plugin is

An FFGL 2.1 effect (`CD01`, `SW Cadence`) that treats a progressive clip as a
stream of **fields with real time between them**, puts a film cadence in front
of it, and shows the result on a progressive screen through a deinterlacer you
choose — including the ways a deinterlacer gets it wrong.

---

## The one idea

**A field is a slice of time, not just a slice of lines.**

A television picture is two fields 1/50 or 1/60 s apart, each holding alternate
lines. Once that is the model, every artefact anybody recognises is a
*consequence* rather than something drawn:

- **combing** — a moving object's two fields are `v · T_field` apart, so a weave
  puts it in two places at once;
- **bob bounce** — line doubling has to invent the missing rows, and the
  invention lands one row lower on one field than the other, so static fine
  detail jumps;
- **field-blend ghosting** — averaging two fields of a moving object is two
  objects at half strength;
- **motion-adaptive mistakes** — the per-pixel weave/bob decision at a threshold
  that is too high (combing leaks through) or too low (detail goes soft);
- **3:2 judder** — 24 frames laid into 60 fields cannot be even, so motion moves
  in the 2-3-2-3 rhythm;
- **a cadence break** — an edit landing mid-cycle, which a naive inverse
  telecine mis-locks on and keeps weaving the wrong pair until it re-locks;
- **swapped field order** — believing the wrong field came first, which is the
  two-forward-one-back stutter.

None of those is a mode in the shader. They are what falls out of choosing
fields by time and rows by parity.

### The rule, in one line

    n(k) = floor( (k + phase + 0.5) · Rs / Rf )

Field `k` carries the source frame current at the *middle* of its own field
period. Every named pulldown is that with different rates — 24→60 gives
`0 0 1 1 1 2 2 3 3 3`, which is 2:3; 25→50 and 30→60 give pairs. There is no
table of cadences anywhere in this repo, and there should not be one.

### What does not fall out, and is the honest limit

**The input is progressive, so a "field" here is half of a whole frame that the
plugin itself sampled.** Real interlaced footage has fields that were *shot*
separately; this synthesises them by taking alternate lines of frames chosen by
time. For Split at 60 fps that is exactly right — one frame, one field, a real
1/60 s apart. For the telecine it is right up to the input's own frame rate: a
24p cadence built out of a 60 fps clip resamples, so what a field carries is the
nearest host frame at or before the moment it was "shot", not a frame shot then.
On real 24p material played at 24p it is exact; on 60 fps material it is a
faithful model of a telecine applied to something that was never film.

**Nothing here reads a real interlaced stream.** If a clip is already
interlaced, this treats it as progressive and interlaces it again.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Pulldown.{h,cpp}` | The cadence as arithmetic. Which frame a field carries, which lines it owns, and the naive inverse telecine. No GL, no pixels. |
| `source/Controls.{h,cpp}` | What a 0..1 slider position means, in physical units. |
| `source/Shaders.{h,cpp}` | Four fragment shaders. The composite is the deinterlacer. |
| `source/Cadence.{h,cpp}` | The plugin: parameters, the ring, the field stream, the presentation rule. |
| `source/PassBuffer.{h,cpp}` | An FBO that reallocates only when it has to and frees its colour texture. From afterglow. |
| `source/Audio.{h,cpp}`, `source/Clock.{h,cpp}` | The FFT-buffer analyser and the host-clock unit measurement. From macroblock. |
| `tools/cdtest/` | The offline harness: renders, checks, benchmarks. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, from a fresh universal build. |

### Three stages, on two processors

The **CPU** decides *which* input frames are fields (by time, out of a ring of
recent frames) and *which* fields the deinterlacer is looking at. The **GPU**
does everything that touches a pixel, with integer row parity. That split is the
reason the checks can be exact: what the CPU decided is a small set of integers
the harness can read straight out of the plugin, and what the GPU did is a
picture it can measure.

---

## Traps

Roughly in the order they will bite.

### ☠️ Never sample *between* two rows

Two adjacent rows are two different fields, a field period apart. A bilinear
sample across them is a sample across time, and it silently turns every mode
into a soft blend — the picture still looks plausible, which is the dangerous
part. Every field read in the composite is a `texelFetch` at an integer row, and
every buffer in this plugin is allocated `Sampling::Nearest` for the same
reason.

### ☠️ The deinterlacer must be a field late

It consumes fields in **pairs** — `(2j, 2j+1)` — and cannot start on a pair
until both fields are in. So at field K the pair in hand is `(K-1, K)` when K is
odd and `(K-2, K-1)` when it is even. Present the pair `(K, K+1)` instead and
you are showing a field that has not happened yet; present only `(K-1, K)` and
the field rate halves. One field of latency is what a real deinterlacer has, and
it is what makes the swap sequence come out `1 0 3 2 5 4` rather than something
that merely looks jittery.

### ☠️ Swap Field Order is invisible under Weave, and that is correct

A weave uses **both** fields of the pair and does not care which one the
deinterlacer believes came first — the rows are assigned by parity either way.
Swap only changes which field is *shown*, so it is visible exactly in the modes
that show one field at a time. `tools/sweep.py` reported it dead until it was
given `Mode=1` as its context, and the right fix was the context, not the code.

### ☠️ Lock Time is monotonic in re-locks, NOT in combed frames

This one is worth reading twice, because the obvious assertion is false and it
passed review once by being plausible.

A lock time of four seconds against a cadence break every 2.7 s never converges,
so the detector is **frozen** on whichever cycle position it started from. A
frozen lock is right by accident about two frames in five — so on a real run it
combed **10** frames where the detector that was actually tracking combed **13**.
Asserting "a slower lock combs more" is asserting a coin toss: it passes or
fails on which phases the breaks happened to pick.

What Lock Time actually sets is how quickly the cadence scores settle, and that
shows up as the number of **re-locks**: 4 at 0.05 s against 0 at 4 s, on the
same material. `cdtest --lock` asserts that, and the README reports the combed
counts without ordering them.

The same fact makes Lock Time the weakest entry in `tools/sweep.py`: it reaches
the *picture* only inside the window after a break where a fast lock has
re-locked and a slow one has not, so its context is tuned to end inside that
window and it is the one entry that would go quiet if the run length changed.
That is documented in the sweep's own docstring rather than left to be
rediscovered.

### Combing is exactly measurable — do not go looking for edges

Two fields of one film frame carry the **same input frame**. So a woven pair
whose two source serials differ is combed, and one whose serials agree cannot
be. `LastPresentationForTest` hands both serials out and `--lock` counts them.
An edge-detection heuristic here would be less accurate and much harder to
believe.

### `ScopedFBOBinding` restores the framebuffer and not the viewport

SDK `b1afaf9`. Every pass's `ResizeViewPort()` leaks into the next one, and the
composite — which draws to the host's own framebuffer and so has no buffer to
size itself from — inherits whatever the last pass left. Here that would be the
**16×1** reduce buffer, so the effect would paint sixteen pixels of the bottom
row and leave the rest of the frame untouched. `ProcessOpenGL` captures
`GL_VIEWPORT` at the top and restores it before the composite.

### Every `ffglex::Scoped*` binding CLEARS to 0 on scope exit

It does not restore. `FFGLFBO::Initialise` sizes its new colour texture under
one of those, so **allocating a buffer unbinds the input texture from the active
unit**. Every `Ensure()` happens before anything binds a texture, and it has to
stay that way. The symptom is the dangerous part: correct on every frame except
the one that allocates — so it shows up once at load and once more each time a
resize or a Field Source change rebuilds the ring.

### `FFGLFBO::Release()` leaks the colour texture

It deletes the framebuffer and the depth renderbuffer, then tests
`depthBufferID` a second time where it plainly meant `colorTextureID`.
`PassBuffer::Destroy()` deletes it first. It matters here rather than being
pedantry: the ring rebuilds on every resize and on every change of Field Source,
which is eight full pictures at a time.

### A TEXT parameter without `SetTextParameter` kills the whole plugin

`instantiateGL` pushes every declared default back through the setters and
deletes the instance the moment one returns `FF_FAIL` — which is exactly what
`CFFGLPlugin::SetTextParameter` does. The About block is display-only text, so
there is nothing to store, but it has to say so *successfully*. Invisible in
every in-repo harness, because they call the plugin class directly.

### A ranged STANDARD parameter cannot have a ranged default

`SetParamInfo` clamps a standard default into 0..1 *before* returning, and
`SetParamRange` can only be called afterwards. So every ranged parameter here is
a plain 0..1 float and the conversions live in `Controls.cpp`.

### `FFGLShader::Set` has no integer-vector overload

The overloads are `float`, `vec2`, `vec3`, `vec4` and `int` — nothing else.
`Set( name, someInt, someInt )` resolves to `(float,float)` and issues a
`glUniform2f` against an `ivec2`, which is a `GL_INVALID_OPERATION` that leaves
the uniform at zero with nothing anywhere the plugin can see. Every integer
uniform here is set one at a time.

### The host's clock is in milliseconds and the header does not say so

Resolume sends milliseconds; the harness sends seconds. `Clock` measures the
unit against a wall clock over the first few frames rather than assuming it, and
the harness **declares** its unit through `SetClockScaleForTest` because it
renders as fast as the GPU allows and there is nothing for the measurement to
measure. Guess wrong and the field clock runs a thousand times fast: the
telecine emits its whole cycle in one frame.

Confirmed on 2026-09-21, the first time this code met a real host: under oxbow
cadence settled on `scale=1.000000` (**seconds**) by frame 60, and under Arena
the host time is in **milliseconds** (raw ≈ 574,073 during that run). The two
units are both real and the detector is what tells them apart — do not hard-code
either one.

### A negative left operand has a negative remainder in both C++ and GLSL

The field index and the cycle position both go negative at start-up and under a
phase shift. `floorMod` in `Pulldown.cpp` is what stops a negative index
reaching `slots[]`, which is not a wrong picture but a crash.

### The test card has to MOVE

Everything here is the difference between one field and the next. On a still
card every mode collapses to the input, the trail of checks all pass vacuously,
and `sweep.py` reports almost every control dead.

### `layout` is a GLSL keyword

So are `flat`, `active`, `filter`, `input`, `output`, `sample`, `common`,
`patch` and `half`. A shader that fails to compile surfaces only at runtime, as
"the effect does nothing", with the real message in the diagnostics log.

### `vcpkg.json` is invisible from the CMakeLists

GLEW arrives through the vcpkg manifest, and if the CMakeLists does not ask for
it, every local build and every macOS CI job passes while the Windows job fails
at *configure*. `find_package(GLEW REQUIRED)` is there now, guarded to
Windows/Linux, and the release workflow's `windows-latest` job has configured,
built and shipped an x64 DLL on the strength of it. `ci.yml` is macOS-only, so
that path is only exercised on a tag. The DLL that ran in Arena was a different
build, configured in the Parallels guest with the same triplet
`x64-windows-static-md`; the CI-built one has never been put in front of Arena.

### ☠️ An ssh session on Windows has no desktop

An ssh login on Windows lands on the **service window station**, which has no
desktop. Arena started from there sits at about 31 MB doing nothing, cannot be
screenshotted, and never serves its REST API. It has to be launched in the
console session (session 1) through the scheduled-task wrapper — on win-lab,
`C:\arena-lab\s1.ps1`. Every attempt to shortcut this wastes a round trip and
looks like a plugin fault.

### ☠️ Arena's REST API lists effects by `idstring`, and its add-effect endpoint lies

`/api/v1/effects` and `/api/v1/sources` name each plugin by its **FFGL id** in
the `idstring` field — this one is `CD01`, not `SW Cadence` — so search on the
id. The add-effect endpoint **returns 200 without adding anything**: nothing
appears in the layer or clip and no plugin is instantiated. Instantiation has to
be driven from **Arena's own effects browser** (double-click applies to the
current selection), and the proof that it happened is the plugin's own diag log,
not the clip's effect list. On the 2026-09-21 run the effect was applied to the
composition and `/api/v1/…/clips/1` still showed only `Transform` afterwards.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1

Everything below is `tools/verify.sh`, which builds the universal Release bundle
from scratch and then asks it every question. All of it drives the **real plugin
class** through the real FFGL sequence in a headless CGL context.

- **Combing is exactly `v`.** A bar moving v px/frame: 1,980 rows checked per
  speed at v = 1, 3 and 7, largest error **0 px**, and the even/odd offset is v
  on every frame.
- **Bob bounces one line.** A two-line rule's top edge alternates between rows
  80 and 81; a one-line rule on an even row exists on alternate fields only.
- **The telecine emits the pattern.** Every input frame is tagged with its own
  grey level and read back out of the picture: 24→60 gives runs
  `3 2 3 2 3 2 …` (A A B B B C C D D D from a different phase), 30→60 and 25→50
  give pairs, and the input frames advance at 2.47, 2.00 and 2.40 host frames
  per source frame against 2.50, 2.00 and 2.40 wanted.
- **Adaptive collapses exactly.** Threshold 0 is Bob Linear and threshold 1 is
  Weave — **0 of 2,995,200 bytes differ** in each case, over 13 frames of the
  moving card — and 0.15 is neither (55,411 bytes from Bob Linear, 139,102 from
  Weave).
- **A swapped field order stutters.** Shown-field sequence `0 1 2 3 4 5 …`
  correct, `1 0 3 2 5 4 …` swapped, asserted as the field index.
- **The inverse telecine locks and mis-locks.** On a clean 2:3 it settles after
  one lock change and combs **0** frames in the settled last third. After
  cadence breaks it combs until it re-locks: 4 re-locks at Lock Time 0.05 s
  against **0** at 4 s.
- **A resize does not leak the old picture.** Resize up, down, and both Field
  Source changes that resize the ring: ring rebuilt, `filled` back to 1, **0**
  stale pixels, no crash.
- **No dead controls.** All **14** swept parameters measurably change the
  picture, each with the context that makes it mean anything.
- **The build is universal and registers.** `lipo` reports `x86_64 arm64`, `nm`
  finds `_plugMain`, the plist names the binary that is there and the bundle
  ad-hoc signs.
- **A host sees the right plugin.** `oxbow probe` reports name `SW Cadence`, id
  `CD01`, type `effect`.
- **The render cost**, `cdtest --bench`, 60 frames after a 20-frame warm-up with
  `glFinish` on both sides:

  | | ms/frame | % of a 60 fps frame |
  | --- | --- | --- |
  | 1280×720 | 0.227 | 1.4% |
  | 1920×1080 | 0.532 | 3.2% |
  | 2560×1440 | 1.008 | 6.0% |
  | 3840×2160 | 2.143 | 12.9% |

  Inverse Telecine costs more, because it adds a diff, a reduce and a
  **synchronous readback** per field: 0.709 / 0.857 / 1.455 / 3.181 ms at the
  same four rasters. The readback is the reason, and it is a deliberate trade —
  the alternative is a fence and a frame of extra latency in the detector.

### Verified in a real host: Resolume Arena 7.27.1 on Windows, 2026-09-21

On **win-lab** — an x64 Windows 11 Pro VM with **no GPU**, where OpenGL comes
from **Mesa llvmpipe** dropped in beside Arena (`GL vendor=Mesa
renderer=llvmpipe (LLVM 22.1.8, 256 bits) version=4.5 (Core Profile) Mesa
26.2.0`) — against **Resolume Arena 7.27.1** (build 15990) running in the console
session:

- **The x64 DLL builds and exports the entry point.** Cross-compiled in the
  Parallels guest on this Mac (ARM64 Windows 11, MSVC 2022 Build Tools,
  `cmake -A x64`, vcpkg triplet `x64-windows-static-md`) — the same route the
  fleet's `winbuild` scripts use; there is no x64 Windows machine in the build
  loop. **374,272 B**, and `dumpbin /EXPORTS` shows `plugMain`.
- **Arena registers it.** Arena's own REST API lists `SW Cadence` among 112 video
  effects, under `idstring` `CD01`, with the description the plugin declares.
- **Arena loads the DLL.** The diag log under `%LOCALAPPDATA%\cadence\logs\`
  carries `plugin loaded build=<stamp>` with the stamp of the DLL built minutes
  earlier.
- **Arena instantiates it and the shaders compile.** Applied from Arena's own
  effects browser, it logged the `GL vendor=Mesa … 4.5 (Core Profile)` line
  followed by `initialised`, and Arena drew its inspector for it, groups and all.
  So `plugMain`, `instantiateGL` and the `SetTextParameter` trap are all
  exercised for the first time.
- **It instantiates and renders headlessly on x64 Windows too.** `oxbow selftest`
  (oxbow built x64 in the same guest): **120 frames, gl error 0x0, PASS**, with
  **921,600 of 921,600** pixels lit — full frame, as a full-frame effect should
  be.
- **The host clock unit detection works in a real host.** Milliseconds under
  Arena against seconds under oxbow — see the clock trap above.
- **No warnings or errors.** The diag log is clean of WARN/ERROR/FAIL.

What that run did **not** show: no GPU was involved anywhere, so it says nothing
about performance on Windows and **no frame timing was taken there** — the
ms/frame table above is macOS only. No real audio reached the plugin in Arena. No
long session, no composition save/reload and no preset recall in the host. The
effect was applied to the **composition**, not to a clip.

### Assumed, or not done

- ☠️ **It has never run on a GPU in Resolume**, and has **never been instantiated
  in Arena on macOS** — nor installed into Extra Effects there. Everything the
  Windows run proved, it proved on a software rasteriser: registration, load,
  instantiation, shader compilation, the parameter groups in the inspector and
  the host's clock unit. What a real GPU driver does with these shaders, and what
  the macOS bundle does in front of Arena, are both still untested.
- ☠️ **The audio path has only ever seen a synthetic click train.** Break On
  Onset works against `--tone`, which pushes a spectrum through the same call
  Resolume uses, but no real music has driven it — no real audio reached the
  plugin in Arena either — and the bin count and magnitudes are taken from
  macroblock rather than measured here. Resolume's 64-bin FFT mapping is still
  assumed, not measured.
- **CI and the release workflow have both run and passed on GitHub**, but `ci.yml`
  is macOS-only: Windows is built only by the release workflow, on a tag. The DLL
  that ran in Arena was built by hand in the Parallels guest, not by that job, so
  the released Windows binary has never been in front of a host.
- **No OpenFX port.** Not required for 0.1.0.
- **The browser demo's CPU half is a hand port that nothing checks but a
  reader.** `demo/tools/check_shaders.py` proves the five shaders in
  `demo/plugin.js` are `source/Shaders.cpp` character for character, and
  `verify.sh` runs it. It proves nothing at all about the JavaScript port of
  `Pulldown.cpp`, `Controls.cpp` and the ring/field/presentation half of
  `ProcessOpenGL`, which is where this plugin's ideas actually live. That port
  was checked **once, by hand, on 2026-09-21** against a sweep of the C++ — 700
  cases of `SourceFrameOfField`, `SourceFrameTime`, `FieldParity` and
  `CycleFields` across five rate pairs and five phases, and 120 fields of
  `InverseTelecine` — and every integer answer, every lock, every cycle position
  and every weave decision agreed exactly, with the conversions differing only
  in float32-against-float64 rounding past the eighth significant figure. That
  was a one-off at the time of writing and there is **no standing check**; it
  will go stale the first time the arithmetic moves without the page moving with
  it.
- **No user guide**, so `guide` is empty in `StoatworksAbout.h` and the About
  block has three buttons rather than four. That header is **generated** by
  `sync-about.py` now — the project is registered in the website's
  `projects.json`, in `sync-about.py`'s TARGETS and in `attributions/names.json`
  — so do not hand-edit it. `ATTRIBUTIONS.md` is still a provisional hand copy in
  the shape the fleet's sync scripts generate, because
  `sync-attributions.py`'s master lists do not know this repo yet.
- **No factory presets.** The fleet's preset mechanism — and the
  host-restatement bug it exists to survive — is deliberately not here: with
  fifteen controls and four groups there is nothing a preset would say that the
  defaults and one dropdown do not. If presets are added later, copy the
  `hostValues[]` pattern from afterglow whole, including `seedHostValues()`
  running *before* `applyPreset` can.
- **The 4:2:2 chroma question is not addressed.** Real interlaced video in 4:2:0
  has its chroma sited per field, which is a whole second artefact. Everything
  here is RGB.
- **Nothing has been through a real show.**

---

## Decisions made along the way

- **`Break Interval` restarts the cycle at a random phase, never the same one.**
  An edit that lands on the phase it left is not an edit anybody would notice,
  so the hash picks from the other `cycle - 1` positions.
- **The audio analysis has no controls.** macroblock exposes attack, release,
  sensitivity and hold; here the only question the spectrum answers is "was
  there an edit this frame", and four sliders to tune that would be four sliders
  an operator cannot hear the difference between.
- **`Field Rate` and `Source Rate` are options, not sliders**, because 24, 25,
  30 and 60 are the only values that mean anything and a slider that lands on 27
  lands on nothing.
- **The option lists are not sorted.** Every one is a progression or a pair, and
  there is nothing to look up alphabetically in a list of three. The fleet's
  alphabetical-display convention exists for twenty-item lists.
- **The ring is 4 frames under Split and 8 under the telecine**, sized to the
  longest look-back the cadence needs rather than always the maximum: eight full
  4K frames is a quarter of a gigabyte.

### The browser demo, 2026-09-21

- **Inverse Telecine is in the demo, and the diff and reduce passes with it.**
  The obvious call was to leave it out as "a CPU detector" — it is one, and the
  kit's other demos have dropped whole CPU stages. It went in because
  `InverseTelecine` is thirty lines of one-pole average and an argmin with a
  margin, and because leaving it out would have taken the mis-lock with it — the
  artefact the *whole Cadence Break control exists to produce*, and the most
  interesting thing the plugin does. The cost is that the page declares
  `needFloat`, because the detector reads a 16×1 RGBA32F row back with
  `readPixels`. That was checked in the browser before it was relied on:
  `EXT_color_buffer_float` present, `RGBA`/`FLOAT` the implementation read pair,
  and a value of 123.5 clearing and reading back exactly — i.e. genuinely
  unclamped float, not a normalised buffer quietly pretending.
- **The demo draws its own moving bar over the clip, and says so.** None of the
  kit's generated clips move fast enough to comb, and this plugin is *about*
  motion: on a still picture every mode collapses to the input, which is the
  same reason the harness's test card moves (see "The test card has to MOVE").
  The overlay is a sweeping vertical bar, a vertically moving block and a
  one-line and a two-line static rule on even rows — the same three jobs the
  harness's card does. It is a pass of the page's own, deliberately **not** in
  `check_shaders.py`'s table, disclosed in the page header comment and in the
  on-page "what this page does not reproduce" list, and switchable off from the
  transport. The alternative considered and rejected was to comb the clip
  harder by inventing a pan, which would have been the same intervention with
  none of the honesty.
- **The audio side is absent rather than present and dead.** `Break On Onset`
  and the 64-bin `Audio` buffer are not on the page at all. A browser has no
  Resolume FFT parameter, and asking a visitor for a microphone to demonstrate a
  video effect is not a trade worth making. The free-running half of Cadence
  Break — an edit every `Break Interval` — *is* there and is the same code path,
  so the mis-lock is still visible; what is not visible is a kick drum choosing
  the moment.
- **Only the clamping half of `Clock.cpp` is ported.** The unit calibration
  answers a question a browser does not ask. The visible consequence is that
  **Restart restarts the clip and not the field clock**, which is the plugin's
  own behaviour — its clock is monotonic, and a host scrubbing backwards moves
  it forward by a 240th of a second.
- **The page's presets are the page's**, and the disclosure says so. This plugin
  ships no factory preset table (see above), so unlike macroblock's there is
  nothing in the repository for them to mirror. Every value in one is a real
  control at a real position; the choice of which is the page's.

### `--pipe`, 2026-09-21

- **`cdtest --pipe` is the same `Session` the checks use**, not a second render
  path, so what a reel shows is what `--comb` and `--adaptive` measured.
- **Its clock is driven by the frame index, never by the wall clock or by the
  rate the pipe delivers.** On this plugin that is not housekeeping: a field is
  a slice of time, so a stall in ffmpeg upstream would otherwise appear in the
  finished file as the cadence speeding up, and 2:3 would stop being 2:3
  halfway through a shot.
- **A partial frame at the end is dropped, not padded**, and says so on stderr
  with the width and height it was expecting. Half a frame of black at the end
  of a reel is a flash, and a flash in an export is a bug report — and the
  commonest mistake with this flag is a `--width` that does not match what
  ffmpeg is sending, which otherwise produces a sheared picture rather than a
  message.
- **Everything but the video goes to stderr.** One stray line in stdout is a
  torn frame for the rest of the reel, which is also why `--pipe` is dispatched
  before any other path in `main` can print.
- A script naming a parameter that does not exist is refused **before a frame is
  read**. A misspelled name that silently did nothing would produce a take that
  looks deliberate and is wrong: the reel would hold whatever the default was,
  with a caption over it describing a control that never moved.
