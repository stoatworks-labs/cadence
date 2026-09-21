#pragma once

#include "Audio.h"
#include "Clock.h"
#include "PassBuffer.h"
#include "Pulldown.h"

#include <FFGLSDK.h>

#include <string>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

namespace cadence
{

/**
	Cadence -- interlace, pulldown and every way a deinterlacer gets it
	wrong, as an FFGL effect.

	**The one idea.** A television picture is two fields a field period
	apart, each holding alternate lines. Treat the input as fields with real
	time between them, put a film cadence in front, and show the result on a
	progressive screen through a chosen deinterlacer -- and every artefact
	anybody recognises falls out of the arithmetic rather than being drawn:
	combing, bob bounce, field-blend ghosting, motion-adaptive mistakes, 3:2
	judder, a cadence break the inverse telecine mis-locks on, and the
	two-forward-one-back stutter of a swapped field order.

	**Three stages, on two processors.** The CPU decides *which* frames are
	fields (`Pulldown.h`: a source frame per field, chosen by time out of a
	ring of recent input frames) and *which* fields the deinterlacer is
	looking at this frame. The GPU does everything that touches a pixel,
	with integer row parity: a field is a set of rows, and no pass here
	ever samples between two of them.

	**One shader per stage** (`Shaders.h`): a copy into the ring, a diff and
	a reduce that give the inverse telecine its one number per field, and
	the composite that is the deinterlacer.

	See AGENTS.md for the traps and for what is verified.
*/
class Cadence : public CFFGLPlugin
{
public:
	Cadence();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// `instantiateGL` pushes every declared default back through the
	/// setters and deletes the instance the moment one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the
	/// plugin, while every offline harness carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one: it renders as fast as the GPU
	/// allows, so there is nothing for the measurement to measure.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	//--- test hooks. Read by cdtest; nothing in the plugin's own operation
	//--- touches them.
	int RingFramesForTest() const
	{
		return ringSize;
	}
	int RingFilledForTest() const
	{
		return ringFilled;
	}
	long FieldsEmittedForTest() const
	{
		return fieldsEmitted;
	}
	int CadencePhaseForTest() const
	{
		return cadencePhase;
	}
	int BreaksForTest() const
	{
		return breakCount;
	}
	const pulldown::InverseTelecine& InverseTelecineForTest() const
	{
		return itc;
	}
	/// The two field indices the last frame composited from, the one it
	/// presented, and the INPUT FRAME each of the two carries -- so a check
	/// can say why a picture came out as it did.
	///
	/// The serials are what make combing exactly measurable rather than a
	/// judgement about an edge: two fields of one film frame carry the same
	/// input frame, so a woven pair whose serials differ is combed, and a
	/// pair whose serials agree cannot be.
	void LastPresentationForTest( long& fieldA, long& fieldB, long& shown,
	                              long& serialA, long& serialB ) const
	{
		fieldA  = lastFieldA;
		fieldB  = lastFieldB;
		shown   = lastShown;
		serialA = lastSerialA;
		serialB = lastSerialB;
	}

	/// The order the host shows them in.
	enum ParamID : FFUInt32
	{
		//Source
		PT_SOURCE,
		PT_SOURCE_RATE,
		PT_FIELD_RATE,
		PT_FIELD_ORDER,
		PT_SWAP,
		PT_BREAK_INTERVAL,
		PT_BREAK_ONSET,
		PT_AUDIO,

		//Deinterlace
		PT_MODE,
		PT_THRESHOLD,
		PT_LOCK,
		PT_SHOW_DECISION,

		//Display
		PT_DISPLAY,
		PT_LINE_FILTER,

		//Output
		PT_MIX,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum, so no saved composition's
		//parameter ids shift. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// What Field Source stores. A progression, so the list is not sorted.
	enum FieldSource
	{
		kSourceSplit    = 0,///< one field per input frame, alternating parity
		kSourceTelecine = 1,///< the input as Source Rate, laid out into Field Rate
		kSourceBreak    = 2,///< telecine with an edit every Break Interval, or on an onset
		kSourceCount
	};

