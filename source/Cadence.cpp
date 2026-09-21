#include "Cadence.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

using namespace ffglex;
using namespace cadence;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Cadence >,// Create method
	"CD01",                  // Plugin unique ID of maximum length 4.
	"SW Cadence",            // Plugin name
	2,                       // API major version number
	1,                       // API minor version number
	0,                       // Plugin major version number
	1,                       // Plugin minor version number
	FF_EFFECT,               // Plugin type
	"Interlace, pulldown and every way a deinterlacer gets it wrong.\n\nTreats the input as fields with real time between them, puts a film cadence in front, and shows the result on a progressive screen through the deinterlacer you choose. Combing, bob bounce, field-blend ghosting, motion-adaptive mistakes, 3:2 judder, a cadence break the inverse telecine mis-locks on, and the two-forward-one-back stutter of a swapped field order all fall out of the arithmetic rather than being drawn.\n\nStart with Split and Weave on something that moves.",// Plugin description
	"Cadence FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kSourceNames[]  = { "Split", "Telecine", "Cadence Break" };
const char* const kSourceRateNames[] = { "24", "25", "30", "60" };
const char* const kFieldRateNames[]  = { "50", "60" };
const char* const kFieldOrderNames[] = { "Top First", "Bottom First" };
const char* const kModeNames[]    = { "Weave", "Bob", "Bob Linear", "Blend", "Adaptive", "Inverse Telecine" };
const char* const kDisplayNames[] = { "Progressive", "Fields" };

constexpr int kSourceRateCount = 4;
constexpr int kFieldRateCount  = 2;
constexpr int kFieldOrderCount = 2;

/// Fields the telecine may emit in one host frame. The clock is clamped to
/// a 24th of a second per frame, which is two and a half fields at 60, so
/// this is never reached in practice; it is a bound, not a budget.
constexpr int kMaxFieldsPerFrame = 4;

/// The ring, by cadence. Split needs the current frame and the two behind
/// it; the telecine also holds a source frame across the fields that carry
/// it, and looks back two fields from there. See AGENTS.md for the sum.
constexpr int kRingSplit    = 4;
constexpr int kRingTelecine = 8;

/// A PCG output mix, exact in 32 bits. The phase an edit lands on has to be
/// random enough to look like an edit and repeatable enough for the harness
/// to reason about.
uint32_t hashInt( uint32_t seed )
{
	uint32_t state = seed * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

int optionValue( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}
} // namespace

