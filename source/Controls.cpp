#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace cadence
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*,
/// which is right for any quantity where the question is "how many times
/// longer" rather than "how much longer".
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}
} // namespace

int SourceRateFromOption( float optionValue )
{
	switch( static_cast< int >( std::lround( optionValue ) ) )
	{
	case 0: return 24;
	case 1: return 25;
	case 2: return 30;
	case 3: return 60;
	default: return 24;
	}
}

int FieldRateFromOption( float optionValue )
{
	return std::lround( optionValue ) == 0 ? 50 : 60;
}

float AdaptiveThresholdFromParam( float value )
{
	return clamp01( value );
}

float LockSecondsFromParam( float value )
{
	return geometric( 0.05f, 4.0f, value );
}

float BreakIntervalFromParam( float value )
{
	return geometric( 0.25f, 30.0f, value );
}

float LineFilterFromParam( float value )
{
	return clamp01( value );
}

} // namespace cadence
