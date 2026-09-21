#include "Pulldown.h"

#include <algorithm>
#include <cmath>

namespace cadence::pulldown
{
namespace
{
int gcd( int a, int b )
{
	while( b != 0 )
	{
		const int t = a % b;
		a           = b;
		b           = t;
	}
	return a;
}

/// C++ gives a negative left operand a negative remainder, and a phase
/// shift can take the field index below zero at start-up.
long floorMod( long a, long m )
{
	const long r = a % m;
	return r < 0 ? r + m : r;
}
} // namespace

int CycleFields( int sourceRate, int fieldRate )
{
	if( sourceRate <= 0 || fieldRate <= 0 )
		return 1;
	return fieldRate / gcd( sourceRate, fieldRate );
}

long SourceFrameOfField( long k, int phase, int sourceRate, int fieldRate )
{
	//floor( ( k + phase + 0.5 ) * Rs / Rf ), kept in integers by doubling:
	//( 2( k + phase ) + 1 ) * Rs / ( 2 Rf ). Both numerator and denominator
	//are positive past start-up; a negative numerator floors towards minus
	//infinity as the rule wants rather than towards zero as `/` does.
	const long numerator   = ( 2 * ( k + phase ) + 1 ) * sourceRate;
	const long denominator = 2L * fieldRate;
	long q                 = numerator / denominator;
	if( ( numerator % denominator ) != 0 && ( numerator < 0 ) )
		--q;
	return q;
}

int FieldParity( long k, bool bottomFieldFirst )
{
	return static_cast< int >( floorMod( k, 2 ) ) ^ ( bottomFieldFirst ? 1 : 0 );
}

double SourceFrameTime( long n, int phase, int sourceRate, int fieldRate )
{
	return static_cast< double >( n ) / sourceRate
	       - static_cast< double >( phase + 1 ) / fieldRate;
}

//---------------------------------------------------------------------------
void InverseTelecine::Observe( long k, float sameParityDiff, float lockSeconds, float fieldSeconds )
{
	const int position = static_cast< int >( floorMod( k, kPeriod ) );

	//One-pole average with a time constant of Lock Time. A time constant of
	//zero snaps: the detector then believes whatever the last cycle said,
	//which is the fastest re-lock and the twitchiest.
	const float alpha = lockSeconds <= 1e-4f ? 1.0f
	                                           : 1.0f - std::exp( -fieldSeconds / lockSeconds );
	score[ position ] += ( sameParityDiff - score[ position ] ) * alpha;

	//Re-lock only when a position is clearly better than the current one.
	//Without the margin a source with no repeats -- where every score is
	//noise -- would move the lock every few fields, and while that IS the
	//failure mode of a naive detector on the wrong material, a lock that
	//never settles is one an operator cannot watch happen.
	int best = locked;
	for( int i = 0; i < kPeriod; ++i )
		if( score[ i ] < score[ best ] )
			best = i;

	if( best != locked && score[ best ] < score[ locked ] * 0.8f )
	{
		locked = best;
		++lockChanges;
	}
}

int InverseTelecine::Position( long k ) const
{
	return static_cast< int >( floorMod( k - locked, kPeriod ) );
}

bool InverseTelecine::WeaveLatest( long k ) const
{
	//Relative to the repeat r (position 0): r-2, r-1, r are one frame and
	//r+1, r+2 the next. So positions 3 and 4 are the first two fields of a
	//three-field frame, 0 is its repeat, and 1 and 2 are the two-field frame.
	//
	//   position  fields of the current frame   weave
	//   3         just one                      the previous frame: ( k-2, k-1 )
	//   4         two                           ( k-1, k )
	//   0         three (k repeats k-2)         ( k-1, k )
	//   1         just one                      the previous frame: ( k-2, k-1 )
	//   2         two                           ( k-1, k )
	const int position = Position( k );
	return position == 4 || position == 0 || position == 2;
}

void InverseTelecine::Reset()
{
	for( float& s : score )
		s = 1.0f;
	locked      = 0;
	lockChanges = 0;
}

} // namespace cadence::pulldown
