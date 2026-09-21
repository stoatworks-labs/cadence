#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
	float in 0..1. That is not a style preference: `SetParamInfo` clamps a
	standard default into 0..1 *before* returning, and `SetParamRange` can
	only be called afterwards -- so a parameter declared in seconds cannot
	declare a default in seconds. The conversions live here, in one file the
	plugin and the harness both use, so there is only ever one answer to what
	a slider position means.

	The rates are options, not sliders, because 24, 25, 30 and 60 are the
	only values that mean anything: a source at 27 fps has no cadence anybody
	recognises, and a slider that lands on it is a slider that lands on
	nothing.
*/
namespace cadence
{

/// The option VALUE of Source Rate, to hertz: 24, 25, 30 or 60.
int SourceRateFromOption( float optionValue );

/// The option VALUE of Field Rate, to hertz: 50 or 60.
int FieldRateFromOption( float optionValue );

/// Adaptive Threshold, in colour units. Linear, and deliberately the whole
/// 0..1: at 0 every non-identical pixel bobs, at 1 nothing can exceed it and
/// every pixel weaves. Those two ends are the harness's `--adaptive` claim.
float AdaptiveThresholdFromParam( float value );

/// Lock Time: 0.05 to 4 s, geometrically. The time constant the inverse
/// telecine's cadence scores settle with -- and so how long after an edit
/// it keeps weaving the wrong pair.
float LockSecondsFromParam( float value );

/// Break Interval: 0.25 to 30 s, geometrically. How often Cadence Break
/// puts an edit in.
float BreakIntervalFromParam( float value );

/// Line Filter: 0 to 1, linear. The side weight of a three-tap vertical
/// filter is a quarter of this, so at 1 the kernel is [0.25 0.5 0.25].
float LineFilterFromParam( float value );

} // namespace cadence
