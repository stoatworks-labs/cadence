/**
	cdtest -- render Cadence offline, and check what its fields are doing.

	Where a field's lines are and which frame they came from is a fact, not
	a matter of taste. This harness drives the REAL plugin class in a
	headless CGL context on a synthetic 60 fps clock, feeds it frames whose
	content is known exactly, and reads the picture back to see whether the
	arithmetic in `Pulldown.h` and the composite shader did what they claim.

		cdtest --out /tmp/frame.png     a picture, on a moving test card
		cdtest --list                   every parameter, its kind and default
		cdtest --comb                   weave: a moving bar combs by exactly v
		cdtest --bob                    bob: a two-line detail bounces a line
		cdtest --pattern                telecine: 2:3 is A A B B B C C D D D
		cdtest --adaptive               adaptive at 0 is Bob Linear, at 1 is Weave
		cdtest --swap                   swapped field order: 1 0 3 2 5 4
		cdtest --ring                   a resize mid-run clears the ring
		cdtest --bench                  the render cost, 720p through 4K
		cdtest --pipe                   raw frames in, raw frames out

	Every check has one flag, every flag has one claim, and every claim is
	stated in the README's Status table with the number this printed.

	**The test card MOVES.** Everything this plugin does is the difference
	between one field and the next; on a still card every mode collapses to
	the input and `tools/sweep.py` would report most controls dead.

	`--pipe` takes the fleet's frame format, so one filming script can drive
	any of these plugins:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | cdtest --pipe --width 1920 --height 1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov

	`--script` is a plain text file of `frame  Parameter Name  value` lines,
	the same format as tinseltest, old-cathode's octest and porthole's
	phtest. Values are held before the first key and after the last, and
	linearly interpolated between.

	Note what interpolation means for an **option** parameter -- Field
	Source, Source Rate, Field Rate, Field Order, Mode, Display. Moving one
	produces the intermediate values on the way, so a change from Weave to
	Adaptive passes through Bob, Bob Linear and Blend. Key them one frame
	apart to cut, and give every such parameter a hold key at the END of
	each section it must not move in.
*/

#include "Cadence.h"
#include "Controls.h"
#include "Pulldown.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace cadence;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures. All of them top row first, like a file; the session flips on
// the way in and out of GL.
//---------------------------------------------------------------------------
using Image = std::vector< unsigned char >;

Image solid( int width, int height, unsigned char grey )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		image[ i ]     = grey;
		image[ i + 1 ] = grey;
		image[ i + 2 ] = grey;
		image[ i + 3 ] = 255;
	}
	return image;
}

void paint( Image& image, int width, int height, int x0, int y0, int x1, int y1, unsigned char r, unsigned char g, unsigned char b )
{
	for( int y = std::max( 0, y0 ); y < std::min( height, y1 ); ++y )
		for( int x = std::max( 0, x0 ); x < std::min( width, x1 ); ++x )
		{
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			image[ i ]     = r;
			image[ i + 1 ] = g;
			image[ i + 2 ] = b;
			image[ i + 3 ] = 255;
		}
}

/// The test card. Not meant to look nice: each part exercises one claim.
///
///   - a vertical bar sweeping right at a whole number of pixels a frame:
///     combing, bob, swap -- anything about WHEN a field was shot
///   - a bright disc on a Lissajous path: motion that is not a straight
///     edge, for the adaptive decision to have something to be wrong about
///   - a one-line and a two-line static horizontal rule: twitter and bounce
///   - a fine horizontal-line grating: what Blend and Line Filter soften
///   - six colour bars and a ramp: so the picture is a picture
Image buildCard( int width, int height, int frame )
{
	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	Image card( static_cast< size_t >( width ) * height * 4 );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			float r = 0.05f + 0.05f * v;
			float g = 0.05f + 0.05f * v;
			float b = 0.08f + 0.06f * v;

			//Six saturated bars across the bottom eighth.
			if( v > 0.875f )
			{
				static const float bars[ 6 ][ 3 ] = {
					{ 1.0f, 0.1f, 0.1f }, { 0.1f, 1.0f, 0.1f }, { 0.1f, 0.1f, 1.0f },
					{ 0.1f, 1.0f, 1.0f }, { 1.0f, 0.1f, 1.0f }, { 1.0f, 1.0f, 0.1f }
				};
				const int bar = std::min( 5, static_cast< int >( u * 6.0f ) );
				r             = bars[ bar ][ 0 ];
				g             = bars[ bar ][ 1 ];
				b             = bars[ bar ][ 2 ];
			}
			//A ramp above them.
			else if( v > 0.75f )
			{
				r = g = b = u;
			}
			//A one-pixel horizontal grating in the top-left: alternate rows.
			else if( u < 0.25f && v < 0.25f )
			{
				r = g = b = ( y & 1 ) ? 0.1f : 0.8f;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i ]      = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 1 ]  = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 2 ]  = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 3 ]  = 255;
		}
	}

	//The sweeping bar: three pixels a frame, wrapping. Whole pixels, so a
	//comb offset is a whole number and a check can ask for exactly it.
	const int barW = std::max( 4, width / 40 );
	const int barX = ( frame * 3 ) % ( width + barW ) - barW;
	paint( card, width, height, barX, static_cast< int >( h * 0.30f ), barX + barW, static_cast< int >( h * 0.72f ), 140, 190, 242 );

	//The static rules, on even rows so they sit in the top field.
	const int ruleY1 = ( static_cast< int >( h * 0.30f ) ) & ~1;
	const int ruleY2 = ( static_cast< int >( h * 0.36f ) ) & ~1;
	paint( card, width, height, static_cast< int >( w * 0.30f ), ruleY1, static_cast< int >( w * 0.95f ), ruleY1 + 1, 255, 255, 255 );
	paint( card, width, height, static_cast< int >( w * 0.30f ), ruleY2, static_cast< int >( w * 0.95f ), ruleY2 + 2, 255, 255, 255 );

	//The disc.
	const float discX = 0.5f + 0.34f * std::sin( t * 0.110f );
	const float discY = 0.5f + 0.20f * std::sin( t * 0.077f + 1.1f );
	const float discR = 0.06f * w;
	const int cx = static_cast< int >( discX * w ), cy = static_cast< int >( discY * h );
	for( int y = std::max( 0, cy - static_cast< int >( discR ) - 1 ); y < std::min( height, cy + static_cast< int >( discR ) + 2 ); ++y )
		for( int x = std::max( 0, cx - static_cast< int >( discR ) - 1 ); x < std::min( width, cx + static_cast< int >( discR ) + 2 ); ++x )
		{
			const float dx = static_cast< float >( x - cx ), dy = static_cast< float >( y - cy );
			if( dx * dx + dy * dy < discR * discR )
			{
				const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
				card[ i ] = 250;
				card[ i + 1 ] = 235;
				card[ i + 2 ] = 200;
			}
		}

	return card;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere
	//without a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

