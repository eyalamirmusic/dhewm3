/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms.
You should have received a copy of these additional terms immediately following
the terms and conditions of the GNU General Public License which accompanied the
Doom 3 Source Code.  If not, please request a copy in writing from id Software
at the address below.

If you have questions concerning this license or the applicable additional
terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc.,
Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

// eacp first, and this is not a style choice. idlib/Str.h does
//
//     #define strcmp idStr::Cmp
//
// and the same for eight other <cstring> functions, so any standard header
// pulled in after a Doom 3 header fails to compile on `using ::strcmp` - which
// is what libc++'s <cstring> is made of. Every translation unit that mixes the
// two has to include eacp's headers before Doom 3's.
#include <eacp/GPU/GPU.h>

#include "sys/platform.h"

#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"
#include "renderer/RenderBackend_Eacp.h"

/*
===============================================================================

	The eacp implementation of idRenderBackend. Phase 2 step 4 of ../../plan.md.

	Nothing here is a translation of the OpenGL one. The state bitfield is - it
	was already API-independent, which is why Phase 1 chose it as the seam - but
	the drawing is not: Doom 3's backend is a matrix stack, texture-env
	combiners, client arrays and two hand-written ARB programs, and none of the
	four has a counterpart in Metal or D3D12. So DrawView is rewritten rather
	than ported, out of the same viewDef_t the frontend already builds.

	What this step lands is everything around that: the device coming up, the
	images, the frame and its passes, and the per-draw state recorded where the
	draws will read it. DrawView itself still draws nothing - see step 4c.

===============================================================================
*/

using namespace eacp;

/*
================================================================================

	The host's two handles. See RenderBackend_Eacp.h.

================================================================================
*/

static GPU::GPUView *	eacpView = NULL;
static GPU::Frame *		eacpFrame = NULL;

void R_EacpSetView( GPU::GPUView *view ) {
	eacpView = view;
}

GPU::GPUView *R_EacpGetView( void ) {
	return eacpView;
}

void R_EacpSetFrame( GPU::Frame *frame ) {
	eacpFrame = frame;
}

/*
================================================================================

	Formats.

	Doom 3 hands the backend GL's internal formats, and its source pixels are
	always tightly packed RGBA bytes on this build - see UploadImageLevel's
	comment on why the .dds path cannot reach here yet. eacp stores one 8-bit
	four-channel format, so what the internal format decides is not the storage
	but the *swizzle*: GL_LUMINANCE8 samples as (l, l, l, 1) and GL_ALPHA8 as
	(0, 0, 0, a), and a shader reading a texture uploaded without that applied
	reads different numbers from the same file.

	Applied on the CPU as the pixels are copied, because the alternative is a
	shader variant per format and the conversion happens once per image while
	the sampling happens once per fragment.

================================================================================
*/

static void R_EacpSwizzleToRGBA( byte *dst, const byte *src, int numPixels, int internalFormat ) {
	for ( int i = 0 ; i < numPixels ; i++ ) {
		const byte	r = src[i*4+0];
		const byte	g = src[i*4+1];
		const byte	b = src[i*4+2];
		const byte	a = src[i*4+3];

		switch ( internalFormat ) {
		case GL_ALPHA8:
		case GL_ALPHA:
			// (0, 0, 0, a)
			dst[i*4+0] = 0;
			dst[i*4+1] = 0;
			dst[i*4+2] = 0;
			dst[i*4+3] = a;
			break;
		case GL_LUMINANCE8:
		case GL_LUMINANCE:
			// (l, l, l, 1), and GL's RGBA-to-luminance conversion is the red
			// channel rather than a weighted sum - which is exact here, since
			// SelectInternalFormat only picks this for an image whose three
			// colour channels were already equal.
			dst[i*4+0] = r;
			dst[i*4+1] = r;
			dst[i*4+2] = r;
			dst[i*4+3] = 255;
			break;
		case GL_LUMINANCE8_ALPHA8:
		case GL_LUMINANCE_ALPHA:
			dst[i*4+0] = r;
			dst[i*4+1] = r;
			dst[i*4+2] = r;
			dst[i*4+3] = a;
			break;
		case GL_INTENSITY8:
		case GL_INTENSITY:
			// (i, i, i, i) - one channel in all four, which is what a mask
			// wants and what makes this different from luminance.
			dst[i*4+0] = r;
			dst[i*4+1] = r;
			dst[i*4+2] = r;
			dst[i*4+3] = r;
			break;
		case GL_RGB8:
		case GL_RGB5:
		case GL_RGB:
			// No alpha channel at all, which GL samples as 1 rather than as
			// whatever the source happened to carry.
			dst[i*4+0] = r;
			dst[i*4+1] = g;
			dst[i*4+2] = b;
			dst[i*4+3] = 255;
			break;
		default:
			dst[i*4+0] = r;
			dst[i*4+1] = g;
			dst[i*4+2] = b;
			dst[i*4+3] = a;
			break;
		}
	}
}

