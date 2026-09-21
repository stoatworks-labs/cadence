#!/usr/bin/env python3
"""
No control is silently dead.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
slider can be wired to nothing while the plugin compiles, links, loads and
renders perfectly. Nothing in a build catches it and nothing in the picture
looks wrong -- the control just does not do anything, which is
indistinguishable from not having noticed what it is for.

This renders each parameter at several positions and checks the picture
actually changed.

    python3 tools/sweep.py [--binary build/cdtest]

--------------------------------------------------------------- the traps

**Most of these controls are conditional, and on this plugin the conditions
are unusually strong.** Source Rate means nothing under Split, because Split
does not have a source rate -- one input frame is one field. Swap Field Order
means nothing under Weave, because a weave uses BOTH fields of the pair and
does not care which of them the deinterlacer thinks came first; it is visible
the moment a mode shows one field at a time. Break Interval means nothing
unless the source is Cadence Break. Those are not defects, they are the
effect, and each one carries the context that makes it mean anything.

A parameter added without a context that turns out to be conditional will be
reported dead here, loudly, which is this table doing its job.

**Time has to pass.** The telecine emits fields on the host's clock, so a run
shorter than a few fields is a picture of the ring still filling. Every render
here is 40 frames of a MOVING card; a still card would have almost every
control read dead, because every mode collapses to the input when nothing
moves.

**Lock Time is a timing control, and a picture is a poor instrument for
one.** What it sets is how quickly the inverse telecine's cadence scores
settle, so it reaches the picture only in the window after a cadence break
where a fast lock has already re-locked and a slow one has not. Outside that
window every lock time weaves the same pair and the frames are identical --
so this entry is tuned to end inside it, and it is the one entry here that
would go quiet if the run length changed. `cdtest --lock` is what actually
measures the control: it counts re-locks, which are monotonic in Lock Time,
where combed frames are not (a lock too slow to converge is frozen, and a
frozen lock is right about two frames in five by accident).

**An entry beginning with `@` is a harness setting, not a plugin parameter.**
`@frames=` is the run length and `@tone` pushes a synthetic click train into
the Audio buffer. The prefix is not decoration: this plugin has real
parameters whose names could collide with a harness flag, and a silent
collision would make a control look alive because the harness did something
else.
"""

import argparse
import hashlib
import pathlib
import subprocess
import sys
import tempfile

# What else has to be true for a parameter to have any effect at all.
CONTEXT = {
    # Split has no source rate and no field rate: one input frame is one
    # field, and the field clock is the host's frame clock.
    "Source Rate": ["Field Source=1"],
    "Field Rate": ["Field Source=1"],
    # A weave uses both fields of the pair and does not care which the
    # deinterlacer believes came first. Bob shows one field at a time, which
    # is exactly when the belief matters.
    "Swap Field Order": ["Mode=1"],
    # Breaks only happen under Cadence Break, and only once enough time has
    # passed for one to be due.
    "Break Interval": ["Field Source=2", "@frames=120"],
    # An onset needs a spectrum to be an onset in. Break Interval is pushed
    # long so the interval does not fire on its own and mask it.
    "Break On Onset": ["Field Source=2", "Break Interval=1", "@tone", "@frames=120"],
    "Adaptive Threshold": ["Mode=4"],
    "Show Decision": ["Mode=4"],
    # Lock Time is a TIMING control, and this is the weakest entry in the
    # table -- see the note in the docstring. It needs the inverse telecine
    # running, something to knock it off its lock, and a run long enough to
    # end inside the window where a fast lock and a slow one disagree about
    # which pair to weave.
    "Lock Time": ["Mode=5", "Field Source=2", "Break Interval=0.35", "@frames=240"],
    # The line filter runs on the deinterlaced frame, not on the Fields
    # display, which is one field on its own lines and black on the rest.
    "Line Filter": ["Display=0", "Mode=1"],
}

# Positions to try. Three rather than two, because a control that is a no-op
# at both ends but not in the middle would otherwise pass.
POSITIONS = ["0.0", "0.5", "1.0"]

# The FFT buffer is the host's to fill and the About block is a text line and
# three browser buttons -- sweeping those opens a tab per press.
SKIP_KINDS = {"buffer", "about", "text", "event"}


def render(binary, out, settings, frames, tone):
    command = [binary, "--out", str(out), "--size", "320x180", "--frames", str(frames)]
    if tone:
        command.append("--tone")
    for setting in settings:
        command += ["--set", setting]

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"render failed: {result.stderr.strip()}")
    return hashlib.sha256(out.read_bytes()).hexdigest()


def parameters(binary):
    """name and kind, from the harness's own declaration."""
    listing = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        raise RuntimeError(f"could not list parameters: {listing.stderr.strip()}")

    found = []
    for line in listing.stdout.splitlines()[1:]:
        fields = line.split()
        if len(fields) < 3:
            continue
        # "  0  Field Source          option     0.0000  [ 0 .. 2 ]"
        rest = line[len(fields[0]) + 2:]
        parts = rest.split("  ")
        parts = [p for p in parts if p.strip()]
        if len(parts) < 2:
            continue
        found.append((parts[0].strip(), parts[1].strip()))
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/cdtest")
    arguments = parser.parse_args()

    binary = pathlib.Path(arguments.binary)
    if not binary.exists():
        print(f"no {binary} -- build with -DCADENCE_BUILD_TOOLS=ON first")
        return 2

    try:
        declared = parameters(str(binary))
    except RuntimeError as error:
        print(error)
        return 2

    if not declared:
        print("no parameters found -- the --list parsing has gone stale")
        return 2

    dead = []
    swept = 0
    skipped = []

    with tempfile.TemporaryDirectory() as directory:
        out = pathlib.Path(directory) / "sweep.png"

        for name, kind in declared:
            if kind in SKIP_KINDS:
                skipped.append((name, kind))
                continue

            context = list(CONTEXT.get(name, []))

            # Long enough for the ring to have filled with DIFFERENT pictures
            # and for the telecine to have emitted a full cadence cycle.
            frames = 40
            tone = False
            for entry in list(context):
                if entry.startswith("@frames="):
                    frames = int(entry.split("=", 1)[1])
                    context.remove(entry)
                elif entry == "@tone":
                    tone = True
                    context.remove(entry)

            digests = set()
            failed = False
            for position in POSITIONS:
                try:
                    digests.add(render(binary, out, context + [f"{name}={position}"], frames, tone))
                except RuntimeError as error:
                    print(f"  {name}: {error}")
                    dead.append(name)
                    failed = True
                    break

            if failed:
                continue

            swept += 1
            alive = len(digests) > 1
            if not alive:
                dead.append(name)
            note = f"   ({', '.join(context)})" if context else ""
            print(f"  {'ok' if alive else 'DEAD':4}  {name}{note}")

    print()
    for name, kind in skipped:
        print(f"  skip  {name}: {kind}")

    print()
    if dead:
        print(f"{len(dead)} parameter(s) changed nothing: {', '.join(dead)}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    print(f"all {swept} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
