/**
 * Cadence — browser demo.
 *
 * A television picture is two fields a field period apart, each holding
 * alternate lines. Cadence treats its input as fields with real time between
 * them, puts a film cadence in front, and shows the result on a progressive
 * screen through a deinterlacer you choose — so combing, bob bounce, field-blend
 * ghosting, motion-adaptive mistakes, 3:2 judder and the two-forward-one-back
 * stutter of a swapped field order all fall out of the arithmetic rather than
 * being drawn.
 *
 * All five shader constants below — `kVertexShader`, `kCopyShader`,
 * `kDiffShader`, `kReduceShader` and `kCompositeShader` — are
 * `source/Shaders.cpp`, copied across unedited. `demo/tools/check_shaders.py`
 * compares them character for character against the C++ and is called from
 * `tools/verify.sh`, because two copies of a shader is exactly the arrangement
 * that drifts.
 *
 * The CPU half is a **port**: `source/Controls.cpp`, `source/Pulldown.cpp`
 * (including `InverseTelecine`), the ring and the presentation rule out of
 * `Cadence::ProcessOpenGL`, and the clamping half of `source/Clock.cpp`.
 * Ported rather than re-derived, and **nothing checks the port but a reader** —
 * `check_shaders.py` only sees the GLSL. When the field arithmetic changes in
 * C++, it has to be changed here by hand.
 *
 * ------------------------------------------------------- what is here
 *
 * The ring of recent input frames, the field stream, the field-parity maths,
 * the presentation rule (a deinterlacer is a field late, on purpose), Split,
 * Telecine and Cadence Break, all six modes including Inverse Telecine with its
 * diff and reduce passes and the per-field readback its detector needs, the
 * Fields display, Show Decision, Line Filter and Mix.
 *
 * ------------------------------------------------------- what is missing
 *
 * **Anything audio-driven.** `Break On Onset` and the 64-bin `Audio` buffer are
 * absent, not present and dead: the spectrum reaches the plugin through a
 * Resolume FFT parameter, a browser has no equivalent, and asking a visitor for
 * a microphone to demonstrate a video effect is not a trade worth making. The
 * free-running side of Cadence Break — an edit every `Break Interval` — is here
 * and is the same code path; what you cannot see is a kick drum choosing the
 * moment. `cdtest --tone` measures that part in the repository.
 *
 * **The host clock's unit.** `Clock.cpp` spends most of its length deciding
 * whether the host counts in seconds or in milliseconds, because Resolume sends
 * one and the offline harness the other. A browser's `requestAnimationFrame` is
 * unambiguous, so only the clamping survives here — the same 1/240 … 1/24
 * window, for the same reason. One consequence is visible: **Restart restarts
 * the clip, not the field clock.** The plugin's clock is monotonic and clamped,
 * so a host scrubbing backwards moves it forward by a 240th of a second, and so
 * does this.
 *
 * **The input is not only the clip.** None of the kit's generated clips move
 * fast enough to comb, and a deinterlacer on a still picture shows you nothing
 * at all — every mode collapses to the input, which is the same reason
 * `cdtest`'s own test card moves. So by default this page draws a sweeping bar,
 * a vertically moving block and a pair of thin rules over the chosen clip
 * before handing it to the plugin. That overlay is **this page's**, not the
 * plugin's; the Motion control in the transport turns it off.
 *
 * What this page is NOT: it is the plugin's shaders, not the plugin. No
 * Resolume, no FFGL, no C++ — and GLSL ES 3.00 rather than desktop GL 4.1 core,
 * which the kit's `port()` handles. Nothing here measures anything; `cdtest` in
 * the repository is the reason to believe the maths.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// Pass the RAW `#version 410 core` text to Program: its constructor calls the
// kit's port() on both sources itself, and pre-porting leaves a second #version
// line in the middle of the file.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. MaxUV is folded in once, in
	//the copy pass; every pass after it works on a texture we allocated,
	//where the picture really does fill the texture.
	uv = vUV;
}
`;

const COPY = `#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV; //the part of the input texture that is really picture

in vec2 uv;
out vec4 fragColor;

void main()
{
	//The slot is the picture's own size, so this sample lands on a texel
	//centre and GL_LINEAR returns the texel: the copy is exact, which is
	//what lets the harness read a grey level back out and know which frame
	//it came from.
	fragColor = texture( InputTexture, uv * MaxUV );
}
`;

const DIFF = `#version 410 core

uniform sampler2D FieldA;
uniform sampler2D FieldB;
uniform int Parity; //the lines both fields own: 0 even from the top, 1 odd
uniform int Width;
uniform int Height;
uniform int GridWidth;
uniform int GridHeight;

out vec4 fragColor;

void main()
{
	int gx = int( gl_FragCoord.x );
	int gy = int( gl_FragCoord.y );

	//Spread the grid over the picture, then snap each row to one of the
	//field's own lines. Rows are counted from the TOP, because that is what
	//a field's parity means; the texture's row 0 is the bottom.
	int x = int( ( float( gx ) + 0.5 ) / float( GridWidth ) * float( Width ) );
	int j = int( ( float( gy ) + 0.5 ) / float( GridHeight ) * float( Height / 2 ) );
	int r = min( 2 * j + Parity, Height - 1 );

	ivec2 at = ivec2( x, Height - 1 - r );
	vec3 a   = texelFetch( FieldA, at, 0 ).rgb;
	vec3 b   = texelFetch( FieldB, at, 0 ).rgb;

	float d = dot( abs( a - b ), vec3( 0.299, 0.587, 0.114 ) );
	fragColor = vec4( d, 0.0, 0.0, 1.0 );
}
`;

const REDUCE = `#version 410 core

uniform sampler2D Grid;
uniform int GridHeight;
uniform int Columns; //grid columns per output texel

out vec4 fragColor;

void main()
{
	int gx    = int( gl_FragCoord.x );
	float sum = 0.0;
	for( int i = 0; i < Columns; ++i )
		for( int y = 0; y < GridHeight; ++y )
			sum += texelFetch( Grid, ivec2( gx * Columns + i, y ), 0 ).r;

	fragColor = vec4( sum, 0.0, 0.0, 1.0 );
}
`;

const COMPOSITE = `#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;

uniform sampler2D FieldA;
uniform sampler2D FieldB;
uniform int ParityA;
uniform int ParityB;

uniform int Width;
uniform int Height;

uniform int Mode;         //0 weave, 1 bob, 2 bob linear, 3 blend, 4 adaptive, 5 inverse telecine
uniform int Display;      //0 progressive, 1 fields
uniform float Threshold;  //adaptive: colour difference above which a pixel bobs
uniform int ShowDecision; //adaptive: paint the mask instead of the picture
uniform float LineFilter; //0..1, the side weight is a quarter of it
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

//Row r, counted from the top, of a source frame. Clamped, so a caller asking
//for the row above the first gets the first.
vec4 rowOf( sampler2D field, int x, int r )
{
	r = clamp( r, 0, Height - 1 );
	return texelFetch( field, ivec2( x, Height - 1 - r ), 0 );
}

//A missing row filled by line doubling: the field's own line just above it.
//Row r-1 always belongs to the field when r does not, except at the top of
//the picture, where the only neighbour is below. This is what makes a two-
//line detail sit one line lower on one field than on the other -- the bob
//bounce -- and a one-line detail vanish on the field that does not own it.
vec4 doubled( sampler2D field, int x, int r )
{
	return r > 0 ? rowOf( field, x, r - 1 ) : rowOf( field, x, r + 1 );
}

//A missing row filled by averaging the field's lines either side of it.
vec4 interpolated( sampler2D field, int x, int r )
{
	if( r <= 0 )
		return rowOf( field, x, r + 1 );
	if( r >= Height - 1 )
		return rowOf( field, x, r - 1 );
	return 0.5 * ( rowOf( field, x, r - 1 ) + rowOf( field, x, r + 1 ) );
}

vec4 deinterlace( int x, int r )
{
	int parity = r & 1;

	if( Display == 1 )
	{
		//One field on its own lines and nothing on the others: what a CRT
		//shows in one field period.
		return parity == ParityA ? rowOf( FieldA, x, r ) : vec4( 0.0, 0.0, 0.0, 1.0 );
	}

	if( Mode == 0 || Mode == 5 )
	{
		//Weave: each field on its own lines. A moving edge is in two places
		//at once, one field period apart -- combing.
		return parity == ParityB ? rowOf( FieldB, x, r ) : rowOf( FieldA, x, r );
	}

	if( Mode == 1 )
		return parity == ParityA ? rowOf( FieldA, x, r ) : doubled( FieldA, x, r );

	if( Mode == 2 )
		return parity == ParityA ? rowOf( FieldA, x, r ) : interpolated( FieldA, x, r );

	if( Mode == 3 )
	{
		//Blend: the field that owns the row, averaged with the other field's
		//interpolation of it. A vertical low-pass and a temporal one at once,
		//which is why a moving object ghosts and a still one goes soft.
		vec4 own   = parity == ParityA ? rowOf( FieldA, x, r ) : rowOf( FieldB, x, r );
		vec4 other = parity == ParityA ? interpolated( FieldB, x, r ) : interpolated( FieldA, x, r );
		return 0.5 * ( own + other );
	}

	//Adaptive. The shown field's own rows pass through; each missing row is
	//either woven from the partner field or interpolated from the shown
	//one, decided per pixel by how much the two answers disagree. Too high
	//a threshold and combing leaks through where the picture moved; too low
	//and fine detail that merely differed between the fields goes soft.
	if( parity == ParityA )
		return ShowDecision == 1 ? vec4( 0.25, 0.25, 0.25, 1.0 ) : rowOf( FieldA, x, r );

	vec4 woven  = rowOf( FieldB, x, r );
	vec4 bobbed = interpolated( FieldA, x, r );
	vec3 d      = abs( woven.rgb - bobbed.rgb );
	bool bob    = max( d.r, max( d.g, d.b ) ) > Threshold;

	if( ShowDecision == 1 )
		return bob ? vec4( 1.0 ) : vec4( 0.0, 0.0, 0.0, 1.0 );

	return bob ? bobbed : woven;
}

void main()
{
	//Integer pixel from the interpolated uv rather than from gl_FragCoord,
	//so the host's viewport origin does not matter and an output that is
	//not the picture's size scales the fields rather than cropping them.
	int x = int( uv.x * float( Width ) );
	int r = Height - 1 - int( uv.y * float( Height ) );
	x     = clamp( x, 0, Width - 1 );
	r     = clamp( r, 0, Height - 1 );

	vec4 colour = deinterlace( x, r );

	if( LineFilter > 0.0 && Display == 0 )
	{
		float side = 0.25 * LineFilter;
		colour = colour * ( 1.0 - 2.0 * side )
		         + side * ( deinterlace( x, max( r - 1, 0 ) ) + deinterlace( x, min( r + 1, Height - 1 ) ) );
	}

	vec4 source = texture( InputTexture, uv * MaxUV );
	fragColor   = mix( source, colour, MixAmount );
}
`;

//---------------------------------------------------------------------------
// This page's own pass. NOT the plugin's, and check_shaders.py does not look
// at it.
//
// It draws the moving detail a deinterlacer needs in front of it, over
// whichever generated clip is selected, and hands the result to the plugin as
// the input. Same three things `cdtest`'s test card carries and for the same
// reasons: a bar sweeping horizontally (combing, and the swapped-field-order
// stutter), a block moving vertically (bob bounce), and a one-line and a
// two-line static rule on even rows from the top (twitter — a one-line detail
// exists on only one field at all).
//
// Rows are counted from the TOP with the same expression the composite uses,
// so a rule that is meant to land on an even row really does.
//---------------------------------------------------------------------------

const MOTION = `#version 410 core

uniform sampler2D InputTexture;
uniform vec2 Resolution;
uniform float Time;
uniform int Motion; //0 clip untouched, 1 overlay

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 base = texture( InputTexture, uv );
	if( Motion == 0 )
	{
		fragColor = base;
		return;
	}

	float w = Resolution.x;
	float h = Resolution.y;
	float x = uv.x * w;
	int row = int( h ) - 1 - int( uv.y * h );
	float r = float( row );

	vec3 c = base.rgb;

	//The sweeping bar. A third of the frame width a second, so at sixty
	//fields a second the offset between one field and the next is several
	//pixels and the comb is a comb rather than a suspicion.
	float barW = max( 4.0, w / 40.0 );
	float barX = mod( Time * w * 0.35, w + barW ) - barW;
	if( x >= barX && x < barX + barW && r > h * 0.16 && r < h * 0.64 )
		c = vec3( 0.62, 0.78, 0.98 );

	//A block moving vertically: line doubling puts it a line lower on one
	//field than on the other, which is the bounce.
	float blockY = ( 0.5 + 0.33 * sin( Time * 1.9 ) ) * h;
	if( abs( x - w * 0.87 ) < w * 0.04 && abs( r - blockY ) < h * 0.045 )
		c = vec3( 0.99, 0.93, 0.55 );

	//A one-line and a two-line rule, both starting on an even row from the
	//top, so under Top First they belong to the top field.
	int rule1 = ( int( h * 0.30 ) / 2 ) * 2;
	int rule2 = ( int( h * 0.36 ) / 2 ) * 2;
	if( x > w * 0.30 && x < w * 0.95 && ( row == rule1 || row == rule2 || row == rule2 + 1 ) )
		c = vec3( 1.0 );

	fragColor = vec4( c, 1.0 );
}
`;

//---------------------------------------------------------------------------
// Controls.cpp — what a slider position means.
//
// `std::lround` rounds half away from zero and `Math.round` rounds half up;
// every value reaching these is non-negative, where the two agree.
//---------------------------------------------------------------------------

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);

/** Equal slider movements are equal *ratios*. */
const geometric = (from, to, t) => from * Math.pow(to / from, clamp01(t));

