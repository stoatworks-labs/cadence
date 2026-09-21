# cadence

Interlace, pulldown and every way a deinterlacer gets it wrong, as an FFGL
**effect** for Resolume Arena/Avenue. C++17 + GLSL 4.10, CMake MODULE → universal
`.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the field arithmetic, the presentation rule or
the inverse telecine.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (into `~/Documents/Resolume Arena/Extra Effects`)
- Render a frame offline: `./build/cdtest --out /tmp/frame.png --size 1280x720`
- Just the test card: `./build/cdtest --card /tmp/card.png`
- List parameters, with kind and default: `./build/cdtest --list`
- Set a control: `--set "Mode=4" --set "Adaptive Threshold=0.3"` (repeatable, by display name)
- Feed the audio input a click train: `--tone`
- Raw frames in, raw frames out (what the video pipeline uses):
  `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/cdtest --pipe --width 1920 --height 1080 [--script cues.txt] | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov`
- `--script` cue sheet for `--pipe`: `frame  Parameter Name  value` lines, held
  before the first key and after the last, linearly interpolated between — the
  same format tinseltest, octest and phtest read. **An option parameter
  interpolates**, so a Weave→Adaptive cue passes through Bob, Bob Linear and
  Blend; key one frame apart to cut. A name that is not a parameter is refused
  before a frame is read, rather than silently doing nothing.
- `--pipe` writes the video on stdout and nothing else: every message goes to
  stderr, because one stray line in stdout is a torn frame for the rest of the
  reel. Its clock is synthetic and driven by the frame index — a stall in ffmpeg
  upstream must not show up in the reel as the cadence speeding up.

## Verify
- Everything, from a fresh universal build: `tools/verify.sh` (~12 s)
- Weave combs by exactly v: `./build/cdtest --comb`
- Bob bounces a line: `./build/cdtest --bob`
- 2:3 emits A A B B B C C D D D: `./build/cdtest --pattern`
- Adaptive 0 is Bob Linear, 1 is Weave: `./build/cdtest --adaptive`
- Swapped field order stutters: `./build/cdtest --swap`
- The inverse telecine locks, mis-locks and re-locks: `./build/cdtest --lock`
- A resize clears the ring: `./build/cdtest --ring`
- No dead controls: `python3 tools/sweep.py`
- The browser demo runs the plugin's own GLSL: `python3 demo/tools/check_shaders.py`
- Render cost: `./build/cdtest --bench` (0.53 ms/frame at 1080p, 2.14 at 4K; macOS only — nothing has been timed on Windows)
- What a host sees: `../oxbow/build/oxbow probe build-universal/Cadence.bundle`
- Windows x64: cross-compiled in the Parallels guest (`cmake -A x64`, vcpkg
  `x64-windows-static-md`); the DLL that Arena loaded was 374,272 B and
  `dumpbin /EXPORTS` shows `plugMain`. `oxbow selftest` there: 120 frames, gl
  error 0x0, PASS, 921,600 of 921,600 pixels lit.

## Notes
- **A field is a slice of TIME, not just a slice of lines.** Field `k` carries
  the source frame current at the middle of its own field period. Every cadence
  is that one rule with different rates — `Pulldown.h`.
- **The CPU decides which frames, the GPU decides which pixels.** Frame
  selection is by time out of a ring; all field maths is integer row parity in
  the composite shader. Nothing samples *between* two rows, because two rows are
  two different fields — every field read is a `texelFetch`.
- **The deinterlacer is a field late, on purpose.** It consumes fields in pairs
  and only starts a pair once both fields are in, which is what a real one does.
  The pair at field K is `(K-1, K)` when K is odd and `(K-2, K-1)` when it is
  even.
- **Swap Field Order is invisible under Weave** and that is correct: a weave
  uses both fields of the pair and does not care which came first. It shows up
  the moment a mode displays one field at a time.
- **Combing is exactly measurable.** Two fields of one film frame carry the same
  input frame, so a woven pair whose two source serials differ IS combed. That
  is what `--lock` counts; no edge detection is involved.
- **Lock Time is monotonic in re-locks, not in combed frames.** A lock too slow
  to converge is frozen, and a frozen lock is right by accident about two frames
  in five — so it can comb *fewer* frames than a detector that is tracking. See
  AGENTS.md.
- All ranged host parameters are 0..1 and mapped in `Controls.cpp`.
  `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange`
  could widen it.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `ScopedFBOBinding` does not restore the viewport. Capture `GL_VIEWPORT` at the
  top of `ProcessOpenGL` and restore it before the composite — the 16×1 reduce
  buffer would otherwise leak into it.
- Every `ffglex::Scoped*` binding **clears to 0** on scope exit rather than
  restoring, so every `Ensure()` happens before anything binds a texture.
- `FFGLFBO::Release()` leaks the colour texture; `PassBuffer::Destroy()` deletes
  it first.
- `FFGLScopedFBOBinding.h` is not in `FFGLSDK.h`; include it by hand.
- `patch`, `sample`, `input`, `output`, `filter`, `common`, `active`, `half`,
  `layout` and `flat` are GLSL reserved words. A shader that will not compile is
  "the effect does nothing" plus a line in the diagnostics log.
- Parameter names must be unique — `--set` and the sweep find them by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `CD01`.

## Browser demo

`demo/` is a static page at **cadence-demo.stoatworks-labs.com**, deployed by
`cf-run npx wrangler deploy` from the repo root (`wrangler.toml`, a
static-assets-only Worker — no build step, no Pages).

- `demo/vendor/` is vendored from
  `infrastructure/stoatworks-backend/resolume-demo/kit/` and is **not** a place
  to edit. Re-sync with
  `~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh cadence`
  and confirm it says `synced cadence` rather than skipping.
- The five shader constants in `demo/plugin.js` are `source/Shaders.cpp`
  verbatim, and `demo/tools/check_shaders.py` (run by `verify.sh`) fails on a
  single character of drift.
- **Everything else in `demo/plugin.js` is a hand port** of `Controls.cpp`,
  `Pulldown.cpp` and the ring/field/presentation half of
  `Cadence::ProcessOpenGL`, and **nothing checks it but a reader.** Change the
  field arithmetic in C++ and it has to be changed there too.
- Absent from the page and said so on it: `Break On Onset` and the `Audio`
  buffer (a browser has no Resolume FFT parameter), and the host-clock-unit half
  of `Clock.cpp`. Inverse Telecine **is** there, diff and reduce passes and all,
  which is why the page declares `needFloat` — the detector reads RGBA32F back.
- The page draws its own moving bar over the clip before handing it to the
  plugin, for the same reason the harness's card moves: on a still picture every
  mode collapses to the input. That pass is the page's, not the plugin's, and is
  deliberately not in `check_shaders.py`'s table.
- Verify a deploy **by content, never by status code** — a stale page answers a
  cheerful 200.

## In a real host
- Registered, loaded and instantiated in **Resolume Arena 7.27.1** (build 15990)
  on Windows, 2026-09-21, with the shaders compiling — on **Mesa llvmpipe**, a
  software rasteriser, on a machine with no GPU.
- Arena lists it under its `idstring` `CD01`. Its add-effect REST endpoint
  returns 200 without adding anything, so instantiate from Arena's own effects
  browser. See AGENTS.md.
- **The host clock is in milliseconds under Arena and seconds under oxbow.**
- An ssh session on Windows has no desktop; Arena has to be started through the
  session-1 scheduled-task wrapper. See AGENTS.md.

## Not done yet
- **Never run on a GPU in Resolume**, and never instantiated in Arena on macOS
  or installed into Extra Effects there.
- No frame timing on Windows; no long session, composition save/reload or preset
  recall in the host; no real audio in the host.
- No OpenFX port, no user guide, no video.
- `ATTRIBUTIONS.md` is still a provisional hand copy — `sync-attributions.py`
  does not know this repo. `source/StoatworksAbout.h` is generated by
  `sync-about.py` now; do not hand-edit it.
- `ci.yml` is macOS-only. Windows is built only by the release workflow, on a
  tag, and that build has never been in front of Arena.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It records which shader failed to compile, the GL
vendor/renderer/version, a ring that could not be allocated, and the host's
clock unit at frame 60.

    ~/Library/Logs/cadence/cadence.YYYY-MM-DD.log     (macOS)
    %LOCALAPPDATA%\cadence\logs\cadence.YYYY-MM-DD.log     (Windows)

On the 2026-09-21 Windows run that log is the proof of instantiation: `plugin
loaded build=<stamp>`, then the GL vendor/renderer line and `initialised`.
