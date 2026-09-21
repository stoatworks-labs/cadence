#include "Shaders.h"

namespace cadence
{

const char* const kVertexShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 1: copy, into a ring slot.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 2: diff. One point per grid texel, on the lines both fields own.
//---------------------------------------------------------------------------
const char* const kDiffShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 3: reduce. Each output texel sums a band of columns of the grid.
//---------------------------------------------------------------------------
const char* const kReduceShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 4: composite. The deinterlacer.
//
// FieldA and FieldB are whole source frames; a field is which of their rows
// the shader is allowed to read. ParityA and ParityB say which. What the two
// fields ARE depends on the mode, and the CPU decides that (see
// Cadence.cpp, "presentation"):
//
//   Weave, Blend, Inverse Telecine   A = the older field of the pair, B = the newer
//   Bob, Bob Linear                  A = the field being shown; B unused
//   Adaptive                         A = the field being shown, B = its partner
//   Display = Fields                 A = the field being shown
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

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
)";

} // namespace cadence