/*
================================================================================

	idRenderBackendEacp

================================================================================
*/

class idRenderBackendEacp : public idRenderBackend {
public:
					idRenderBackendEacp();

	virtual const char *Name( void ) const { return "eacp"; }
	virtual backEndName_t Path( void ) const { return BE_EACP; }
	virtual void	Init( void );
	virtual void	Shutdown( void );
	virtual void	DrawView( void );
	virtual void	ReleaseTextures( void );
	virtual void	SetDefaultState( void );
	virtual void	SetDrawBuffer( int buffer );
	virtual void	SwapBuffers( void );
	virtual void	SetState( int stateBits );
	virtual void	ClearStateDelta( void );
	virtual void	SetCull( int cullType );
	virtual void	SetTexEnv( int env );
	virtual void	SelectTexture( int unit );
	virtual void	DrawIndexed( const srfTriangles_t *tri, int numIndexes );
	virtual void	CheckErrors( void );

	virtual void	AllocImage( idImage *image );
	virtual void	FreeImage( idImage *image );
	virtual void	BindImage( idImage *image );
	virtual void	BindImageFragment( idImage *image );
	virtual void	BindNoImage( void );
	virtual void	UploadImageLevel( idImage *image, int face, int level, int internalFormat,
									  int width, int height, int externalFormat,
									  const byte *pixels );
	virtual void	UploadCompressedImageLevel( idImage *image, int level, int internalFormat,
												int width, int height, int numBytes,
												const byte *data );
	virtual void	SetImageMaxLevel( idImage *image, int maxLevel );
	virtual void	UploadScratchImage( idImage *image, const byte *data, int cols, int rows );
	virtual void	SetImageFilterAndRepeat( const idImage *image );
	virtual void	SetCubeImageFilterAndRepeat( const idImage *image );
	virtual void	RefreshImageFilter( const idImage *image );
	virtual void	SetImageBorderColor( const idImage *image, const float rgba[4] );
	virtual void	CopyFramebufferToImage( idImage *image, int x, int y,
											int imageWidth, int imageHeight,
											bool useOversizedBuffer );
	virtual void	CopyDepthbufferToImage( idImage *image, int x, int y,
											int imageWidth, int imageHeight,
											bool useOversizedBuffer );

private:
	// The pass every draw goes into. One per Doom 3 view, because a view is
	// exactly what wants the depth and stencil planes cleared and the colour
	// kept - which is what a pass with clear turned off does.
	void			BeginPass( bool clearColor );
	void			EndPass( void );

	// The texture an idImage carries, created on the first upload and
	// destroyed by FreeImage. Held by pointer in idImage::backendTexture,
	// because GPU::Texture has no empty state to default-construct.
	static GPU::Texture *	TextureFor( const idImage *image );
	static void				ReplaceTexture( idImage *image, GPU::Texture *texture );

	GPU::RenderPass *	pass;
	bool				passClearsColor;

	// The image on each texture unit, which is what a draw binds. The GL
	// backend has no equivalent because GL remembers this itself.
	idImage *			boundImages[MAX_MULTITEXTURE_UNITS];

	// One warning per unimplemented path rather than one per frame, which is
	// the difference between a note and an unusable console.
	bool				warnedCopyFramebuffer;
	bool				warnedCompressed;
	bool				warnedExternalFormat;
};

static idRenderBackendEacp	renderBackendEacp;
idRenderBackend *			renderBackend = &renderBackendEacp;

idRenderBackendEacp::idRenderBackendEacp() {
	pass = NULL;
	passClearsColor = false;
	memset( boundImages, 0, sizeof( boundImages ) );
	warnedCopyFramebuffer = false;
	warnedCompressed = false;
	warnedExternalFormat = false;
}

