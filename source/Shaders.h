#pragma once

/**
	The passes, as GLSL source.

	Four fragment shaders and one vertex shader:

	1. **copy**      picture size, into the ring slot the frame lands in.
	                 Resolves MaxUV once, so every later pass works on a
	                 texture we allocated where the picture fills it and an
	                 integer row IS a picture row.
	2. **diff**      a small grid (kDiffGridWidth x kDiffGridHeight). Each
	                 texel is the luma difference between two fields at one
	                 point on the lines they both own. Only in Inverse
	                 Telecine mode: it is the one number the detector sees.
	3. **reduce**    kReduceWidth x 1. Sums the grid by columns so the CPU
	                 reads back sixteen floats rather than nine thousand.
	4. **composite** output size, straight to the host's framebuffer. The
	                 deinterlacer, the Fields display, the line filter and
	                 the mix. Every field read in it is a `texelFetch` at an
	                 integer row: a field is a set of rows, and a bilinear
	                 sample between two rows is a sample between two fields.

	All the field arithmetic is in the composite shader's `deinterlace()`,
	which is called once per pixel -- or three times when Line Filter is up,
	because the filter is folded into the same pass rather than costing a
	buffer. That fold is why `--adaptive` can be pixel-exact: Adaptive at
	its two extremes and the two modes it collapses to go through the same
	function with the same arithmetic.
*/

namespace cadence
{

/// The measurement grid the diff pass renders at, and the row the reduce
/// pass leaves. 128 x 72 is 9,216 samples of a field: sparse against a
/// picture, plenty for a mean, and the same cost at 720p and 4K.
constexpr int kDiffGridWidth  = 128;
constexpr int kDiffGridHeight = 72;
constexpr int kReduceWidth    = 16;

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kDiffShader;
extern const char* const kReduceShader;
extern const char* const kCompositeShader;

} // namespace cadence