Image flipRows( const Image& image, int width, int height )
{
	Image flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BOOLEAN: return "boolean";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_EVENT: return "event";
	default: return "other";
	}
}

int findParameter( Cadence& plugin, const std::string& name )
{
	for( unsigned int i = 0; i < Cadence::PT_COUNT; ++i )
	{
		const char* const declared = plugin.GetParamName( i );
		if( declared != nullptr && name == declared )
			return static_cast< int >( i );
	}
	return -1;
}

bool applySetting( Cadence& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	const int index = findParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "' (try --list)";
		return false;
	}

	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( value.c_str(), nullptr ) );
	return true;
}

void listParameters( Cadence& plugin )
{
	std::printf( "%3s  %-20s  %-8s %8s  range\n", "id", "name", "kind", "default" );
	for( unsigned int i = 0; i < Cadence::PT_COUNT; ++i )
	{
		const char* const name  = plugin.GetParamName( i );
		const unsigned int type = plugin.GetParamType( i );
		const float value       = plugin.GetFloatParameter( i );

		if( i >= Cadence::PT_ABOUT_FIRST )
		{
			std::printf( "%3u  %-20s  %-8s %8s  -\n", i, name ? name : "?", "about", "-" );
			continue;
		}

		float high = 1.0f;
		if( type == FF_TYPE_OPTION )
			high = static_cast< float >( plugin.GetNumParamElements( i ) - 1 );

		if( type == FF_TYPE_BUFFER )
			std::printf( "%3u  %-20s  %-8s %8s  -\n", i, name ? name : "?", kindName( type ), "-" );
		else
			std::printf( "%3u  %-20s  %-8s %8.4f  [ %g .. %g ]\n", i, name ? name : "?", kindName( type ), value, 0.0f, high );
	}
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one `frame Parameter Name value` per line, applied when
// the frame number is reached and linearly interpolated between keys. The same
// format tinseltest, octest and phtest read, so one filming script drives any
// of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them ("Adaptive Threshold") and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

/// A click train through the same call the host uses. Every sixth frame a
/// hit in the low band, alternating hard and soft; a sawtooth in the top
/// band so the detector has continuous flux to be decisive against.
void injectSpectrum( Cadence& plugin, int frame )
{
	const bool click = ( frame % 6 ) == 0 && frame > 0;
	const bool hard  = ( frame % 12 ) == 0;
	const float ramp = static_cast< float >( frame % 20 ) / 20.0f;

	for( int i = 0; i < audio::kBins; ++i )
	{
		float value = 0.0f;
		if( i < 8 )
			value = click ? ( hard ? 0.85f : 0.30f ) : 0.05f;
		else if( i < 28 )
			value = 0.04f;
		else
			value = 0.03f + 0.30f * ramp;
		plugin.SetParamElementValue( Cadence::PT_AUDIO, static_cast< unsigned int >( i ), value );
	}
}

//---------------------------------------------------------------------------
// A session: one plugin, one picture size, frames in and pictures out.
//---------------------------------------------------------------------------
class Session
{
public:
	Session( int width, int height, double fps )
		: fps( fps )
	{
		resize( width, height );
	}

	~Session()
	{
		release();
		if( initialised )
			plugin.DeInitGL();
	}

	Cadence& Plugin()
	{
		return plugin;
	}

	bool set( const std::string& assignment )
	{
		std::string error;
		if( applySetting( plugin, assignment, error ) )
			return true;
		std::fprintf( stderr, "--set %s: %s\n", assignment.c_str(), error.c_str() );
		return false;
	}

	/// Change the picture size mid-run, keeping the plugin instance: this is
	/// how --ring provokes a rebuild.
	void resize( int newWidth, int newHeight )
	{
		release();
		width  = newWidth;
		height = newHeight;
		sourceTexture = makeTexture( width, height, nullptr );
		outputTexture = makeTexture( width, height, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );
	}

	bool init()
	{
		FFGLViewportStruct viewport = {};
		viewport.width  = static_cast< FFUInt32 >( width );
		viewport.height = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		initialised = true;
		return true;
	}

	/// One frame in, one picture out, both top row first. `frame` drives
	/// the synthetic clock.
	bool render( const Image& source, int frame, Image* out )
	{
		//A synthetic clock, and it has to be synthetic. Left to the wall
		//clock the harness renders a hundred frames in a few milliseconds,
		//so no time passes and the telecine never emits a second field. The
		//unit is DECLARED rather than measured, because an absolute time
		//handed over in a single frame is genuinely ambiguous.
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );

		if( tone )
			injectSpectrum( plugin, frame );

		const Image flipped = flipRows( source, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		FFGLTextureStruct inputStruct = {};
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

		ProcessOpenGLStruct process = {};
		process.numInputTextures    = 1;
		process.inputTextures       = inputs;
		process.HostFBO             = outputFBO;

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			return false;

		if( out != nullptr )
		{
			Image raw( static_cast< size_t >( width ) * height * 4 );
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glPixelStorei( GL_PACK_ALIGNMENT, 1 );
			glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data() );
			*out = flipRows( raw, width, height );
		}
		return true;
	}

	int width  = 0;
	int height = 0;
	double fps = 60.0;
	bool tone  = false;

private:
	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	Cadence plugin;
	bool initialised     = false;
	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
};