/*
======================
idRenderBackendEacp::Init

What R_CheckPortableExtensions does on OpenGL: say what the device can do. The
difference is that here every answer is known at compile time rather than asked
of a driver, so this is a statement of what this backend implements rather than
a query - and the falses are the open gaps in plan.md section 5, each one a
capability Doom 3 will simply not use until it is filled in.
======================
*/
void idRenderBackendEacp::Init( void ) {
	GPU::Device &	device = GPU::Device::shared();

	glConfig.vendor_string = "eacp";
	glConfig.renderer_string = "eacp";
	glConfig.version_string = "eacp";
	glConfig.extensions_string = "";
	glConfig.glVersion = 0.0f;

	// Metal's guaranteed maximum on every device eacp runs on, and D3D12's at
	// feature level 11.
	glConfig.maxTextureSize = 16384;

	// The interaction pass is Doom 3's widest, at five: the light falloff, the
	// light projection, the bump map, the diffuse map and the specular map.
	// Eight is what both APIs guarantee several times over and what the shader
	// side has room for.
	glConfig.maxTextureUnits = 8;
	glConfig.maxTextureCoords = 8;
	glConfig.maxTextureImageUnits = 8;
	glConfig.maxTextureAnisotropy = 1.0f;

	glConfig.colorBits = 32;
	glConfig.alphabits = 8;
	glConfig.depthBits = 24;

	// The stencil plane the view was created with. Doom 3 clears the buffer to
	// 1 << (stencilBits - 1) so that a shadow count can go down as well as up
	// without wrapping, and eight is what both APIs' combined depth-stencil
	// format carries.
	glConfig.stencilBits = 8;

	glConfig.multitextureAvailable = true;
	glConfig.twoSidedStencilAvailable = true;
	glConfig.textureNonPowerOfTwoAvailable = true;

	// Gap 4. The pk4s carry every texture twice - 3395 .dds files beside 3771
	// .tga in the demo - and CheckPrecompressedImage is gated on this, so
	// saying no here loads the uncompressed original rather than needing a
	// decompressor. It costs load time and four times the memory, which is
	// what the gap is about.
	glConfig.textureCompressionAvailable = false;
	glConfig.bptcTextureCompressionAvailable = false;

	// Gap 7: sampling is fixed when an eacp shader is compiled, and the four
	// configurations it offers are linear or nearest by clamp or repeat. Mip
	// filtering and anisotropy are not among them.
	glConfig.anisotropicAvailable = false;
	glConfig.textureLODBiasAvailable = false;

	// Gap 5. Skyboxes and reflections; the normalization cube map that was the
	// third user can be deleted outright, normalize() being free in a shader.
	glConfig.cubeMapAvailable = false;

	// Never happens - Generate3DImage is defined and has no callers, so TT_3D
	// is unreachable (plan.md section 5, "Checked, and not gaps").
	glConfig.texture3DAvailable = false;

	// Fixed-function, and the whole reason DrawView is a rewrite rather than a
	// port. Nothing here can express a texture-env combiner, and nothing needs
	// to once the passes are shaders.
	glConfig.textureEnvAddAvailable = false;
	glConfig.textureEnvCombineAvailable = false;
	glConfig.registerCombinersAvailable = false;
	glConfig.envDot3Available = false;
	glConfig.sharedTexturePaletteAvailable = false;

	// The ARB programs, which this backend does not run and R_ARB2_Init is
	// never called to look for.
	glConfig.ARBVertexProgramAvailable = false;
	glConfig.ARBFragmentProgramAvailable = false;
	glConfig.allowARB2Path = false;

	// idVertexCache keeps its blocks in system memory when this is false, and
	// hands out plain pointers rather than buffer offsets. That is the shape
	// this backend wants for now: it streams what a draw needs through
	// GPU::StreamingBuffers, which is the pool idVertexCache will eventually
	// move onto wholesale.
	glConfig.ARBVertexBufferObjectAvailable = false;

	// An optimisation, and a scope cut (plan.md section 7).
	glConfig.depthBoundsTestAvailable = false;

	glConfig.glDebugOutputAvailable = false;
	glConfig.haveDebugContext = false;

	// The default framebuffer's alpha channel is a Wayland problem and there is
	// no Wayland here.
	glConfig.shouldFillWindowAlpha = false;
	glConfig.isWayland = false;

	common->Printf( "GPU: %s\n", device.name().c_str() );

	glConfig.isInitialized = true;
}

