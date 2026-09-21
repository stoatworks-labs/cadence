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
- Render cost: `./build/cdtest --bench` (0.53 ms/frame at 1080p, 2.14 at 4K)
- What a host sees: `../oxbow/build/oxbow probe build-universal/Cadence.bundle`

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

## Not done yet
- **Never loaded into Resolume**, and never installed into Extra Effects.
- No remote, no release tag, no website registration, no OpenFX port, no
  browser demo, no user guide.
- `source/StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies.
- Windows and the CI workflows have never run.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It records which shader failed to compile, the GL
vendor/renderer/version, a ring that could not be allocated, and the host's
clock unit at frame 60.

    ~/Library/Logs/cadence/cadence.YYYY-MM-DD.log