//---------------------------------------------------------------------------
// Reading pictures.
//---------------------------------------------------------------------------
unsigned char grey( const Image& image, int width, int x, int y )
{
	return image[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
}

/// The first column on row `y` at or above `level`, or -1.
int leftEdge( const Image& image, int width, int y, unsigned char level )
{
	for( int x = 0; x < width; ++x )
		if( grey( image, width, x, y ) >= level )
			return x;
	return -1;
}

/// Frames in a run that always has enough history: the deinterlacer is a
/// field late and the pair it looks at is two fields deep.
constexpr int kSettle = 3;

//---------------------------------------------------------------------------
// --comb
//
// Weave on Split: a bar moving v pixels a frame lands v pixels apart on
// the two fields of a pair, and which field is the newer one is decided by
// the pair rule in Cadence.cpp. Asserted for every row and three speeds.
//---------------------------------------------------------------------------
int runComb( int width, int height )
{
	int failures = 0;
	const int speeds[] = { 1, 3, 7 };

	for( int v : speeds )
	{
		Session session( width, height, 60.0 );
		if( !session.set( "Field Source=0" ) || !session.set( "Mode=0" ) || !session.set( "Display=0" ) || !session.init() )
			return 1;

		const int barW  = 8;
		auto barAt = [ & ]( int frame ) { return 10 + v * frame; };

		int checked = 0;
		int worst   = 0;
		Image out;
		for( int frame = 0; frame < 14; ++frame )
		{
			Image source = solid( width, height, 0 );
			paint( source, width, height, barAt( frame ), 0, barAt( frame ) + barW, height, 255, 255, 255 );
			if( !session.render( source, frame, &out ) )
			{
				std::fprintf( stderr, "comb: ProcessOpenGL failed\n" );
				return 1;
			}
			if( frame < kSettle )
				continue;

			//The pair rule: K odd -> ( K-1, K ), K even -> ( K-2, K-1 ). Odd
			//rows belong to the odd (bottom) field under Top First, so odd
			//rows show the newer field's bar and even rows the older one's.
			const int newer = ( frame & 1 ) ? frame : frame - 1;
			const int older = newer - 1;

			for( int y = 0; y < height; ++y )
			{
				const int expect = ( y & 1 ) ? barAt( newer ) : barAt( older );
				const int got    = leftEdge( out, width, y, 128 );
				++checked;
				const int d = std::abs( got - expect );
				worst       = std::max( worst, d );
				if( d != 0 )
				{
					++failures;
					if( failures <= 8 )
						std::printf( "  v=%d frame %2d row %3d: bar at %d, expected %d\n", v, frame, y, got, expect );
				}
			}

			//And the comb itself, as the spec states it: the even and odd
			//rows are offset by exactly v.
			const int offset = leftEdge( out, width, 1, 128 ) - leftEdge( out, width, 0, 128 );
			if( offset != v )
			{
				++failures;
				std::printf( "  v=%d frame %2d: comb offset %d, expected %d\n", v, frame, offset, v );
			}
		}
		std::printf( "comb v=%d: %d rows checked, largest error %d px\n", v, checked, worst );
	}

	std::printf( "%s\n", failures == 0 ? "comb: a moving bar combs by exactly v on every row" : "comb: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bob
//
// Bob on Split, static rules. A one-line rule lives in one field only, so
// it is there on alternate frames and doubled to two lines when it is. A
// two-line rule has a line in each field, and line doubling puts its top
// edge one row lower on one field than on the other: the bounce.
//---------------------------------------------------------------------------
int runBob( int width, int height )
{
	Session session( width, height, 60.0 );
	if( !session.set( "Field Source=0" ) || !session.set( "Mode=1" ) || !session.init() )
		return 1;

	const int y1 = 40;//one line, even: the top field
	const int y2 = 80;//two lines, even and odd
	const int x  = width / 2;

	Image source = solid( width, height, 0 );
	paint( source, width, height, 0, y1, width, y1 + 1, 255, 255, 255 );
	paint( source, width, height, 0, y2, width, y2 + 2, 255, 255, 255 );

	int failures = 0;
	int checked  = 0;
	Image out;
	for( int frame = 0; frame < 14; ++frame )
	{
		if( !session.render( source, frame, &out ) )
		{
			std::fprintf( stderr, "bob: ProcessOpenGL failed\n" );
			return 1;
		}
		if( frame < kSettle )
			continue;

		//Shown field is K-1 (no swap), whose parity under Top First is
		//( K-1 ) & 1.
		const int parity = ( frame - 1 ) & 1;

		std::vector< int > lit;
		for( int y = 0; y < height; ++y )
			if( grey( out, width, x, y ) > 128 )
				lit.push_back( y );

		std::vector< int > expect;
		if( parity == 0 )
			expect = { y1, y1 + 1, y2, y2 + 1 };
		else
			expect = { y2 + 1, y2 + 2 };

		++checked;
		if( lit != expect )
		{
			++failures;
			std::printf( "  frame %2d (field parity %d): lit rows", frame, parity );
			for( int y : lit )
				std::printf( " %d", y );
			std::printf( ", expected" );
			for( int y : expect )
				std::printf( " %d", y );
			std::printf( "\n" );
		}
	}

	std::printf( "bob: %d frames checked; the two-line rule's top edge alternates %d, %d and the one-line rule is present on alternate frames only\n",
	             checked, y2, y2 + 1 );
	std::printf( "%s\n", failures == 0 ? "bob: a two-line detail bounces one line per field" : "bob: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --pattern
//
// Telecine, Fields display, every input frame a distinct grey. Reading the
// lit row's grey back says which input frame each field carries, and the
// run lengths of the sequence ARE the pulldown.
//---------------------------------------------------------------------------
struct PatternCase
{
	const char* name;
	const char* sourceRate;///< option value
	const char* fieldRate; ///< option value
	std::vector< int > runs;///< expected run lengths, repeating
};

int runPattern( int width, int height )
{
	const PatternCase cases[] = {
		{ "24 -> 60 (2:3)", "0", "1", { 2, 3 } },
		{ "30 -> 60 (2:2)", "2", "1", { 2, 2 } },
		{ "25 -> 50 (2:2)", "1", "0", { 2, 2 } },
	};

	int failures = 0;

	for( const PatternCase& c : cases )
	{
		Session session( width, height, 60.0 );
		if( !session.set( "Field Source=1" ) || !session.set( std::string( "Source Rate=" ) + c.sourceRate )
		    || !session.set( std::string( "Field Rate=" ) + c.fieldRate ) || !session.set( "Display=1" ) || !session.init() )
			return 1;

		//One grey per input frame, 1..N: 0 is what the other field's rows
		//show, so it cannot be a frame.
		std::vector< int > sourceOfField;//distinct fields, in order
		std::vector< int > parityOfField;
		int lastParity = -1;

		Image out;
		const int frames = 60;
		for( int frame = 0; frame < frames; ++frame )
		{
			if( !session.render( solid( width, height, static_cast< unsigned char >( frame + 1 ) ), frame, &out ) )
			{
				std::fprintf( stderr, "pattern: ProcessOpenGL failed\n" );
				return 1;
			}
			if( frame < 12 )
				continue;//let the first cycle and its start-up fallback go by

			const int top    = grey( out, width, width / 2, 0 );
			const int bottom = grey( out, width, width / 2, 1 );
			const int parity = top != 0 ? 0 : 1;
			const int level  = top != 0 ? top : bottom;

			if( ( top != 0 ) == ( bottom != 0 ) )
			{
				++failures;
				std::printf( "  %s frame %d: rows 0 and 1 read %d and %d; exactly one should be lit\n", c.name, frame, top, bottom );
				continue;
			}

			//At 50 fields on a 60 fps host a field shows for two frames;
			//consecutive fields always alternate parity, so a repeated
			//parity is the same field again, not a new one.
			if( parity == lastParity )
				continue;
			lastParity = parity;
			sourceOfField.push_back( level - 1 );
			parityOfField.push_back( parity );
		}

		//Runs of the same source frame.
		std::vector< int > runs;
		std::vector< int > runSources;
		for( size_t i = 0; i < sourceOfField.size(); ++i )
		{
			if( i == 0 || sourceOfField[ i ] != sourceOfField[ i - 1 ] )
			{
				runs.push_back( 1 );
				runSources.push_back( sourceOfField[ i ] );
			}
			else
				++runs.back();
		}

		//Drop the partial run at each end, then the middle must repeat the
		//pattern from SOME phase of it.
		if( runs.size() < 6 )
		{
			++failures;
			std::printf( "  %s: only %zu runs seen\n", c.name, runs.size() );
			continue;
		}
		const std::vector< int > middle( runs.begin() + 1, runs.end() - 1 );

		bool matched = false;
		for( size_t phase = 0; phase < c.runs.size() && !matched; ++phase )
		{
			matched = true;
			for( size_t i = 0; i < middle.size(); ++i )
				if( middle[ i ] != c.runs[ ( phase + i ) % c.runs.size() ] )
				{
					matched = false;
					break;
				}
		}

		//The source frames must advance -- a run is a NEW input frame, not
		//the same one again -- and the sampled input frames must advance at
		//the source rate: 2.5 host frames per source frame for 24 -> 60.
		bool advancing = true;
		for( size_t i = 1; i < runSources.size(); ++i )
			if( runSources[ i ] <= runSources[ i - 1 ] )
				advancing = false;

		const int sourceRate = SourceRateFromOption( std::strtof( c.sourceRate, nullptr ) );
		const double wantStep = 60.0 / sourceRate;
		const double gotStep  = static_cast< double >( runSources.back() - runSources.front() ) / static_cast< double >( runSources.size() - 1 );

		std::string seq;
		for( size_t i = 0; i < std::min< size_t >( 10, sourceOfField.size() ); ++i )
			seq += std::string( 1, static_cast< char >( 'A' + ( sourceOfField[ i ] - sourceOfField[ 0 ] ) / 2 % 26 ) ) + " ";

		std::printf( "pattern %s: %zu fields, runs", c.name, sourceOfField.size() );
		for( int r : middle )
			std::printf( " %d", r );
		std::printf( "; input frames advance %.2f host frames per source frame (want %.2f)\n", gotStep, wantStep );

		if( !matched || !advancing || std::fabs( gotStep - wantStep ) > 0.15 )
		{
			++failures;
			std::printf( "  %s: FAILED (%s%s%s)\n", c.name, matched ? "" : "run lengths wrong ",
			             advancing ? "" : "source frames not advancing ", std::fabs( gotStep - wantStep ) > 0.15 ? "wrong source rate" : "" );
		}
	}

	std::printf( "%s\n", failures == 0 ? "pattern: 2:3 emits A A B B B C C D D D, and 2:2 emits pairs" : "pattern: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --adaptive
//
// Adaptive Threshold at 0 is Bob Linear and at 1 is Weave, byte for byte,
// on the moving card, over a run of frames. Both extremes go through the
// same deinterlace() with the same arithmetic, which is what makes
// "pixel-exact" a claim rather than a hope.
//---------------------------------------------------------------------------
int runAdaptive( int width, int height )
{
	struct Case
	{
		const char* name;
		std::vector< const char* > a;
		std::vector< const char* > b;
	};
	const Case cases[] = {
		{ "threshold 0 == Bob Linear", { "Mode=4", "Adaptive Threshold=0" }, { "Mode=2" } },
		{ "threshold 1 == Weave", { "Mode=4", "Adaptive Threshold=1" }, { "Mode=0" } },
	};

	auto renderRun = [ & ]( const std::vector< const char* >& settings, std::vector< Image >& frames ) {
		Session session( width, height, 60.0 );
		for( const char* s : settings )
			if( !session.set( s ) )
				return false;
		if( !session.set( "Field Source=0" ) || !session.init() )
			return false;
		for( int frame = 0; frame < 16; ++frame )
		{
			Image out;
			if( !session.render( buildCard( width, height, frame ), frame, &out ) )
				return false;
			frames.push_back( out );
		}
		return true;
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		std::vector< Image > a, b;
		if( !renderRun( c.a, a ) || !renderRun( c.b, b ) )
		{
			std::fprintf( stderr, "adaptive: render failed\n" );
			return 1;
		}

		size_t differing = 0;
		for( size_t f = kSettle; f < a.size(); ++f )
			for( size_t i = 0; i < a[ f ].size(); ++i )
				if( a[ f ][ i ] != b[ f ][ i ] )
					++differing;

		const size_t compared = ( a.size() - kSettle ) * a[ 0 ].size();
		std::printf( "adaptive %s: %zu of %zu bytes differ\n", c.name, differing, compared );
		if( differing != 0 )
			++failures;
	}

	//And that the middle of the range is neither: a control whose ends are
	//two other modes and whose middle is one of them is a two-way switch.
	{
		std::vector< Image > mid, bob, weave;
		if( !renderRun( { "Mode=4", "Adaptive Threshold=0.15" }, mid ) || !renderRun( { "Mode=2" }, bob ) || !renderRun( { "Mode=0" }, weave ) )
			return 1;
		size_t fromBob = 0, fromWeave = 0;
		for( size_t f = kSettle; f < mid.size(); ++f )
			for( size_t i = 0; i < mid[ f ].size(); ++i )
			{
				fromBob += mid[ f ][ i ] != bob[ f ][ i ];
				fromWeave += mid[ f ][ i ] != weave[ f ][ i ];
			}
		std::printf( "adaptive threshold 0.15: differs from Bob Linear in %zu bytes and from Weave in %zu\n", fromBob, fromWeave );
		if( fromBob == 0 || fromWeave == 0 )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "adaptive: 0 is Bob Linear and 1 is Weave, pixel-exact" : "adaptive: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --swap
//
// Bob on Split with a bar moving one pixel a frame. With the field order
// believed correctly the shown field is K-1 every frame: 0 1 2 3 4 5. With
// it swapped the later field of each pair goes first: 1 0 3 2 5 4.
//---------------------------------------------------------------------------
int runSwap( int width, int height )
{
	int failures = 0;

	for( int swap = 0; swap <= 1; ++swap )
	{
		Session session( width, height, 60.0 );
		if( !session.set( "Field Source=0" ) || !session.set( "Mode=1" )
		    || !session.set( swap ? "Swap Field Order=1" : "Swap Field Order=0" ) || !session.init() )
			return 1;

		std::vector< int > got, want;
		Image out;
		for( int frame = 0; frame < 12; ++frame )
		{
			Image source = solid( width, height, 0 );
			paint( source, width, height, 20 + frame, 0, 24 + frame, height, 255, 255, 255 );
			if( !session.render( source, frame, &out ) )
			{
				std::fprintf( stderr, "swap: ProcessOpenGL failed\n" );
				return 1;
			}
			if( frame < 1 )
				continue;

			//Bob doubles the shown field into every row, so any row will do;
			//row 0 and row 1 are both read and must agree.
			const int x0 = leftEdge( out, width, 0, 128 );
			const int x1 = leftEdge( out, width, 1, 128 );
			if( x0 != x1 )
			{
				++failures;
				std::printf( "  swap=%d frame %d: rows 0 and 1 disagree (%d, %d)\n", swap, frame, x0, x1 );
			}
			got.push_back( x0 - 20 );

			const int pairFirst = ( frame - 1 ) & ~1;
			const bool firstHalf = ( frame & 1 ) != 0;
			const int shown = firstHalf ? ( swap ? pairFirst + 1 : pairFirst ) : ( swap ? pairFirst : pairFirst + 1 );
			want.push_back( shown );
		}

		std::printf( "swap=%d: shown field sequence", swap );
		for( int f : got )
			std::printf( " %d", f );
		std::printf( "  (expected" );
		for( int f : want )
			std::printf( " %d", f );
		std::printf( ")\n" );

		if( got != want )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "swap: a swapped field order goes two forward, one back" : "swap: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --ring
//
// Resize the picture mid-run on one plugin instance. It must not crash, the
// ring must come back empty, and nothing of the old picture may survive
// into the first new frame. Then change Field Source, which changes the
// ring's length, and check the same.
//---------------------------------------------------------------------------
int runRing( int width, int height )
{
	Session session( width, height, 60.0 );
	if( !session.set( "Field Source=0" ) || !session.set( "Mode=0" ) || !session.init() )
		return 1;

	int failures = 0;
	Image out;

	for( int frame = 0; frame < 10; ++frame )
		if( !session.render( solid( width, height, 200 ), frame, &out ) )
		{
			std::fprintf( stderr, "ring: ProcessOpenGL failed before the resize\n" );
			return 1;
		}

	const int filledBefore = session.Plugin().RingFilledForTest();

	//Up, then down, then a Field Source change.
	struct Step
	{
		const char* what;
		int width, height;
		const char* setting;
	};
	const Step steps[] = {
		{ "resize up", width * 2, height * 2, nullptr },
		{ "resize down", width / 2, height / 2, nullptr },
		{ "Split -> Telecine (ring 4 -> 8)", width / 2, height / 2, "Field Source=1" },
		{ "Telecine -> Split (ring 8 -> 4)", width / 2, height / 2, "Field Source=0" },
	};

	int frame = 10;
	for( const Step& step : steps )
	{
		session.resize( step.width, step.height );
		if( step.setting != nullptr && !session.set( step.setting ) )
			return 1;

		const int framesBefore = session.Plugin().RingFramesForTest();

		//A different picture, so anything of the old one is recognisable.
		if( !session.render( solid( step.width, step.height, 50 ), frame++, &out ) )
		{
			std::printf( "  %s: ProcessOpenGL FAILED\n", step.what );
			++failures;
			continue;
		}

		const int filled = session.Plugin().RingFilledForTest();
		const int frames = session.Plugin().RingFramesForTest();

		size_t stale = 0;
		for( size_t i = 0; i < out.size(); i += 4 )
			if( out[ i ] == 200 )
				++stale;

		const bool ok = filled == 1 && stale == 0;
		std::printf( "  %-36s ring %d -> %d frames, filled %d, stale pixels %zu  %s\n", step.what, framesBefore, frames, filled, stale, ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;

		//Run it on a few frames so the next step starts from a full ring.
		for( int i = 0; i < 6; ++i )
			session.render( solid( step.width, step.height, 200 ), frame++, nullptr );
	}

	std::printf( "ring: filled %d before the first resize\n", filledBefore );
	std::printf( "%s\n", failures == 0 ? "ring: a resize or a source change rebuilds the ring empty, without a crash" : "ring: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --lock
//
// The inverse telecine, and the artefact it exists for.
//
// Combing is exactly measurable here and needs no judgement about an edge:
// the two fields of one film frame carry the SAME input frame, so a woven
// pair whose two source serials differ is combed, and one whose serials
// agree cannot be. That is read straight out of the plugin.
//
// Two claims:
//
//   1. On a clean 2:3 cadence the detector locks and STAYS locked, and once
//      it has, no frame is combed. An inverse telecine that cannot do this
//      on undamaged material is not mis-locking, it is broken.
//   2. After a cadence break it weaves the wrong pair until it re-locks, and
//      how long that lasts is what Lock Time sets. A short Lock Time re-locks
//      sooner and combs fewer frames.
//---------------------------------------------------------------------------
struct LockRun
{
	int frames        = 0;
	int combed        = 0;///< frames whose woven pair straddled two film frames
	int combedAfter   = 0;///< the same, counted only after the first break
	int lockChanges   = 0;
	int breaks        = 0;
	int worstRelock   = 0;///< longest run of combed frames following a break
	int settledCombed = 0;///< combed frames in the last third of a clean run
};

LockRun measureLock( int width, int height, const std::vector< std::string >& settings, int frames )
{
	LockRun run;
	Session session( width, height, 60.0 );
	for( const std::string& s : settings )
		if( !session.set( s ) )
			return run;
	if( !session.init() )
		return run;

	int lastBreaks = 0;
	int currentRun = 0;

	for( int frame = 0; frame < frames; ++frame )
	{
		if( !session.render( buildCard( width, height, frame ), frame, nullptr ) )
		{
			std::fprintf( stderr, "lock: ProcessOpenGL failed on frame %d\n", frame );
			return run;
		}

		long fieldA = 0, fieldB = 0, shown = 0, serialA = 0, serialB = 0;
		session.Plugin().LastPresentationForTest( fieldA, fieldB, shown, serialA, serialB );

		const int breaks = session.Plugin().BreaksForTest();
		if( breaks != lastBreaks )
		{
			lastBreaks = breaks;
			currentRun = 0;
		}

		//The ring has to have filled before a pair means anything.
		if( frame < 12 )
			continue;

		++run.frames;
		const bool combed = serialA != serialB;
		if( combed )
		{
			++run.combed;
			if( breaks > 0 )
				++run.combedAfter;
			++currentRun;
			run.worstRelock = std::max( run.worstRelock, currentRun );
		}
		else
			currentRun = 0;

		if( frame >= frames * 2 / 3 && combed )
			++run.settledCombed;
	}

	run.lockChanges = session.Plugin().InverseTelecineForTest().LockChanges();
	run.breaks      = session.Plugin().BreaksForTest();
	return run;
}

int runLock( int width, int height )
{
	int failures = 0;

	//--- 1. A clean 2:3 cadence. -------------------------------------------
	{
		const LockRun clean = measureLock( width, height,
		                                   { "Field Source=1", "Source Rate=0", "Field Rate=1", "Mode=5", "Lock Time=0.35" }, 240 );
		std::printf( "lock clean 2:3:   %d frames, %d combed (%d in the settled last third), %d lock changes\n",
		             clean.frames, clean.combed, clean.settledCombed, clean.lockChanges );

		//Once settled, a correct lock weaves only pairs from one film frame.
		if( clean.settledCombed != 0 )
		{
			std::printf( "  FAILED: %d combed frames after the detector should have settled\n", clean.settledCombed );
			++failures;
		}
		if( clean.frames == 0 )
		{
			std::printf( "  FAILED: nothing rendered\n" );
			++failures;
		}
	}

	//--- 2. A cadence break, at two lock times. ----------------------------
	//
	// Break Interval 0.5 is about 2.7 s, so a 360-frame run holds two edits
	// with room between them to re-lock -- or not to.
	const char* const breakSettings[] = { "Field Source=2", "Source Rate=0", "Field Rate=1", "Mode=5", "Break Interval=0.5" };

	auto withLock = [ & ]( const char* lockTime ) {
		std::vector< std::string > settings( std::begin( breakSettings ), std::end( breakSettings ) );
		settings.push_back( std::string( "Lock Time=" ) + lockTime );
		return measureLock( width, height, settings, 360 );
	};

	const LockRun quick = withLock( "0.0" );//0.05 s
	const LockRun slow  = withLock( "1.0" );//4 s

	std::printf( "lock after a break, Lock Time 0.00 (%.2f s): %d breaks, %d combed frames, longest run %d, %d lock changes\n",
	             LockSecondsFromParam( 0.0f ), quick.breaks, quick.combedAfter, quick.worstRelock, quick.lockChanges );
	std::printf( "lock after a break, Lock Time 1.00 (%.2f s): %d breaks, %d combed frames, longest run %d, %d lock changes\n",
	             LockSecondsFromParam( 1.0f ), slow.breaks, slow.combedAfter, slow.worstRelock, slow.lockChanges );

	if( quick.breaks < 1 || slow.breaks < 1 )
	{
		std::printf( "  FAILED: no cadence break happened, so nothing was knocked off its lock\n" );
		++failures;
	}

	//A break has to cost something, or there is no artefact to speak of.
	if( quick.combedAfter == 0 && slow.combedAfter == 0 )
	{
		std::printf( "  FAILED: a cadence break combed nothing at either lock time\n" );
		++failures;
	}

	//And the control has to be a control. What is asserted is the number of
	//times the detector RE-LOCKS, not the number of combed frames, and the
	//difference is a real finding rather than a technicality:
	//
	// a lock time of four seconds against a break every 2.7 s never
	// converges at all, so the detector is frozen on whichever cycle
	// position it started from -- and a frozen lock is right by accident
	// about two frames in five. On the run below it combs FEWER frames than
	// the detector that is actually tracking. So combed frames do not order
	// the two lock times, and a check that asserted they did would be
	// asserting a coin toss: it passed or failed on which phases the breaks
	// happened to pick.
	//
	// Re-locks are the thing Lock Time sets, they are monotonic in it, and
	// they are what the operator is actually dialling.
	if( quick.lockChanges <= slow.lockChanges )
	{
		std::printf( "  FAILED: a 0.05 s lock re-locked %d times and a 4 s lock %d -- Lock Time is not doing what it says\n",
		             quick.lockChanges, slow.lockChanges );
		++failures;
	}

	std::printf( "lock: combed frames do NOT order the two -- a lock too slow to converge is frozen, and a frozen lock is right by accident. Re-locks are what Lock Time sets.\n" );
	std::printf( "%s\n", failures == 0
	                         ? "lock: a clean cadence settles and stops combing; a break combs until it re-locks, and Lock Time decides whether it re-locks at all"
	                         : "lock: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames, double fps )
{
	Session session( width, height, fps );
	for( const std::string& s : settings )
		if( !session.set( s ) )
			return -1.0;
	if( !session.init() )
		return -1.0;

	//The card is built once: this times the plugin, not the CPU painting a
	//4K test card sixty times.
	const Image card = buildCard( width, height, 0 );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( card, frame, nullptr );
	glFinish();

	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		session.render( card, warmup + frame, nullptr );
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	const double seconds = std::chrono::duration< double >( end - start ).count();
	return seconds * 1000.0 / static_cast< double >( frames );
}

int runBench( const std::vector< std::string >& settings, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};

	//glFinish on both sides, because GL calls queue and an unsynchronised
	//version times how fast a for loop runs; a warm-up thrown away, because
	//the first frames pay for allocating the ring.
	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( settings, size.width, size.height, frames, fps );
		if( ms < 0.0 )
			return 1;
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n",
		             size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0 );
	}

	std::printf( "\nThe cost is one copy and one composite per frame, plus -- in Inverse\n"
	             "Telecine only -- a diff, a reduce and a synchronous readback of sixteen\n"
	             "floats per field. Whatever --set said is what was measured.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe
//
// Raw RGBA in, raw RGBA out, one frame at a time, through the real plugin
// class -- the same Session every check above uses, so what a reel shows is
// what the checks measured.
//
// The clock is SYNTHETIC and driven by the frame index, not by the wall clock
// and not by the rate the pipe delivers. That is not a detail on this plugin:
// a field is a slice of time, so a stall in ffmpeg upstream would otherwise
// show up in the finished reel as the cadence speeding up, and 2:3 would stop
// being 2:3 halfway through a shot.
//---------------------------------------------------------------------------
int runPipe( int width, int height, double fps, const std::string& scriptPath,
             const std::vector< std::string >& settings, bool tone )
{
	Session session( width, height, fps );
	session.tone = tone;

	for( const std::string& setting : settings )
		if( !session.set( setting ) )
			return 2;

	//Resolve the script's parameter names to indices once, up front, and
	//refuse to run on a name that is not a parameter. A misspelled name that
	//silently did nothing would produce a take that looks deliberate and is
	//wrong -- the reel would hold whatever the default was, with a caption
	//over it describing a control that never moved.
	std::map< unsigned int, Track > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}

		for( const auto& entry : tracks )
		{
			const int index = findParameter( session.Plugin(), entry.first );
			if( index < 0 )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n",
				              entry.first.c_str() );
				return 2;
			}
			automation[ static_cast< unsigned int >( index ) ] = entry.second;
		}
	}

	if( !session.init() )
		return 1;

	Image frame( static_cast< size_t >( width ) * height * 4 );
	Image out;

	for( int index = 0;; ++index )
	{
		size_t filled = 0;
		while( filled < frame.size() )
		{
			const ssize_t got = read( STDIN_FILENO, frame.data() + filled, frame.size() - filled );
			if( got <= 0 )
				break;
			filled += static_cast< size_t >( got );
		}

		//End of stream. A PARTIAL frame is dropped rather than padded: half a
		//frame of black at the end of a reel is a flash, and a flash in an
		//export is a bug report. It also catches the commonest mistake here,
		//a --width or --height that does not match what ffmpeg is sending --
		//which otherwise produces a sheared picture rather than a message.
		if( filled < frame.size() )
		{
			if( filled > 0 )
				std::fprintf( stderr,
				              "dropped %zu bytes of a partial frame at frame %d -- do --width %d "
				              "--height %d match the stream?\n",
				              filled, index, width, height );
			break;
		}

		for( const auto& track : automation )
			session.Plugin().SetFloatParameter( track.first, valueAt( track.second, index ) );

		if( !session.render( frame, index, &out ) )
		{
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", index );
			return 1;
		}

		size_t written = 0;
		while( written < out.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
			if( put <= 0 )
				break;
			written += static_cast< size_t >( put );
		}

		//The consumer went away: ffmpeg hitting its own -frames limit, or a
		//head further down the pipeline. Not an error.
		if( written < out.size() )
			break;
	}

	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"cdtest -- render and check the Cadence interlace effect\n"
		"\n"
		"  --out PATH        render the moving test card through the plugin (default /tmp/cadence.png)\n"
		"  --card PATH       write the test card alone, undecorated\n"
		"  --size WxH        picture size (default 1280x720); --width N / --height N also work\n"
		"  --frames N        frames to render before reading back (default 24)\n"
		"  --fps N           synthetic frame rate driving the clock (default 60)\n"
		"  --tone            push a synthetic click train into the Audio buffer every frame\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its kind and its default, then exit\n"
		"  --comb            weave: a moving bar combs by exactly v pixels\n"
		"  --bob             bob: a two-line detail bounces one line per field\n"
		"  --pattern         telecine: 2:3 emits A A B B B C C D D D\n"
		"  --adaptive        adaptive at 0 is Bob Linear, at 1 is Weave, pixel-exact\n"
		"  --swap            swapped field order: 1 0 3 2 5 4\n"
		"  --lock            inverse telecine: a clean cadence settles; a break combs until it re-locks\n"
		"  --ring            a resize mid-run rebuilds the ring empty, no crash\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH     parameter cues for --pipe: 'frame Parameter Name value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/cadence.png";
	std::string cardPath;
	std::string scriptPath;
	int width  = 1280;
	int height = 720;
	int frames = 24;
	double fps = 60.0;
	bool tone  = false;
	bool wantList = false, wantComb = false, wantBob = false, wantPattern = false;
	bool wantAdaptive = false, wantSwap = false, wantRing = false, wantBench = false;
	bool wantLock = false, wantPipe = false;
	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--tone" )
			tone = true;
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--comb" )
			wantComb = true;
		else if( argument == "--bob" )
			wantBob = true;
		else if( argument == "--pattern" )
			wantPattern = true;
		else if( argument == "--adaptive" )
			wantAdaptive = true;
		else if( argument == "--swap" )
			wantSwap = true;
		else if( argument == "--lock" )
			wantLock = true;
		else if( argument == "--ring" )
			wantRing = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	//No GL needed, so answered before a context is made -- which means it
	//still works on a machine where creating one fails, and in CI.
	if( wantList )
	{
		Cadence plugin;
		for( const std::string& setting : settings )
		{
			std::string error;
			if( !applySetting( plugin, setting, error ) )
			{
				std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
				return 2;
			}
		}
		listParameters( plugin );
		return 0;
	}

	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, buildCard( width, height, 0 ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	//Before anything else that could print: stdout is the video in --pipe and
	//one stray line of text in it is a torn frame for the rest of the reel.
	if( wantPipe )
	{
		const int piped = runPipe( width, height, fps, scriptPath, settings, tone );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return piped;
	}

	int result = 0;
	//The checks run at a small size: they are about rows and columns, and
	//320x180 has all the rows and columns a claim needs.
	const int checkW = 320, checkH = 180;
	if( wantComb )
		result |= runComb( checkW, checkH );
	if( wantBob )
		result |= runBob( checkW, checkH );
	if( wantPattern )
		result |= runPattern( checkW, checkH );
	if( wantAdaptive )
		result |= runAdaptive( checkW, checkH );
	if( wantSwap )
		result |= runSwap( checkW, checkH );
	if( wantLock )
		result |= runLock( checkW, checkH );
	if( wantRing )
		result |= runRing( checkW, checkH );
	if( wantBench )
		result |= runBench( settings, frames, fps );

	if( wantComb || wantBob || wantPattern || wantAdaptive || wantSwap || wantLock || wantRing || wantBench )
	{
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	}

	//A still, at the end of a run of frames on the moving card.
	{
		Session session( width, height, fps );
		session.tone = tone;
		for( const std::string& setting : settings )
			if( !session.set( setting ) )
				return 2;
		if( !session.init() )
			return 1;

		Image out;
		for( int frame = 0; frame < frames; ++frame )
		{
			if( !session.render( buildCard( width, height, frame ), frame, frame == frames - 1 ? &out : nullptr ) )
			{
				std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
				return 1;
			}
		}

		if( !writePng( outPath, width, height, out ) )
		{
			std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}