/*
======================
idRenderBackendEacp::Shutdown
======================
*/
void idRenderBackendEacp::Shutdown( void ) {
	EndPass();
	memset( boundImages, 0, sizeof( boundImages ) );
}

/*
======================
idRenderBackendEacp::BeginPass / EndPass

One eacp pass per Doom 3 view, and the mapping is not arbitrary: a pass clears
depth and stencil unconditionally as it opens and can be told whether to clear
colour, which is exactly what RB_BeginDrawingView asks for - the depth buffer
and the stencil buffer emptied for this view, the colour left where the views
before it put it.
======================
*/
void idRenderBackendEacp::BeginPass( bool clearColor ) {
	if ( pass ) {
		return;
	}

	if ( !eacpFrame ) {
		return;
	}

	GPU::RenderPassDescriptor	descriptor;

	descriptor.clear = clearColor;

	// What Doom 3 clears the stencil buffer to, and it is deliberately not
	// zero: a shadow volume's count goes down as well as up, and the buffer is
	// unsigned, so the algorithm starts half way up its range.
	descriptor.clearStencil = (unsigned char)( 1 << ( glConfig.stencilBits - 1 ) );

	pass = new GPU::RenderPass( eacpFrame->beginPass( descriptor ) );
	passClearsColor = false;
}

void idRenderBackendEacp::EndPass( void ) {
	if ( !pass ) {
		return;
	}

	pass->end();
	delete pass;
	pass = NULL;
}

/*
======================
idRenderBackendEacp::SetDefaultState
======================
*/
void idRenderBackendEacp::SetDefaultState( void ) {
	memset( &backEnd.glState, 0, sizeof( backEnd.glState ) );
	backEnd.glState.forceGlState = true;

	memset( boundImages, 0, sizeof( boundImages ) );
}

/*
======================
idRenderBackendEacp::SetDrawBuffer

Where the frame goes. There is one place it can go - the drawable the view
handed us - so what is left of this is the debug clear, which several of the
r_show* tools rely on to blank the parts of the screen they do not draw.
======================
*/
void idRenderBackendEacp::SetDrawBuffer( int buffer ) {
	EndPass();
	passClearsColor = true;
	BeginPass( true );
}

/*
======================
idRenderBackendEacp::SwapBuffers

Not a present. eacp's Frame presents itself when GPUView::render returns, which
is after common->Frame() has run - so all this has to do is close whatever pass
is still open, because a pass outliving the frame that made it is undefined.
======================
*/
void idRenderBackendEacp::SwapBuffers( void ) {
	EndPass();
}

/*
======================
idRenderBackendEacp::DrawView

Step 4c. The frontend's work all happens whether or not this draws - the
surfaces are culled, the interactions built, the shadow volumes extruded and the
command list issued - so an empty body here is the whole engine running with
nothing on the glass, which is the point of landing it separately.
======================
*/
void idRenderBackendEacp::DrawView( void ) {
}

/*
======================
idRenderBackendEacp::ReleaseTextures

Nothing to hand back. The GL version exists because Doom 3 shares one context
with the editor; here every texture is an object with an owner.
======================
*/
void idRenderBackendEacp::ReleaseTextures( void ) {
}

/*
======================
	Per-draw state.

	Recorded rather than acted on, because a modern pipeline is compiled from
	all of it at once rather than set one field at a time. Step 4c turns the
	recorded state into a RenderPipelineDescriptor and looks it up in a cache.
======================
*/
void idRenderBackendEacp::SetState( int stateBits ) {
	backEnd.glState.glStateBits = stateBits;
	backEnd.glState.forceGlState = false;
}

void idRenderBackendEacp::ClearStateDelta( void ) {
	backEnd.glState.forceGlState = true;
}

void idRenderBackendEacp::SetCull( int cullType ) {
	backEnd.glState.faceCulling = cullType;
}

void idRenderBackendEacp::SetTexEnv( int env ) {
	backEnd.glState.tmu[backEnd.glState.currenttmu].texEnv = env;
}

void idRenderBackendEacp::SelectTexture( int unit ) {
	if ( unit < 0 || unit >= MAX_MULTITEXTURE_UNITS ) {
		common->Warning( "GL_SelectTexture: unit = %i", unit );
		return;
	}

	backEnd.glState.currenttmu = unit;
}