const SOURCE_RATES = [24, 25, 30, 60];

function sourceRateFromOption(optionValue) {
  const index = Math.round(optionValue);
  return index >= 0 && index < SOURCE_RATES.length ? SOURCE_RATES[index] : 24;
}

function fieldRateFromOption(optionValue) {
  return Math.round(optionValue) === 0 ? 50 : 60;
}

const adaptiveThresholdFromParam = (value) => clamp01(value);
const lockSecondsFromParam = (value) => geometric(0.05, 4.0, value);
const breakIntervalFromParam = (value) => geometric(0.25, 30.0, value);
const lineFilterFromParam = (value) => clamp01(value);

//---------------------------------------------------------------------------
// Pulldown.cpp — the cadence, as arithmetic.
//
// Field `k` at field rate Rf covers [k/Rf, (k+1)/Rf) and carries the source
// frame current at the middle of that slice: n = floor( (k + 0.5) Rs / Rf ).
// One rule, and 2:3, 2:2 and one-field-per-frame are all special cases of it.
//---------------------------------------------------------------------------

function gcd(a, b) {
  while (b !== 0) {
    const t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/** C++ gives a negative left operand a negative remainder; a phase shift can. */
function floorMod(a, m) {
  const r = a % m;
  return r < 0 ? r + m : r;
}

function cycleFields(sourceRate, fieldRate) {
  if (sourceRate <= 0 || fieldRate <= 0) return 1;
  return fieldRate / gcd(sourceRate, fieldRate);
}

function sourceFrameOfField(k, phase, sourceRate, fieldRate) {
  // Kept in integers by doubling, exactly as the C++ does: a float division
  // landing a hair under an integer is a different pattern.
  const numerator = (2 * (k + phase) + 1) * sourceRate;
  const denominator = 2 * fieldRate;
  let q = Math.trunc(numerator / denominator);
  if (numerator % denominator !== 0 && numerator < 0) q -= 1;
  return q;
}

function fieldParity(k, bottomFieldFirst) {
  return floorMod(k, 2) ^ (bottomFieldFirst ? 1 : 0);
}

function sourceFrameTime(n, phase, sourceRate, fieldRate) {
  return n / sourceRate - (phase + 1) / fieldRate;
}

/** A PCG output mix, exact in 32 bits. Math.imul is the only way to get one. */
function hashInt(seed) {
  const state = ((Math.imul(seed >>> 0, 747796405) >>> 0) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

/**
 * A naive inverse telecine, ported from `Pulldown.cpp`.
 *
 * It watches one number per field — how different the field is from the field
 * two before it, which owns the same lines — and looks for the field that is a
 * *repeat*. What makes it naive is exactly what the plugin is for: the scores
 * are a running average with a time constant of Lock Time, so after an edit the
 * old lock persists and the pairs it weaves straddle two film frames and comb.
 */
class InverseTelecine {
  static PERIOD = 5;

  constructor() {
    this.reset();
  }

  reset() {
    this.score = [1, 1, 1, 1, 1];
    this.locked = 0;
    this.lockChanges = 0;
  }

  observe(k, sameParityDiff, lockSeconds, fieldSeconds) {
    const position = floorMod(k, InverseTelecine.PERIOD);

    // One-pole average with a time constant of Lock Time. Zero snaps.
    const alpha = lockSeconds <= 1e-4 ? 1.0 : 1.0 - Math.exp(-fieldSeconds / lockSeconds);
    this.score[position] += (sameParityDiff - this.score[position]) * alpha;

    // Re-lock only when a position is clearly better than the current one. A
    // lock that never settles is one an operator cannot watch happen.
    let best = this.locked;
    for (let i = 0; i < InverseTelecine.PERIOD; i += 1) {
      if (this.score[i] < this.score[best]) best = i;
    }

    if (best !== this.locked && this.score[best] < this.score[this.locked] * 0.8) {
      this.locked = best;
      this.lockChanges += 1;
    }
  }

  position(k) {
    return floorMod(k - this.locked, InverseTelecine.PERIOD);
  }

  /** At field k, weave (k-1, k)? Otherwise (k-2, k-1). */
  weaveLatest(k) {
    const position = this.position(k);
    return position === 4 || position === 0 || position === 2;
  }
}

//---------------------------------------------------------------------------
// Shaders.h — the measurement grid the diff pass renders at, and the row the
// reduce pass leaves. 9,216 samples of a field: sparse against a picture,
// plenty for a mean, and the same cost at 720p and 4K.
//---------------------------------------------------------------------------
const DIFF_GRID_WIDTH = 128;
const DIFF_GRID_HEIGHT = 72;
const REDUCE_WIDTH = 16;

// Cadence.h / Cadence.cpp.
const MAX_RING = 8;
const MAX_FIELDS = 8;
const RING_SPLIT = 4;
const RING_TELECINE = 8;
const MAX_FIELDS_PER_FRAME = 4;

// Clock.cpp. Shorter than the first is a duplicated call or a clock that has
// not moved; longer is a stall or a scrub. Both are clamped, not believed.
const MIN_FRAME_SECONDS = 1 / 240;
const MAX_FRAME_SECONDS = 1 / 24;

//---------------------------------------------------------------------------
// The renderer.
//---------------------------------------------------------------------------

function createRenderer(gl, quad) {
  const motionShader = new Program(gl, VERTEX, MOTION, 'motion (this page, not the plugin)');
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const diffShader = new Program(gl, VERTEX, DIFF, 'diff');
  const reduceShader = new Program(gl, VERTEX, REDUCE, 'reduce');
  const compositeShader = new Program(gl, VERTEX, COMPOSITE, 'composite');

  // The card this page draws, and then treats as the plugin's input.
  const card = new PassBuffer(gl, { filter: 'linear' });

  // The ring. Nearest, as the plugin's slots are: every field read in the
  // composite is a texelFetch at an integer row, because two rows are two
  // different fields and a bilinear sample between them is a sample between
  // two fields.
  const slots = [];
  for (let i = 0; i < MAX_RING; i += 1) slots.push(new PassBuffer(gl, { filter: 'nearest' }));

  const diffGrid = new PassBuffer(gl, { filter: 'nearest' });
  const reduceRow = new PassBuffer(gl, { filter: 'nearest' });
  const sums = new Float32Array(REDUCE_WIDTH * 4);

  const state = {
    slotSerial: new Array(MAX_RING).fill(-1),
    slotTime: new Array(MAX_RING).fill(0),
    ringSize: 0,
    ringWidth: 0,
    ringHeight: 0,
    ringFilled: 0,
    frameSerial: 0,

    fields: [],
    fieldsEmitted: 0,
    nextTelecineField: -1,
    cadencePhase: 0,
    breakCount: 0,
    lastBreakTime: -1,

    itc: new InverseTelecine(),

    // Clock.cpp, minus the unit calibration.
    now: 0,
    lastTime: -1,
  };

  function clearRing() {
    state.slotSerial.fill(-1);
    state.slotTime.fill(0);
    state.ringFilled = 0;
    state.fields = [];
    state.fieldsEmitted = 0;
    state.nextTelecineField = -1;
    state.itc.reset();
  }

  /**
   * Bring the ring to this size and length, and empty it if either changed.
   *
   * `PassBuffer.ensure()` returns `this` on both paths, not a boolean, so the
   * comparison below is against our own record of the size rather than against
   * a return value — the kit throws rather than reporting a failure.
   */
  function ensureRing(width, height, frames) {
    frames = Math.min(Math.max(frames, 2), MAX_RING);
    const same = width === state.ringWidth && height === state.ringHeight && frames === state.ringSize;

    for (let i = 0; i < MAX_RING; i += 1) {
      // Shrinking really does free them: four spare frames at 1080p is
      // thirty-three megabytes the visitor believes they gave back.
      if (i >= frames) slots[i].dispose();
      else slots[i].ensure(width, height, gl.RGBA8);
    }

    if (!same) {
      // The ring's indexing is modulo its own length, so changing the length
      // does not shuffle the contents, it reinterprets them: every slot still
      // holds a real frame, filed under the wrong serial. Starting empty costs
      // a few frames and is the only answer that is not wrong.
      for (let i = 0; i < frames; i += 1) slots[i].clearTo(0, 0, 0, 0);
      clearRing();
      state.ringWidth = width;
      state.ringHeight = height;
      state.ringSize = frames;
    }
  }

  function textureOfSerial(serial) {
    if (state.ringSize <= 0) return null;
    const slot = serial % state.ringSize;
    if (serial >= 0 && state.slotSerial[slot] === serial) return slots[slot].texture;

    // Overwritten, or never there. The newest frame is the least wrong answer:
    // a field a frame late is a stutter, a field of black is a hole.
    const newest = state.frameSerial - 1;
    return newest >= 0 ? slots[newest % state.ringSize].texture : null;
  }

  function serialAtOrBefore(seconds) {
    let best = -1;
    let bestTime = -1;
    let oldest = -1;
    let oldestTime = 0;

    for (let i = 0; i < state.ringSize; i += 1) {
      if (state.slotSerial[i] < 0) continue;
      if (state.slotTime[i] <= seconds + 1e-6 && (best < 0 || state.slotTime[i] > bestTime)) {
        best = state.slotSerial[i];
        bestTime = state.slotTime[i];
      }
      if (oldest < 0 || state.slotTime[i] < oldestTime) {
        oldest = state.slotSerial[i];
        oldestTime = state.slotTime[i];
      }
    }

    return best >= 0 ? best : oldest;
  }

  function fieldAt(index) {
    for (let i = state.fields.length - 1; i >= 0; i -= 1) {
      if (state.fields[i].index === index) return state.fields[i];
    }
    return null;
  }

  function pushField(index, parity, sourceSerial) {
    if (state.fields.length === MAX_FIELDS) state.fields.shift();
    state.fields.push({ index, parity, sourceSerial });
  }

  /** A different phase, never the same one: an edit that lands where it left is not an edit. */
  function cadenceBreak(now, cycle) {
    if (cycle > 1) {
      const h = hashInt(((Math.imul(state.breakCount, 2654435761) >>> 0) ^ 0x9e3779b9) >>> 0);
      state.cadencePhase = (state.cadencePhase + 1 + (h % (cycle - 1))) % cycle;
    }
    state.breakCount += 1;
    state.lastBreakTime = now;
  }

  /**
   * The inverse telecine's one number: how different field `k` is from the
   * field two before it. Runs the diff and reduce passes and reads sixteen
   * floats back.
   *
   * That readback is synchronous and it stalls, here as in the plugin — the
   * cost is on the record in the README's bench table, Inverse Telecine against
   * Weave. A browser feels it more than a host does, and a telecine emitting
   * four fields in one frame pays for it four times.
   */
  function sameParityDifference(current, twoBack) {
    diffGrid.bind();
    diffShader.use();
    bindTexture(gl, 0, textureOfSerial(current.sourceSerial));
    bindTexture(gl, 1, textureOfSerial(twoBack.sourceSerial));
    diffShader.setSampler('FieldA', 0);
    diffShader.setSampler('FieldB', 1);
    diffShader.setInt('Parity', current.parity);
    diffShader.setInt('Width', state.ringWidth);
    diffShader.setInt('Height', state.ringHeight);
    diffShader.setInt('GridWidth', DIFF_GRID_WIDTH);
    diffShader.setInt('GridHeight', DIFF_GRID_HEIGHT);
    quad.draw();

    reduceRow.bind();
    reduceShader.use();
    bindTexture(gl, 0, diffGrid.texture);
    reduceShader.setSampler('Grid', 0);
    reduceShader.setInt('GridHeight', DIFF_GRID_HEIGHT);
    reduceShader.setInt('Columns', DIFF_GRID_WIDTH / REDUCE_WIDTH);
    quad.draw();

    // RGBA/FLOAT off an RGBA32F attachment, which EXT_color_buffer_float makes
    // legal — `needFloat` below is what asks for it, and the kit throws by name
    // if the browser will not give it.
    gl.readPixels(0, 0, REDUCE_WIDTH, 1, gl.RGBA, gl.FLOAT, sums);

    let total = 0;
    for (let i = 0; i < REDUCE_WIDTH; i += 1) total += sums[i * 4];
    return total / (DIFF_GRID_WIDTH * DIFF_GRID_HEIGHT);
  }

  return {
    render({ input, params, width, height, time, variant }) {
      //-------------------------------------------------------------------
      // 0. This page's own pass: the clip, with the moving detail a
      //    deinterlacer needs in front of it. Everything after this line
      //    treats the result as the plugin's input.
      //-------------------------------------------------------------------
      card.ensure(width, height, gl.RGBA8);
      card.bind();
      motionShader.use();
      bindTexture(gl, 0, input.texture);
      motionShader.setSampler('InputTexture', 0);
      motionShader.set('Resolution', width, height);
      motionShader.set('Time', time);
      motionShader.setInt('Motion', variant === 'clip' ? 0 : 1);
      gl.disable(gl.BLEND);
      quad.draw();

      //-------------------------------------------------------------------
      // 1. Time. Clock.cpp's clamping, without its unit calibration: a
      //    stalled or backgrounded tab must not move the field clock by half
      //    a second in one frame, and a restart must not move it backwards.
      //-------------------------------------------------------------------
      let dt = 1 / 60;
      if (state.lastTime >= 0) {
        dt = Math.min(Math.max(time - state.lastTime, MIN_FRAME_SECONDS), MAX_FRAME_SECONDS);
        state.now += dt;
      }
      state.lastTime = time;
      const now = state.now;

      //-------------------------------------------------------------------
      // 2. What the controls say.
      //-------------------------------------------------------------------
      const source = params.option('source');
      const mode = params.option('mode');
      const display = params.option('display');
      const sourceRate = sourceRateFromOption(params.get('sourceRate'));
      const fieldRate = fieldRateFromOption(params.get('fieldRate'));
      const bottomFirst = params.get('fieldOrder') >= 0.5;
      const swap = params.get('swap') >= 0.5;
      const lockSeconds = lockSecondsFromParam(params.get('lock'));
      const breakInterval = breakIntervalFromParam(params.get('breakInterval'));

      //-------------------------------------------------------------------
      // 3. Buffers. Every allocation here, before anything binds a texture —
      //    the plugin's rule, because in ffglex a scoped binding CLEARS to 0
      //    on scope exit rather than restoring and an allocation mid-chain
      //    silently unbinds the input. WebGL is not ffglex, but keeping the
      //    order keeps the two readable side by side.
      //-------------------------------------------------------------------
      ensureRing(width, height, source === 0 ? RING_SPLIT : RING_TELECINE);
      if (mode === 5) {
        diffGrid.ensure(DIFF_GRID_WIDTH, DIFF_GRID_HEIGHT, gl.RGBA32F);
        reduceRow.ensure(REDUCE_WIDTH, 1, gl.RGBA32F);
      }

      //-------------------------------------------------------------------
      // 4. Capture: the picture into the ring slot its serial lands on.
      //-------------------------------------------------------------------
      const serial = state.frameSerial;
      state.frameSerial += 1;
      const slot = serial % state.ringSize;

      slots[slot].bind();
      copyShader.use();
      bindTexture(gl, 0, card.texture);
      copyShader.setSampler('InputTexture', 0);
      // The card is exactly the picture, so all of the texture is picture.
      copyShader.set('MaxUV', 1, 1);
      quad.draw();

      state.slotSerial[slot] = serial;
      state.slotTime[slot] = now;
      state.ringFilled = Math.min(state.ringFilled + 1, state.ringSize);

      //-------------------------------------------------------------------
      // 5. Fields. Which frames become fields, and when.
      //-------------------------------------------------------------------
      const wantItc = mode === 5;

      const emit = (k, sourceSerial, fieldSeconds) => {
        const parity = fieldParity(k, bottomFirst);

        // Measured before the field joins the list, so "two back" means it.
        if (wantItc) {
          const twoBack = fieldAt(k - 2);
          if (twoBack) {
            const current = { index: k, parity, sourceSerial };
            state.itc.observe(k, sameParityDifference(current, twoBack), lockSeconds, fieldSeconds);
          }
        }

        pushField(k, parity, sourceSerial);
        state.fieldsEmitted += 1;
      };

      if (source === 0) {
        // Split: one field per input frame, alternating parity, with a real
        // frame period between them. Field Rate and Source Rate say nothing.
        emit(state.fieldsEmitted, serial, dt);
      } else {
        const cycle = cycleFields(sourceRate, fieldRate);

        if (source === 2) {
          if (state.lastBreakTime < 0) state.lastBreakTime = now;
          if (now - state.lastBreakTime >= breakInterval) cadenceBreak(now, cycle);
          // Break On Onset is absent — see the header. Nothing else fires one.
        }

        // Field k is due at k / Rf, with an epsilon of a ten-thousandth of a
        // field because a clock that is a sum of frame deltas does not always
        // land on the integer it should.
        const due = Math.floor(now * fieldRate + 1e-4);
        if (state.nextTelecineField < 0) state.nextTelecineField = due;

        // A stall longer than the bound is skipped, not caught up: four fields
        // emitted in one frame is a stutter, forty is a freeze.
        if (due - state.nextTelecineField > MAX_FIELDS_PER_FRAME) {
          state.nextTelecineField = due - MAX_FIELDS_PER_FRAME;
        }

        while (state.nextTelecineField <= due) {
          const k = state.nextTelecineField;
          state.nextTelecineField += 1;
          const n = sourceFrameOfField(k, state.cadencePhase, sourceRate, fieldRate);
          const shot = sourceFrameTime(n, state.cadencePhase, sourceRate, fieldRate);
          emit(k, serialAtOrBefore(shot), 1 / fieldRate);
        }
      }

      //-------------------------------------------------------------------
      // 6. Presentation. Which two fields the deinterlacer sees, and which
      //    one it is showing.
      //
      //    The deinterlacer is frame-based: it consumes fields in pairs
      //    (2j, 2j+1) and only starts a pair once both fields are in. So at
      //    field K the pair in hand is (K-1, K) when K is odd and (K-2, K-1)
      //    when K is even — one field of latency, which is what a real one
      //    has. Swap Field Order is the belief about their order being wrong.
      //-------------------------------------------------------------------
      if (state.fields.length === 0) return;

      const K = state.fields[state.fields.length - 1].index;
      const pairFirst = K >= 1 ? (K - 1) & ~1 : K;
      const pairSecond = K >= 1 ? pairFirst + 1 : K;
      const firstHalf = (K & 1) !== 0 || K === 0;
      const shown = firstHalf ? (swap ? pairSecond : pairFirst) : (swap ? pairFirst : pairSecond);

      let fieldAIndex = pairFirst;
      let fieldBIndex = pairSecond;

      if (mode === 5) {
        // Inverse telecine: the detector's belief about which two fields are
        // one film frame. Under a wrong lock the pair straddles two, and that
        // is the artefact.
        if (K >= 2) {
          fieldAIndex = state.itc.weaveLatest(K) ? K - 1 : K - 2;
          fieldBIndex = fieldAIndex + 1;
        }
      } else if (mode === 1 || mode === 2) {
        fieldAIndex = shown;
        fieldBIndex = shown;
      } else if (mode === 4) {
        fieldAIndex = shown;
        fieldBIndex = pairFirst + pairSecond - shown;
      }
      // Weave and Blend take the pair as it stands.

      if (display === 1) fieldAIndex = shown;

      // A field that has already left the list — start-up, or the first frame
      // after a rebuild — falls back to the newest, the least wrong picture
      // available.
      const newest = state.fields[state.fields.length - 1];
      const a = fieldAt(fieldAIndex) ?? newest;
      const b = fieldAt(fieldBIndex) ?? newest;

      //-------------------------------------------------------------------
      // 7. Composite, straight to the canvas. The kit bound it and set the
      //    viewport before calling us, and the passes above have since bound
      //    framebuffers of their own at other sizes.
      //-------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      compositeShader.use();
      bindTexture(gl, 0, card.texture);
      bindTexture(gl, 1, textureOfSerial(a.sourceSerial));
      bindTexture(gl, 2, textureOfSerial(b.sourceSerial));

      compositeShader.setSampler('InputTexture', 0);
      compositeShader.set('MaxUV', 1, 1);
      compositeShader.setSampler('FieldA', 1);
      compositeShader.setSampler('FieldB', 2);
      compositeShader.setInt('ParityA', a.parity);
      compositeShader.setInt('ParityB', b.parity);
      compositeShader.setInt('Width', state.ringWidth);
      compositeShader.setInt('Height', state.ringHeight);
      compositeShader.setInt('Mode', mode);
      compositeShader.setInt('Display', display);
      compositeShader.set('Threshold', adaptiveThresholdFromParam(params.get('threshold')));
      compositeShader.setInt('ShowDecision', params.get('showDecision') >= 0.5 ? 1 : 0);
      compositeShader.set('LineFilter', lineFilterFromParam(params.get('lineFilter')));
      compositeShader.set('MixAmount', params.get('mix'));
      quad.draw();
    },
  };
}

//---------------------------------------------------------------------------
// The controls, read out of the plugin's own constructor. Same names, same
// groups, same order, same defaults, same dropdown elements.
//
// Absent: `Break On Onset` and the 64-bin `Audio` buffer, for the reason at the
// top of this file, and the About block, which is four buttons that open a
// browser.
//---------------------------------------------------------------------------

const seconds = (convert) => (value) => {
  const s = convert(value);
  return s < 1 ? `${(s * 1000).toFixed(0)} ms` : `${s.toFixed(2)} s`;
};

mountDemo({
  name: 'Cadence',
  pluginId: 'CD01',
  tagline:
    'Interlace, pulldown and every way a deinterlacer gets it wrong. A field is a slice of time as much as a slice of lines: treat the input as fields with real time between them, put a film cadence in front, and show the result on a progressive screen through the deinterlacer you choose. Combing, bob bounce, field-blend ghosting, motion-adaptive mistakes, 3:2 judder, a cadence break the inverse telecine mis-locks on, and the two-forward-one-back stutter of a swapped field order all fall out of the arithmetic rather than being drawn.',
  repo: 'https://github.com/stoatworks-labs/cadence',

  blurb:
    "It is Cadence's own GLSL, ported from the repository to WebGL2 and running on generated clips in this page — with moving detail drawn over them by this page, because a deinterlacer on a still picture shows you nothing at all. Same parameters, same field arithmetic, no install.",

  // The diff and reduce passes the inverse telecine's detector reads are
  // RGBA32F, and a float render target is an opt-in in WebGL2
  // (EXT_color_buffer_float) where desktop GL simply has it. Eight bits would
  // quantise a mean absolute difference that is routinely a thousandth, and the
  // detector would lock on the quantisation rather than on the cadence.
  needFloat: true,

  // A field the picture does not own is opaque black under the Fields display,
  // and every other mode is opaque too: there is no alpha here to put anything
  // behind.
  showBackdrop: false,

  variants: {
    label: 'Motion',
    default: 'bar',
    options: [
      {
        id: 'bar',
        name: 'Bar over clip',
        hint: "This page draws a sweeping bar, a vertically moving block and two thin rules over the clip. Not the plugin's — the generated clips do not move fast enough to comb.",
      },
      {
        id: 'clip',
        name: 'Clip only',
        hint: 'The generated clip untouched. Most modes will look like the input, which is what a deinterlacer does to a picture that is barely moving.',
      },
    ],
  },

  params: [
    { id: 'source', name: 'Field Source', type: 'option', default: 0, group: 'Source',
      elements: ['Split', 'Telecine', 'Cadence Break'],
      hint: 'Split: one field per input frame, alternating parity — the honest one, and the only one where Source Rate and Field Rate say nothing. Telecine: the input laid out as Source Rate into Field Rate. Cadence Break: telecine with an edit every Break Interval.' },
    { id: 'sourceRate', name: 'Source Rate', type: 'option', default: 0, group: 'Source',
      elements: ['24', '25', '30', '60'],
      hint: 'A list and not a slider because 24, 25, 30 and 60 are the only values that mean anything: a source at 27 fps has no cadence anybody recognises.' },
    { id: 'fieldRate', name: 'Field Rate', type: 'option', default: 1, group: 'Source',
      elements: ['50', '60'],
      hint: '24 into 60 is 2:3 — A A B B B C C D D D. 25 into 50 and 30 into 60 are both 2:2, which has no repeat field, which is why an inverse telecine has nothing to lock to on either.' },
    { id: 'fieldOrder', name: 'Field Order', type: 'option', default: 0, group: 'Source',
      elements: ['Top First', 'Bottom First'],
      hint: 'Which lines the even fields own. Top first means the even fields are the top fields.' },
    { id: 'swap', name: 'Swap Field Order', type: 'boolean', default: 0, group: 'Source',
      hint: 'The deinterlacer believing the pair was shot the other way round. Invisible under Weave — a weave uses both fields and does not care which came first — and a two-forward-one-back stutter under anything that shows one field at a time.' },
    { id: 'breakInterval', name: 'Break Interval', type: 'standard', default: 0.43, group: 'Source',
      display: seconds(breakIntervalFromParam),
      hint: 'How often Cadence Break restarts the pulldown at a different phase. Geometric, 0.25 s to 30 s.' },

    { id: 'mode', name: 'Mode', type: 'option', default: 0, group: 'Deinterlace',
      elements: ['Weave', 'Bob', 'Bob Linear', 'Blend', 'Adaptive', 'Inverse Telecine'],
      hint: 'Weave puts each field on its own lines, so a moving edge is in two places a field period apart. Bob line-doubles one field, Bob Linear interpolates it. Blend averages the two. Adaptive decides per pixel. Inverse Telecine tries to find the film frames again.' },
    { id: 'threshold', name: 'Adaptive Threshold', type: 'standard', default: 0.15, group: 'Deinterlace',
      display: (v) => `${v.toFixed(2)} (${Math.round(v * 255)}/255)`,
      hint: 'How far the woven and the interpolated answers may disagree before the pixel bobs. At 0 every non-identical pixel bobs and Adaptive IS Bob Linear; at 1 nothing can exceed it and Adaptive IS Weave. Both ends are pixel-exact, and cdtest --adaptive proves it.' },
    { id: 'lock', name: 'Lock Time', type: 'standard', default: 0.5, group: 'Deinterlace',
      display: seconds(lockSecondsFromParam),
      hint: 'The time constant the inverse telecine’s cadence scores settle with — and so how long after an edit it keeps weaving the wrong pair. Geometric, 0.05 s to 4 s.' },
    { id: 'showDecision', name: 'Show Decision', type: 'boolean', default: 0, group: 'Deinterlace',
      hint: 'Paint the adaptive mask instead of the picture: white where the pixel bobbed, black where it wove, mid-grey on the rows the shown field already owns. The fastest way to see what Adaptive Threshold is actually doing.' },

    { id: 'display', name: 'Display', type: 'option', default: 0, group: 'Display',
      elements: ['Progressive', 'Fields'],
      hint: 'Fields shows one field on its own lines and black on the others — what a CRT puts up in one field period. It bypasses the deinterlacer entirely, so Mode has nothing to say while it is on.' },
    { id: 'lineFilter', name: 'Line Filter', type: 'standard', default: 0, group: 'Display',
      display: (v) => {
        const side = 0.25 * lineFilterFromParam(v);
        return side === 0 ? 'off' : `[${side.toFixed(2)} ${(1 - 2 * side).toFixed(2)} ${side.toFixed(2)}]`;
      },
      hint: 'A three-tap vertical filter folded into the same pass. The side weight is a quarter of the slider, so at 1 the kernel is [0.25 0.5 0.25] — the anti-twitter filter a broadcast chain puts in front of an interlaced output.' },

    { id: 'mix', name: 'Mix', type: 'standard', default: 1, group: 'Output',
      display: (v) => `${Math.round(v * 100)}%`,
      hint: 'Against the untouched input, so it is a direct A/B with the source.' },
  ],

  sources: ['grid', 'detail', 'scene', 'bars', 'ramp', 'spot'],

  // Combinations chosen for this page. The plugin ships no factory presets, so
  // unlike macroblock's these are not mirrored from a table in the repository —
  // every value is a real control at a real position, but the choice is ours.
  presets: {
    'Combing': { source: 0, mode: 0 },
    'Bob bounce': { source: 0, mode: 1 },
    'Bob, interpolated': { source: 0, mode: 2 },
    'Field-blend ghosting': { source: 0, mode: 3 },
    'Adaptive, mask shown': { source: 0, mode: 4, showDecision: 1 },
    'Adaptive set too low': { source: 0, mode: 4, threshold: 0.01 },
    'One field at a time': { source: 0, display: 1 },
    'Swapped field order': { source: 0, mode: 1, swap: 1 },
    '2:3 judder (24 into 60)': { source: 1, sourceRate: 0, fieldRate: 1, mode: 0 },
    'Inverse telecine, locked': { source: 1, sourceRate: 0, fieldRate: 1, mode: 5 },
    'Inverse telecine, breaking': { source: 2, sourceRate: 0, fieldRate: 1, mode: 5, breakInterval: 0.3 },
    'Nothing to lock to (2:2)': { source: 1, sourceRate: 1, fieldRate: 0, mode: 5 },
    'Anti-twitter filter': { source: 0, mode: 0, lineFilter: 1 },
  },

  differences: [
    'The audio side is not here at all. Break On Onset and the plugin’s 64-bin Audio buffer are absent rather than present and dead: the spectrum reaches the plugin through a Resolume FFT parameter and a browser has no equivalent. The free-running half of Cadence Break — an edit every Break Interval — is here and is the same code path; what you cannot see is a kick drum choosing the moment. cdtest --tone measures that part in the repository.',
    'The moving detail is this page’s, not the plugin’s. None of the generated clips move fast enough to comb, and on a still picture every mode collapses to the input — which is exactly why the plugin’s own offline test card moves. So a sweeping bar, a vertically moving block and two thin static rules are drawn over the clip before it reaches the plugin. Set Motion to “Clip only” in the transport to take them away and see what a barely-moving picture looks like.',
    'Everything else on the CPU side is a hand port: Controls.cpp, Pulldown.cpp including the inverse telecine’s detector, the ring of recent frames and the presentation rule out of Cadence::ProcessOpenGL. Nothing checks that port but a reader. demo/tools/check_shaders.py only compares the five shaders, and it fails the repository’s verify script if a character of them drifts from source/Shaders.cpp.',
    'JavaScript has no float. The plugin’s conversions and the inverse telecine’s running average are 32-bit in C++ and 64-bit here, so Lock Time’s 447 ms and Break Interval’s 1.96 s agree with the plugin to about eight significant figures rather than exactly. The integer field arithmetic — which source frame a field carries, which lines it owns, which two fields get woven — has no such gap and agrees exactly.',
    'The clock is the browser’s, and only the clamping half of Clock.cpp survives. Frame deltas are held to the same 1/240 … 1/24 window the plugin holds its host’s to, so a backgrounded tab cannot emit forty fields in one frame. The rest of that file decides whether the host counts in seconds or milliseconds, which does not arise here. One visible consequence: Restart restarts the clip, not the field clock — the plugin’s clock is monotonic, and a host scrubbing backwards moves it forward by a 240th of a second, as this does.',
    'Changing the clip, the composition size or Field Source empties the ring, exactly as the plugin does on a resize: the ring is indexed modulo its own length, so changing that length reinterprets its contents rather than shuffling them. Give it a second afterwards — until it refills, every field falls back to the newest frame and the picture is briefly just the input.',
    'The presets are this page’s own combinations. The plugin ships no factory preset table, so unlike some other demos in this fleet there is nothing here mirrored from the repository; every value is a real control at a real position, but the choice of which is ours.',
    'The plugin’s numerical proof — a bar moving v pixels a frame offset between the even and odd lines by exactly v, 2:3 read back out of the picture as A A B B B C C D D D, Adaptive at its two extremes matching Bob Linear and Weave byte for byte, and combing counted exactly by comparing the two fields’ source serials — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
