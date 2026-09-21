#pragma once

/**
	The cadence, as arithmetic. No GL, no pixels: which source frame a
	field carries, which lines it owns, when its source was shot, and how a
	naive inverse telecine decides which two fields to weave.

	Everything here runs on the CPU in the plugin, and the harness calls the
	same functions to say what the GPU *should* have shown -- so a claim like
	"3:2 emits A A B B B C C D D D" is checked against the picture, not
	against a second copy of the rule.

	------------------------------------------------------------- the rule

	A field is a slice of time as much as a slice of lines. Field `k` at a
	field rate `Rf` covers `[k/Rf, (k+1)/Rf)`, and it carries the source frame
	that was current at the *middle* of that slice:

	    n(k) = floor( (k + 0.5) * Rs / Rf )

	One rule, and every pulldown anybody names is a special case of it:

	    24 -> 60   n = 0 0 1 1 1 2 2 3 3 3   2:3 (A A B B B C C D D D)
	    25 -> 50   n = 0 0 1 1 2 2           2:2
	    30 -> 60   n = 0 0 1 1 2 2           2:2
	    60 -> 60   n = 0 1 2 3               one field per frame

	The plugin does not have a 24p clip; it has a stream of host frames. So a
	"source frame" is the host frame that arrived at or before the moment
	that source frame would have been shot, and the telecine runs one field
	late so that the frame is always already in the ring. See
	`SourceFrameTime`.

	------------------------------------------------------------- an edit

	A cadence break is the pulldown restarting at a different phase, which is
	what an edit between two telecined shots looks like to a deinterlacer.
	`phase` shifts the field index the rule sees, in whole fields, so the
	repeat lands somewhere else in the five-field cycle.
*/
namespace cadence::pulldown
{

/// Fields in one full cycle of the pattern: Rf / gcd( Rs, Rf ). Five for
/// 24 -> 60, two for 25 -> 50, one for 60 -> 60.
int CycleFields( int sourceRate, int fieldRate );

/// The source frame field `k` carries, under a phase shift of `phase`
/// fields. Integer arithmetic throughout: the whole point of the pattern is
/// that it is exact, and a float division that lands a hair under an
/// integer is a different pattern.
long SourceFrameOfField( long k, int phase, int sourceRate, int fieldRate );

/// The line parity field `k` owns: 0 for the even lines counted from the
/// TOP of the picture (the top field), 1 for the odd ones. Top field first
/// means even fields are top fields.
int FieldParity( long k, bool bottomFieldFirst );

/// When source frame `n` was shot, in the host's seconds, under the same
/// phase shift. The plugin takes the newest ring entry captured at or
/// before this. It is one field period *earlier* than the frame's own
/// start, `n / Rs`, and that is what guarantees the entry exists: the first
/// field carrying frame n begins no earlier than n/Rs - 0.5/Rf, so a frame
/// shot a whole field before that is always in the past.
double SourceFrameTime( long n, int phase, int sourceRate, int fieldRate );

/**
	A naive inverse telecine.

	It watches one number per field -- how different the field is from the
	field two before it, which owns the same lines -- and looks for the field
	that is a *repeat*. In 2:3 that happens once every five fields, at a fixed
	position in the cycle, so the score for that position falls towards zero
	while the other four stay up. The position with the lowest score is the
	lock.

	Given the lock, the cycle is known and so is which two fields belong to
	the same film frame. `WeaveLatest` says whether that pair is the newest
	two fields or the two before them.

	What makes it naive is exactly what the plugin is for. The scores are a
	running average with a time constant of `Lock Time`, so after an edit the
	old lock persists until the new repeat position has had time to win --
	and until it does, the pairs it weaves straddle two film frames and comb.
	On a source with no repeats at all (Split, or 2:2) the lowest score is
	noise, the lock wanders, and the output stutters and combs in the way a
	3:2 detector fed the wrong material really does.
*/
class InverseTelecine
{
public:
	static constexpr int kPeriod = 5;

	/// A new field `k` has arrived, and this is its difference from field
	/// `k - 2` (mean absolute luma, 0..1). `fieldSeconds` is one field
	/// period, `lockSeconds` the averaging time constant.
	void Observe( long k, float sameParityDiff, float lockSeconds, float fieldSeconds );

	/// The cycle position the detector currently believes holds the repeat.
	int Phase() const
	{
		return locked;
	}

	/// The position of field `k` in the believed cycle, 0..4, where 0 is the
	/// repeat field (the third of a three-field frame).
	int Position( long k ) const;

	/// At field `k`, weave ( k-1, k )? Otherwise weave ( k-2, k-1 ). Under
	/// a correct lock the answer is always a pair from one film frame; under
	/// a wrong one it is sometimes not, and that is the artefact.
	bool WeaveLatest( long k ) const;

	/// How many times the lock has moved. The harness counts these.
	int LockChanges() const
	{
		return lockChanges;
	}

	float Score( int position ) const
	{
		return score[ position < 0 ? 0 : ( position >= kPeriod ? kPeriod - 1 : position ) ];
	}

	void Reset();

private:
	float score[ kPeriod ] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	int locked             = 0;
	int lockChanges        = 0;
};

} // namespace cadence::pulldown
