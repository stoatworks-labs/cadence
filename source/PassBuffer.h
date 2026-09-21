#pragma once

#include <FFGLSDK.h>

namespace cadence
{
/**
	An off-screen buffer: one slot of the frame ring, or one stage of the
	inverse-telecine measurement.

	Three things on top of the SDK's FFGLFBO, all inherited from the fleet.

	**It reallocates only when it has to.** `Ensure()` is called every frame
	for every slot and is a no-op in the overwhelming majority of them.

	**It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
	deletes the framebuffer and the depth renderbuffer, then tests
	`depthBufferID` a second time where it plainly meant `colorTextureID` --
	so the colour texture is leaked on every release (SDK b1afaf9,
	`FFGLFBO.cpp`). `Destroy()` deletes it first. The ring here rebuilds on
	every resize and every change of Field Source, and an operator dragging a
	layer's size is asking for eight full pictures to be released a few
	times a second.

	**It owns its filtering.** Every buffer in this plugin is read with
	`texelFetch` -- a field is a set of integer rows, and a bilinear sample
	across two rows is a sample across two fields -- so `Nearest` is the
	only sampling anything here wants. The other two are kept because the
	trap of choosing wrongly is silent: `GL_LINEAR` on a data buffer does
	not fail, it returns plausible averages of unrelated numbers.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Sampling
	{
		Nearest,  ///< for data read texel-for-texel. No filtering, no mip chain.
		Linear,   ///< for pictures read between texels. Bilinear, no mip chain.
		Mipmapped ///< for pictures that also get reduced. Trilinear + GenerateMipmaps().
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a ring slot's
	/// contents are shown for as long as a field references it, and
	/// undefined texture memory shown for a few frames is a glitch nobody
	/// can reproduce.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling );

	/// Rebuild the mip chain from level 0, for a Sampling::Mipmapped buffer.
	void GenerateMipmaps();

	/// Clear to transparent black.
	void Clear();

	/// The colour texture, for binding as an input to a later pass. The SDK
	/// keeps `colorTextureID` protected and offers only `GetTextureInfo()`,
	/// which builds a six-field struct to reach one of them.
	GLuint TextureID() const
	{
		return colorTextureID;
	}

	GLsizei Width() const
	{
		return width;
	}

	GLsizei Height() const
	{
		return height;
	}

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	Sampling sampling = Sampling::Nearest;
};

} // namespace cadence
