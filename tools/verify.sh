#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# It builds the UNIVERSAL Release bundle from scratch and then asks it every
# question this repo knows how to ask. Each check answers one none of the
# others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. A shader that will not
#                 compile presents to an operator as "the effect does
#                 nothing", with the real message buried in a log file.
#   --comb        weave: a bar moving v pixels a frame is offset between the
#                 even and odd lines by exactly v. This is the plugin's
#                 central claim -- a field is a slice of TIME -- and it is
#                 asserted per row, not per picture.
#   --bob         line doubling puts a two-line detail one row lower on one
#                 field than the other, and a one-line detail exists on only
#                 one field at all.
#   --pattern     the telecine really emits A A B B B C C D D D, read back
#                 out of the picture by tagging every input frame with its
#                 own grey level. Also 2:2 at 30->60 and 25->50.
#   --adaptive    Adaptive Threshold at 0 IS Bob Linear and at 1 IS Weave,
#                 byte for byte, and the middle is neither. A control whose
#                 ends are two other modes is worth proving exactly.
#   --swap        a swapped field order produces the two-forward-one-back
#                 position sequence, asserted as the field index shown.
#   --lock        the inverse telecine settles on a clean cadence and stops
#                 combing; a cadence break combs until it re-locks; and Lock
#                 Time decides whether it re-locks at all. Combing is exact
#                 here rather than judged: two fields of one film frame carry
#                 the same input frame, so a woven pair with two different
#                 source serials IS combed.
#   --ring        a resize mid-run, and a Field Source change that resizes
#                 the ring, rebuild it empty without a crash and without a
#                 pixel of the old picture surviving.
#   demo          that demo/plugin.js still carries the plugin's OWN five
#                 shaders, character for character. The browser demo's whole
#                 claim is that what it runs is this repository's GLSL, and two
#                 copies of a shader is exactly the arrangement that drifts --
#                 invisibly from both sides, because the plugin keeps working
#                 and the page keeps working.
#   sweep.py      that no control is silently dead. A GLSL uniform whose name
#                 does not match the C++ is ignored without a word, so this is
#                 the only thing standing between a typo and a shipped slider
#                 that does nothing.
#   registration  that the bundle contains a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the macOS build really universal, or did CMake latch the
#                 architecture list before the setting arrived and report
#                 success anyway.
#   plist         does CFBundleExecutable name the binary that is actually on
#                 disk -- if it does not, codesign reports "code object is not
#                 signed at all" about a NESTED object and mentions neither
#                 the plist nor the cause.
#   codesign      the exact command the release job runs, against a copy, so a
#                 verify run never leaves a signature on the build tree.
#   oxbow         the name, the id and the type a HOST sees, which nothing
#                 else here reaches.
#
#   --bench       the render cost. Not pass/fail -- there is no threshold
#                 worth asserting on somebody else's GPU -- but a verify run
#                 leaves a timing on the record, which is what turns "it feels
#                 slower" into a comparison.
#
# The last four are release-job work done locally on purpose. A check that
# only runs in CI, after a tag, is a check that will catch you after the tag.
set -uo pipefail

cd "$(dirname "$0")/.."

# The universal build IS the thing being verified, so it is built here rather
# than assumed. build/ stays whatever the developer left it.
BUILD="${BUILD:-build-universal}"

failures=()

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures+=("$1"); }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V,
# which demands an explicit layout( location ) on every uniform and varying.
# Those are Vulkan rules and not GLSL ones, and without the flag every shader
# "fails" for reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# A shader may be several ADJACENT raw strings -- MSVC caps one literal at
# about 16 KB -- so everything up to the terminating semicolon is joined.
# Nothing here is split today; the joining is what stops a future split from
# silently dropping half a shader out of this check.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is
	# a fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# has lost track of where this repo keeps its GLSL, and a check that
		# silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders: every one through a real GLSL compiler"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

#---------------------------------------------------------------------------
# The build being verified. Universal, Release, from a clean configure.
#---------------------------------------------------------------------------
step "build: a fresh universal Release build"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
	&& cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "configures and builds"