void idRenderBackendEacp::DrawIndexed( const srfTriangles_t *tri, int numIndexes ) {
}

void idRenderBackendEacp::CheckErrors( void ) {
	// Both backends under eacp report as they happen: Metal's API validation
	// aborts on the call that was wrong, and D3D12's debug layer prints there.
	// There is no error queue to drain.
}

/*
================================================================================

	Images.

================================================================================
*/

GPU::Texture *idRenderBackendEacp::TextureFor( const idImage *image ) {
	return (GPU::Texture *)image->backendTexture;
}

void idRenderBackendEacp::ReplaceTexture( idImage *image, GPU::Texture *texture ) {
	delete (GPU::Texture *)image->backendTexture;
	image->backendTexture = texture;
}

/*
====================
idRenderBackendEacp::AllocImage

A name, and nothing behind it yet - the texture is created by the first upload,
which is the first point its size and format are known. texnum is what the rest
of the engine tests against TEXTURE_NOT_LOADED, so it has to become something
here even though it names nothing on this backend.
====================
*/
void idRenderBackendEacp::AllocImage( idImage *image ) {
	static GLuint	nextName = 1;

	image->texnum = nextName++;
	image->backendTexture = NULL;
}

void idRenderBackendEacp::FreeImage( idImage *image ) {
	ReplaceTexture( image, NULL );

	for ( int i = 0 ; i < MAX_MULTITEXTURE_UNITS ; i++ ) {
		if ( boundImages[i] == image ) {
			boundImages[i] = NULL;
		}
	}
}

void idRenderBackendEacp::BindImage( idImage *image ) {
	boundImages[backEnd.glState.currenttmu] = image;
	backEnd.glState.tmu[backEnd.glState.currenttmu].textureType = image->type;
}

void idRenderBackendEacp::BindImageFragment( idImage *image ) {
	boundImages[backEnd.glState.currenttmu] = image;
}

void idRenderBackendEacp::BindNoImage( void ) {
	boundImages[backEnd.glState.currenttmu] = NULL;
	backEnd.glState.tmu[backEnd.glState.currenttmu].textureType = TT_DISABLED;
}

/*
====================
idRenderBackendEacp::UploadImageLevel

Level 0 only, and the reason is eacp's rather than a shortcut. Doom 3 builds its
own mip chain and uploads it a level at a time; eacp builds one on the CPU from
level 0 - deliberately, so that Metal's generateMipmaps and a hand-written D3D12
chain cannot produce two different pictures - and has no per-level entry point
at all. So the levels below the first are dropped and eacp's own are used.

Two things go with them, and both are worth knowing before someone looks for the
bug: R_MipMap's preserveBorder, which is what keeps a TR_CLAMP_TO_ZERO image's
zero edge intact all the way down its chain, and image_colorMipLevels, the
debug tool that tints each level.
====================
*/
void idRenderBackendEacp::UploadImageLevel( idImage *image, int face, int level, int internalFormat,
											int width, int height, int externalFormat,
											const byte *pixels ) {
	if ( level != 0 || face != 0 ) {
		return;
	}

	if ( externalFormat != GL_RGBA ) {
		// The .dds path, which textureCompressionAvailable = false keeps shut,
		// is the only producer of anything else.
		if ( !warnedExternalFormat ) {
			warnedExternalFormat = true;
			common->Warning( "eacp: image '%s' uploaded in an unsupported source format 0x%x",
							 image->imgName.c_str(), externalFormat );
		}
		return;
	}

	if ( width <= 0 || height <= 0 || pixels == NULL ) {
		return;
	}

	byte *		swizzled = (byte *)R_StaticAlloc( width * height * 4 );
	R_EacpSwizzleToRGBA( swizzled, pixels, width * height, internalFormat );

	GPU::TextureDescriptor	descriptor;

	descriptor.width = width;
	descriptor.height = height;
	descriptor.format = GPU::TextureFormat::RGBA8Unorm;

	// Only where something will sample them. TF_DEFAULT is Doom 3's name for
	// "the mipmapped one"; TF_LINEAR and TF_NEAREST both sample level 0 alone,
	// so a chain built for them is a third more upload and memory for nothing.
	descriptor.mipmapped = ( image->filter == TF_DEFAULT );

	ReplaceTexture( image, new GPU::Texture( GPU::Device::shared(), descriptor, swizzled ) );

	R_StaticFree( swizzled );
}