	/// What Mode stores.
	enum Mode
	{
		kModeWeave           = 0,
		kModeBob             = 1,
		kModeBobLinear       = 2,
		kModeBlend           = 3,
		kModeAdaptive        = 4,
		kModeInverseTelecine = 5,
		kModeCount
	};

	/// What Display stores.
	enum Display
	{
		kDisplayProgressive = 0,
		kDisplayFields      = 1,
		kDisplayCount
	};

	/// The longest ring the plugin holds. Eight full pictures at 4K is a
	/// quarter of a gigabyte, which is why the ring is sized to the cadence
	/// rather than always this.
	static constexpr int kMaxRing = 8;

private:
	/// One field: which input frame it carries, which lines it owns.
	struct Field
	{
		long index;       ///< k, the field's place in the stream
		int parity;       ///< 0 = even rows from the top (the top field)
		long sourceSerial;///< the input frame, by serial number
	};

	/// Bring the ring to this size and length, and empty it if either
	/// changed. False means the driver would not give us the memory.
	bool ensureRing( int width, int height, int frames );
	void clearRing();

	/// The texture holding input frame `serial`, or the newest one if that
	/// frame has already been overwritten.
	GLuint textureOfSerial( long serial ) const;

	/// The newest input frame captured at or before `seconds`, or the
	/// oldest in the ring if none is.
	long serialAtOrBefore( double seconds ) const;

	const Field* fieldAt( long index ) const;
	void pushField( long index, int parity, long sourceSerial );

	/// Restart the pulldown at a different phase: an edit.
	void cadenceBreak( double now, int cycle );

	/// The inverse telecine's one number: how different field `k` is from
	/// the field two before it. Runs the diff and reduce passes and reads
	/// sixteen floats back.
	float sameParityDifference( const Field& current, const Field& twoBack );

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader diffShader;
	ffglex::FFGLShader reduceShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	//---------------------------------------------------------------------
	// The ring of recent input frames. A fixed array and not a
	// std::vector: ffglex::FFGLFBO has a user-declared destructor and raw GL
	// ids, so its implicit copy constructor duplicates the ids without
	// duplicating the objects, and a vector reallocation would hand two
	// PassBuffers the same framebuffer and delete it twice.
	//---------------------------------------------------------------------
	PassBuffer slots[ kMaxRing ];
	long slotSerial[ kMaxRing ] = {};
	double slotTime[ kMaxRing ] = {};

	int ringSize     = 0;
	int ringWidth    = 0;
	int ringHeight   = 0;
	int ringFilled   = 0;
	long frameSerial = 0;///< the next input frame's serial number

	//---------------------------------------------------------------------
	// The field stream: the last few fields emitted, oldest first.
	//---------------------------------------------------------------------
	static constexpr int kMaxFields = 8;
	Field fields[ kMaxFields ] = {};
	int fieldCount             = 0;

	long fieldsEmitted    = 0;   ///< Split: the next field's index. Telecine: fields emitted so far.
	long nextTelecineField = -1; ///< Telecine: the next field index due, or -1 before the first
	int cadencePhase      = 0;
	int breakCount        = 0;
	double lastBreakTime  = -1.0;

	long lastFieldA  = -1;
	long lastFieldB  = -1;
	long lastShown   = -1;
	long lastSerialA = -1;
	long lastSerialB = -1;

	pulldown::InverseTelecine itc;

	PassBuffer diffGrid;
	PassBuffer reduceRow;

	audio::Analyser analyser;
	Clock clock;
	double hostTime = -1.0;

	/// Counts frames so the sixtieth can log what the host's clock actually
	/// looks like. One line, once, in the diag log.
	int clockFrames = 0;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};

} // namespace cadence