Cadence::Cadence()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The field clock runs on host time, so an export matches the preview
	//and the telecine's 24 frames really are 24 a second. Without this the
	//host never fills the FFT buffer either, and the whole audio side sits
	//at zero while looking, from the inspector, exactly as though it were
	//working.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// Split and Weave: the honest pair. A 60 fps clip becomes 30i and is
	// shown as a weave, which is combing on anything that moves and nothing
	// at all on anything that does not.
	//---------------------------------------------------------------------
	params[ PT_SOURCE ]         = static_cast< float >( kSourceSplit );
	params[ PT_SOURCE_RATE ]    = 0.0f;//24
	params[ PT_FIELD_RATE ]     = 1.0f;//60
	params[ PT_FIELD_ORDER ]    = 0.0f;//top first
	params[ PT_SWAP ]           = 0.0f;
	params[ PT_BREAK_INTERVAL ] = 0.43f;//about two seconds
	params[ PT_BREAK_ONSET ]    = 0.0f;
	params[ PT_AUDIO ]          = 0.0f;

	params[ PT_MODE ]          = static_cast< float >( kModeWeave );
	params[ PT_THRESHOLD ]     = 0.15f;
	params[ PT_LOCK ]          = 0.5f;//about half a second
	params[ PT_SHOW_DECISION ] = 0.0f;

	params[ PT_DISPLAY ]     = static_cast< float >( kDisplayProgressive );
	params[ PT_LINE_FILTER ] = 0.0f;

	params[ PT_MIX ] = 1.0f;

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every ranged parameter is a plain 0..1 float. SetParamInfo clamps an
	// FF_TYPE_STANDARD default into 0..1 *before* a range can be attached
	// (SDK b1afaf9), so a parameter declared in seconds cannot declare a
	// default in seconds. The conversions live in Controls.cpp.
	//
	// Option lists are declared in their natural order and NOT sorted: every
	// one of them is a progression (Split -> Telecine -> Cadence Break, 24 ->
	// 60, Weave -> Inverse Telecine) or a pair, and there is nothing to look
	// up alphabetically in a list of three.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int paramID, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( paramID, name, static_cast< unsigned int >( count ), params[ paramID ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( paramID, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};

	declareOptions( PT_SOURCE, "Field Source", kSourceNames, kSourceCount );
	declareOptions( PT_SOURCE_RATE, "Source Rate", kSourceRateNames, kSourceRateCount );
	declareOptions( PT_FIELD_RATE, "Field Rate", kFieldRateNames, kFieldRateCount );
	declareOptions( PT_FIELD_ORDER, "Field Order", kFieldOrderNames, kFieldOrderCount );
	SetParamInfof( PT_SWAP, "Swap Field Order", FF_TYPE_BOOLEAN );
	SetParamInfof( PT_BREAK_INTERVAL, "Break Interval", FF_TYPE_STANDARD );
	SetParamInfof( PT_BREAK_ONSET, "Break On Onset", FF_TYPE_BOOLEAN );

	//The spectrum. Resolume fills a buffer declared FF_USAGE_FFT with 64
	//bins once per frame; nothing else in FFGL carries audio.
	SetBufferParamInfo( PT_AUDIO, "Audio", audio::kBins, FF_USAGE_FFT );
	for( int i = 0; i < audio::kBins; ++i )
		SetParamElementInfo( PT_AUDIO, static_cast< unsigned int >( i ), "", 0.0f );

	declareOptions( PT_MODE, "Mode", kModeNames, kModeCount );
	SetParamInfof( PT_THRESHOLD, "Adaptive Threshold", FF_TYPE_STANDARD );
	SetParamInfof( PT_LOCK, "Lock Time", FF_TYPE_STANDARD );
	SetParamInfof( PT_SHOW_DECISION, "Show Decision", FF_TYPE_BOOLEAN );

	declareOptions( PT_DISPLAY, "Display", kDisplayNames, kDisplayCount );
	SetParamInfof( PT_LINE_FILTER, "Line Filter", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each
	//group is a contiguous run of the enum.
	for( FFUInt32 i = PT_SOURCE; i <= PT_AUDIO; ++i )
		SetParamGroup( i, "Source" );
	for( FFUInt32 i = PT_MODE; i <= PT_SHOW_DECISION; ++i )
		SetParamGroup( i, "Deinterlace" );
	for( FFUInt32 i = PT_DISPLAY; i <= PT_LINE_FILTER; ++i )
		SetParamGroup( i, "Display" );
	SetParamGroup( PT_MIX, "Output" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	for( int i = 0; i < kMaxRing; ++i )
		slotSerial[ i ] = -1;

	FFGLLog::LogToHost( "Created Cadence effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Cadence::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not
	//compile it is almost always the driver or the GL version, and knowing
	//which machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &diffShader, kDiffShader, "diff" },
		{ &reduceShader, kReduceShader, "reduce" },
		{ &compositeShader, kCompositeShader, "composite" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Cadence: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Cadence: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	clearRing();
	frameSerial = 0;

	diag::info( "initialised, ring up to " + std::to_string( kMaxRing ) + " frames" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void Cadence::clearRing()
{
	for( int i = 0; i < kMaxRing; ++i )
	{
		slotSerial[ i ] = -1;
		slotTime[ i ]   = 0.0;
	}
	ringFilled        = 0;
	fieldCount        = 0;
	fieldsEmitted     = 0;
	nextTelecineField = -1;
	lastFieldA = lastFieldB = lastShown = -1;
	lastSerialA = lastSerialB = -1;
	itc.Reset();
}

bool Cadence::ensureRing( int width, int height, int frames )
{
	frames = std::clamp( frames, 2, kMaxRing );

	const bool same = width == ringWidth && height == ringHeight && frames == ringSize;

	for( int i = 0; i < kMaxRing; ++i )
	{
		if( i >= frames )
		{
			//Shrinking really does have to free them: four spare 4K frames
			//is a hundred and thirty megabytes the operator believes they
			//gave back when they switched to Split.
			slots[ i ].Destroy();
			continue;
		}

		if( !slots[ i ].Ensure( width, height, GL_RGBA8, PassBuffer::Sampling::Nearest ) )
			return false;
	}

	if( !same )
	{
		//The ring's indexing is modulo its own length, so changing the
		//length does not shuffle the contents, it reinterprets them: every
		//slot still holds a real frame, filed under the wrong serial. And a
		//resize leaves nothing worth keeping either way. Starting empty
		//costs a few frames and is the only answer that is not wrong.
		for( int i = 0; i < frames; ++i )
			slots[ i ].Clear();

		clearRing();

		ringWidth  = width;
		ringHeight = height;
		ringSize   = frames;

		diag::info( "ring rebuilt: " + std::to_string( frames ) + " x " + std::to_string( width ) + "x"
		            + std::to_string( height ) + " = "
		            + std::to_string( ( static_cast< long long >( frames ) * width * height * 4 ) / ( 1024 * 1024 ) )
		            + " MB" );
	}

	return true;
}

GLuint Cadence::textureOfSerial( long serial ) const
{
	if( ringSize <= 0 )
		return 0;

	const int slot = static_cast< int >( serial % ringSize );
	if( serial >= 0 && slotSerial[ slot ] == serial )
		return slots[ slot ].TextureID();

	//Overwritten, or never there. The newest frame is the least wrong
	//answer: a field a frame late is a stutter, a field of black is a hole.
	const long newest = frameSerial - 1;
	return newest >= 0 ? slots[ newest % ringSize ].TextureID() : 0;
}

long Cadence::serialAtOrBefore( double seconds ) const
{
	long best       = -1;
	double bestTime = -1.0;
	long oldest     = -1;
	double oldestTime = 0.0;

	for( int i = 0; i < ringSize; ++i )
	{
		if( slotSerial[ i ] < 0 )
			continue;

		//An epsilon of a hundredth of a field, so a frame captured AT the
		//source's moment counts as before it. The harness drives the clock
		//in exact sixtieths and the sums do not always land on the integer.
		if( slotTime[ i ] <= seconds + 1e-6 && ( best < 0 || slotTime[ i ] > bestTime ) )
		{
			best     = slotSerial[ i ];
			bestTime = slotTime[ i ];
		}
		if( oldest < 0 || slotTime[ i ] < oldestTime )
		{
			oldest     = slotSerial[ i ];
			oldestTime = slotTime[ i ];
		}
	}

	return best >= 0 ? best : oldest;
}

const Cadence::Field* Cadence::fieldAt( long index ) const
{
	for( int i = fieldCount - 1; i >= 0; --i )
		if( fields[ i ].index == index )
			return &fields[ i ];
	return nullptr;
}

void Cadence::pushField( long index, int parity, long sourceSerial )
{
	if( fieldCount == kMaxFields )
	{
		for( int i = 1; i < kMaxFields; ++i )
			fields[ i - 1 ] = fields[ i ];
		--fieldCount;
	}
	fields[ fieldCount++ ] = Field { index, parity, sourceSerial };
}

void Cadence::cadenceBreak( double now, int cycle )
{
	//A different phase, never the same one: an edit that happened to land
	//on the cycle it left is not an edit anybody would notice.
	if( cycle > 1 )
	{
		const uint32_t h = hashInt( static_cast< uint32_t >( breakCount ) * 2654435761u ^ 0x9E3779B9u );
		cadencePhase     = ( cadencePhase + 1 + static_cast< int >( h % static_cast< uint32_t >( cycle - 1 ) ) ) % cycle;
	}
	++breakCount;
	lastBreakTime = now;
}

//---------------------------------------------------------------------------
float Cadence::sameParityDifference( const Field& current, const Field& twoBack )
{
	{
		ScopedFBOBinding fbo( diffGrid.GetGLID(), ScopedFBOBinding::RB_REVERT );
		diffGrid.ResizeViewPort();
		ScopedShaderBinding shader( diffShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding a( textureOfSerial( current.sourceSerial ) );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding b( textureOfSerial( twoBack.sourceSerial ) );

		diffShader.Set( "FieldA", 0 );
		diffShader.Set( "FieldB", 1 );
		diffShader.Set( "Parity", current.parity );
		diffShader.Set( "Width", ringWidth );
		diffShader.Set( "Height", ringHeight );
		diffShader.Set( "GridWidth", kDiffGridWidth );
		diffShader.Set( "GridHeight", kDiffGridHeight );
		quad.Draw();
	}

	float sums[ kReduceWidth * 4 ] = {};
	{
		ScopedFBOBinding fbo( reduceRow.GetGLID(), ScopedFBOBinding::RB_REVERT );
		reduceRow.ResizeViewPort();
		ScopedShaderBinding shader( reduceShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding grid( diffGrid.TextureID() );

		reduceShader.Set( "Grid", 0 );
		reduceShader.Set( "GridHeight", kDiffGridHeight );
		reduceShader.Set( "Columns", kDiffGridWidth / kReduceWidth );
		quad.Draw();

		//A synchronous read of sixty-four bytes. It stalls until the diff
		//and reduce passes have run, which is early in the frame; the cost
		//is on the record in the README's bench table, Inverse Telecine
		//against Weave.
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, kReduceWidth, 1, GL_RGBA, GL_FLOAT, sums );
	}

	float total = 0.0f;
	for( int i = 0; i < kReduceWidth; ++i )
		total += sums[ i * 4 ];

	return total / static_cast< float >( kDiffGridWidth * kDiffGridHeight );
}

//---------------------------------------------------------------------------
FFResult Cadence::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it.
	//`ScopedFBOBinding` restores the framebuffer binding and only that (SDK
	//b1afaf9): every pass's ResizeViewPort() leaks into the pass after it,
	//and the composite, which draws to the host's own framebuffer, would
	//inherit the 16x1 reduce viewport and paint sixteen pixels.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// Time. The clock measures the host's unit rather than assuming it:
	// Resolume sends milliseconds, the harness sends seconds, and the FFGL
	// header says nothing. Now() is monotonic and clamped, so a scrub or a
	// stall moves the field clock by at most a 24th of a second.
	//---------------------------------------------------------------------
	clock.Update( hostTime );
	const double now = clock.Now();
	const float dt   = clock.FrameSeconds();

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime )
		            + " scale=" + std::to_string( clock.ClockScale() )
		            + " seconds=" + std::to_string( now ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int source   = optionValue( params[ PT_SOURCE ], kSourceCount );
	const int mode     = optionValue( params[ PT_MODE ], kModeCount );
	const int display  = optionValue( params[ PT_DISPLAY ], kDisplayCount );
	const int sourceRate = SourceRateFromOption( params[ PT_SOURCE_RATE ] );
	const int fieldRate  = FieldRateFromOption( params[ PT_FIELD_RATE ] );
	const bool bottomFirst = params[ PT_FIELD_ORDER ] >= 0.5f;
	const bool swap        = params[ PT_SWAP ] >= 0.5f;
	const bool onsetBreaks = params[ PT_BREAK_ONSET ] >= 0.5f;
	const float lockSeconds   = LockSecondsFromParam( params[ PT_LOCK ] );
	const float breakInterval = BreakIntervalFromParam( params[ PT_BREAK_INTERVAL ] );

	//---------------------------------------------------------------------
	// Audio, before anything decides a field: an onset this frame is an
	// edit this frame, not next.
	//---------------------------------------------------------------------
	if( clock.Jumped() )
		analyser.Reset();
	{
		float bins[ audio::kBins ] = {};
		int binCount               = 0;
		if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
		{
			binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
			for( int i = 0; i < binCount; ++i )
				bins[ i ] = info->elements[ i ].value;
		}
		//Fixed analysis settings: the only thing this plugin wants from the
		//spectrum is a yes or no per frame, and four sliders to tune a
		//detector whose output is "an edit happened" would be four sliders
		//an operator cannot hear the difference between.
		audio::Settings settings;
		analyser.Update( bins, binCount, dt, settings );
	}

	//---------------------------------------------------------------------
	// Buffers. Every Ensure() happens here, before anything binds a texture:
	// FFGLFBO::Initialise sizes its new colour texture under a scoped
	// binding, and every ffglex Scoped* binding CLEARS to 0 on scope exit
	// rather than restoring. Allocating a buffer mid-chain silently unbinds
	// the input texture, correctly on every frame except the one that
	// allocates.
	//---------------------------------------------------------------------
	const int ringWanted = source == kSourceSplit ? kRingSplit : kRingTelecine;

	if( !ensureRing( pictureWidth, pictureHeight, ringWanted )
	    || !diffGrid.Ensure( kDiffGridWidth, kDiffGridHeight, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	    || !reduceRow.Ensure( kReduceWidth, 1, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the frame ring: " + std::to_string( ringWanted ) + " x "
		             + std::to_string( pictureWidth ) + "x" + std::to_string( pictureHeight ) );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 1. Capture: the picture into the ring slot its serial lands on.
	//---------------------------------------------------------------------
	const long serial = frameSerial++;
	const int slot    = static_cast< int >( serial % ringSize );
	{
		ScopedFBOBinding fbo( slots[ slot ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		slots[ slot ].ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		quad.Draw();
	}
	slotSerial[ slot ] = serial;
	slotTime[ slot ]   = now;
	ringFilled         = std::min( ringFilled + 1, ringSize );

	//---------------------------------------------------------------------
	// 2. Fields. Which frames become fields, and when.
	//---------------------------------------------------------------------
	const bool wantItc = mode == kModeInverseTelecine;

	auto emit = [ & ]( long k, long sourceSerial, float fieldSeconds ) {
		const int parity = pulldown::FieldParity( k, bottomFirst );

		//The inverse telecine's number, measured before the field joins
		//the list so that "two back" means what it says.
		if( wantItc )
		{
			if( const Field* twoBack = fieldAt( k - 2 ) )
			{
				const Field current { k, parity, sourceSerial };
				itc.Observe( k, sameParityDifference( current, *twoBack ), lockSeconds, fieldSeconds );
			}
		}

		pushField( k, parity, sourceSerial );
		++fieldsEmitted;
	};

	if( source == kSourceSplit )
	{
		//One field per input frame, alternating parity, with a real frame
		//period between them. The field clock IS the host frame clock here;
		//Field Rate and Source Rate have nothing to say.
		emit( fieldsEmitted, serial, dt );
	}
	else
	{
		const int cycle = pulldown::CycleFields( sourceRate, fieldRate );

		if( source == kSourceBreak )
		{
			if( lastBreakTime < 0.0 )
				lastBreakTime = now;
			if( now - lastBreakTime >= breakInterval )
				cadenceBreak( now, cycle );
			if( onsetBreaks && analyser.Fired( audio::Band::Full ) )
				cadenceBreak( now, cycle );
		}

		//Field k is due at k / Rf. An epsilon of a ten-thousandth of a
		//field, because the harness's clock is a sum of sixtieths and the
		//product does not always land on the integer it should.
		const long due = static_cast< long >( std::floor( now * fieldRate + 1e-4 ) );
		if( nextTelecineField < 0 )
			nextTelecineField = due;

		//A stall longer than the bound is skipped, not caught up: four
		//fields emitted in one frame is a stutter, forty is a freeze.
		if( due - nextTelecineField > kMaxFieldsPerFrame )
			nextTelecineField = due - kMaxFieldsPerFrame;

		while( nextTelecineField <= due )
		{
			const long k = nextTelecineField++;
			const long n = pulldown::SourceFrameOfField( k, cadencePhase, sourceRate, fieldRate );
			const double shot = pulldown::SourceFrameTime( n, cadencePhase, sourceRate, fieldRate );
			emit( k, serialAtOrBefore( shot ), 1.0f / static_cast< float >( fieldRate ) );
		}
	}

	//---------------------------------------------------------------------
	// 3. Presentation. Which two fields the deinterlacer sees, and which
	//    one it is showing.
	//
	// The deinterlacer is frame-based: it consumes fields in pairs (2j,
	// 2j+1), the way an interlaced stream hands over frames, and it only
	// starts on a pair once both fields are in. So at field K the pair in
	// hand is ( K-1, K ) when K is odd and ( K-2, K-1 ) when K is even --
	// one field of latency, which is what a real one has.
	//
	// Field-rate modes show the pair's two fields one after the other, in
	// the order the deinterlacer BELIEVES they were shot. Swap Field Order
	// is that belief being wrong: it shows the later field first, and a
	// steadily moving object goes 1, 0, 3, 2, 5, 4.
	//---------------------------------------------------------------------
	if( fieldCount == 0 )
		return FF_FAIL;

	const long K         = fields[ fieldCount - 1 ].index;
	const long pairFirst = K >= 1 ? ( ( K - 1 ) & ~1L ) : K;
	const long pairSecond = K >= 1 ? pairFirst + 1 : K;
	const bool firstHalf = ( K & 1 ) != 0 || K == 0;
	const long shown     = firstHalf ? ( swap ? pairSecond : pairFirst )
	                                 : ( swap ? pairFirst : pairSecond );

	long fieldA = pairFirst;
	long fieldB = pairSecond;

	switch( mode )
	{
	case kModeWeave:
	case kModeBlend:
		break;

	case kModeInverseTelecine:
		if( K >= 2 )
		{
			fieldA = itc.WeaveLatest( K ) ? K - 1 : K - 2;
			fieldB = fieldA + 1;
		}
		break;

	case kModeBob:
	case kModeBobLinear:
		fieldA = shown;
		fieldB = shown;
		break;

	case kModeAdaptive:
	default:
		fieldA = shown;
		fieldB = pairFirst + pairSecond - shown;
		break;
	}

	if( display == kDisplayFields )
		fieldA = shown;

	//A field that has already left the list -- start-up, or the first frame
	//after a rebuild -- falls back to the newest, which is the least wrong
	//picture available.
	const Field* a = fieldAt( fieldA );
	const Field* b = fieldAt( fieldB );
	if( a == nullptr )
		a = &fields[ fieldCount - 1 ];
	if( b == nullptr )
		b = &fields[ fieldCount - 1 ];

	lastFieldA  = a->index;
	lastFieldB  = b->index;
	lastShown   = shown;
	lastSerialA = a->sourceSerial;
	lastSerialB = b->sourceSerial;

	//---------------------------------------------------------------------
	// 4. Composite, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		//Back to the host's viewport. See the note where it was captured.
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding input( picture.Handle );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding textureA( textureOfSerial( a->sourceSerial ) );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding textureB( textureOfSerial( b->sourceSerial ) );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		compositeShader.Set( "InputTexture", 0 );
		compositeShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		compositeShader.Set( "FieldA", 1 );
		compositeShader.Set( "FieldB", 2 );
		compositeShader.Set( "ParityA", a->parity );
		compositeShader.Set( "ParityB", b->parity );
		compositeShader.Set( "Width", ringWidth );
		compositeShader.Set( "Height", ringHeight );
		compositeShader.Set( "Mode", mode );
		compositeShader.Set( "Display", display );
		compositeShader.Set( "Threshold", AdaptiveThresholdFromParam( params[ PT_THRESHOLD ] ) );
		compositeShader.Set( "ShowDecision", params[ PT_SHOW_DECISION ] >= 0.5f ? 1 : 0 );
		compositeShader.Set( "LineFilter", LineFilterFromParam( params[ PT_LINE_FILTER ] ) );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Cadence::DeInitGL()
{
	copyShader.FreeGLResources();
	diffShader.FreeGLResources();
	reduceShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	for( int i = 0; i < kMaxRing; ++i )
		slots[ i ].Destroy();
	diffGrid.Destroy();
	reduceRow.Destroy();

	ringWidth  = 0;
	ringHeight = 0;
	ringSize   = 0;
	clearRing();

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Cadence::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing, so they are handled
	// before the params[] write below -- there is no value to keep.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Cadence::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Cadence::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Cadence::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Cadence::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}