/*
====================
idRenderBackendEacp::UploadCompressedImageLevel

Gap 4. Unreachable while textureCompressionAvailable is false, since that is
what CheckPrecompressedImage tests before it opens a .dds at all - so this
warns rather than decompresses, and the warning is the thing to look for if
that flag is ever turned on before a decoder exists.
====================
*/
void idRenderBackendEacp::UploadCompressedImageLevel( idImage *image, int level, int internalFormat,
													  int width, int height, int numBytes,
													  const byte *data ) {
	if ( !warnedCompressed ) {
		warnedCompressed = true;
		common->Warning( "eacp: compressed texture upload is not implemented (image '%s')",
						 image->imgName.c_str() );
	}
}

void idRenderBackendEacp::SetImageMaxLevel( idImage *image, int maxLevel ) {
	// The .dds path's, and it goes with UploadCompressedImageLevel.
}

/*
====================
idRenderBackendEacp::UploadScratchImage

The cinematic path: a whole image replaced every frame. Texture::update reuses
the GPU resource, which is the point of having a separate entry point for this
at all - creating one per video frame would allocate at 30Hz for the life of the
menu.
====================
*/
void idRenderBackendEacp::UploadScratchImage( idImage *image, const byte *data, int cols, int rows ) {
	if ( rows == cols * 6 ) {
		// A cube map animation, which needs gap 5 first.
		return;
	}

	if ( cols <= 0 || rows <= 0 || data == NULL ) {
		return;
	}

	image->type = TT_2D;

	GPU::Texture *	texture = TextureFor( image );

	if ( texture && texture->width() == cols && texture->height() == rows ) {
		texture->update( data );
	} else {
		GPU::TextureDescriptor	descriptor;

		descriptor.width = cols;
		descriptor.height = rows;
		descriptor.format = GPU::TextureFormat::RGBA8Unorm;

		ReplaceTexture( image, new GPU::Texture( GPU::Device::shared(), descriptor, data ) );
	}

	image->uploadWidth = cols;
	image->uploadHeight = rows;

	BindImage( image );
}

/*
====================
	Sampler state.

	Nothing to do, and that is the interesting part rather than an omission.
	eacp bakes the sampling into the shader - a Windows-on-Arm driver bug made
	a per-draw sampler descriptor unreliable, so the root signature declares a
	static sampler per (slot, configuration) pair instead. So an image's filter
	and repeat are not state to set on it; they select which compiled variant
	of a program draws it, which is step 4c's business and is read straight off
	idImage there.
====================
*/
void idRenderBackendEacp::SetImageFilterAndRepeat( const idImage *image ) {
}

void idRenderBackendEacp::SetCubeImageFilterAndRepeat( const idImage *image ) {
}

void idRenderBackendEacp::RefreshImageFilter( const idImage *image ) {
}

void idRenderBackendEacp::SetImageBorderColor( const idImage *image, const float rgba[4] ) {
	// TR_CLAMP_TO_BORDER's one user is the generated _borderClamp image, and
	// R_BorderClampImage has already written zeroes into its edge texels - so
	// clamp-to-edge over that data is the same picture as a zero border, which
	// is the argument plan.md section 4.3 makes for the whole repeat enum.
}

/*
====================
idRenderBackendEacp::CopyFramebufferToImage

_currentRender, and it is the one thing here that needs a different frame shape
rather than a different call: a texture cannot be sampled by the pass that is
rendering into it, so this needs the composited frame to live in an app-owned
render target that the drawable is blitted from at the end - PureDOOM's
captureTarget, which plan.md section 3 flags as the answer for exactly this.
====================
*/
void idRenderBackendEacp::CopyFramebufferToImage( idImage *image, int x, int y,
												  int imageWidth, int imageHeight,
												  bool useOversizedBuffer ) {
	if ( !warnedCopyFramebuffer ) {
		warnedCopyFramebuffer = true;
		common->Warning( "eacp: copying the framebuffer into an image is not implemented yet" );
	}

	backEnd.c_copyFrameBuffer++;
}

void idRenderBackendEacp::CopyDepthbufferToImage( idImage *image, int x, int y,
												  int imageWidth, int imageHeight,
												  bool useOversizedBuffer ) {
}