else
	fail "the universal build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	printf '\nFAILURES:\n'
	printf '  %s\n' "${failures[@]}"
	exit 1
fi

TEST="$BUILD/cdtest"

#---------------------------------------------------------------------------
# The claims.
#---------------------------------------------------------------------------
for check in comb bob pattern adaptive swap lock ring; do
	step "$check"
	if "$TEST" --"$check"; then
		pass "cdtest --$check"
	else
		fail "cdtest --$check"
	fi
done

step "sweep: no control silently dead"
if python3 tools/sweep.py --binary "$TEST" > /tmp/cadence-sweep.txt 2>&1; then
	tail -1 /tmp/cadence-sweep.txt
	pass "every control reaches the picture"
else
	printf '   *** dead controls, see /tmp/cadence-sweep.txt\n'
	tail -6 /tmp/cadence-sweep.txt
	fail "tools/sweep.py reports a dead control"
fi

step "demo: the browser page runs the plugin's own shaders"
if [ -f demo/tools/check_shaders.py ]; then
	if python3 demo/tools/check_shaders.py > /tmp/cadence-demo.txt 2>&1; then
		tail -1 /tmp/cadence-demo.txt
		pass "demo/plugin.js carries the plugin's GLSL, character for character"
	else
		printf '   *** the demo has drifted, see /tmp/cadence-demo.txt\n'
		tail -12 /tmp/cadence-demo.txt
		fail "demo/plugin.js no longer matches source/Shaders.cpp"
	fi
else
	printf '   skipped: demo/tools/check_shaders.py is not here\n'
fi

step "bench: the render cost, for the record"
"$TEST" --bench --frames 60 2>&1 | sed -n '3,8p'

#---------------------------------------------------------------------------
# The bundle, as a host meets it.
#---------------------------------------------------------------------------
BUNDLE="$BUILD/Cadence.bundle"
BIN="$BUNDLE/Contents/MacOS/Cadence"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# Captured, then matched -- never `nm ... | grep -q`. Under `set -o
	# pipefail` a `grep -q` that FINDS its match exits immediately, the writer
	# upstream takes SIGPIPE, and the PIPELINE reports failure even though the
	# symbol is there. It is output-size dependent, so it fires on the bigger
	# binary first and looks intermittent.
	symbols=$( nm -gU "$BIN" 2>/dev/null )
	case "$symbols" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the host will load the bundle and find no plugins" ;;
	esac

	step "lipo: universal"
	archs=$( lipo -archs "$BIN" 2>/dev/null )
	printf '   architectures: %s\n' "$archs"
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$( /usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	ident=$( /usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null )
	if [ "$ident" = "com.stoatworks.ffgl.cadence" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident', expected com.stoatworks.ffgl.cadence"
	fi

	step "codesign"
	# On a COPY, so a verify run never leaves a signature on the build tree
	# that the release job did not put there.
	tmp=$( mktemp -d )
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Cadence.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
		codesign --force --sign - --timestamp=none "$tmp/Cadence.bundle" 2>&1 | sed 's/^/       /'
	fi
	rm -rf "$tmp"

	step "oxbow: the name, id and type a host sees"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		out=$( "$OXBOW" probe "$BUNDLE" 2>&1 )
		printf '%s\n' "$out" | sed -n '1,8p' | sed 's/^/   /'
		case "$out" in *"SW Cadence"*) pass "name is SW Cadence" ;; *) fail "the host does not see the name SW Cadence" ;; esac
		case "$out" in *CD01*) pass "id is CD01" ;; *) fail "the host does not see the id CD01" ;; esac
		case "$out" in *"type:        effect"*) pass "type is effect" ;; *) fail "the host does not see an effect" ;; esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if (( ${#failures[@]} == 0 )); then
	printf '\033[32mall checks passed\033[0m\n'
	exit 0
fi

printf '\033[31mFAILURES:\033[0m\n'
printf '  %s\n' "${failures[@]}"
exit 1
