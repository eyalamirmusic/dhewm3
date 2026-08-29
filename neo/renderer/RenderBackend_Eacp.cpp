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

#include "renderer/RenderProgs_Eacp.h"

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

	Step 4c drew the 2D view - the menus, the console, the HUD, the loading
	screens - which is one path: shader passes over a viewDef with no
	viewEntitys. Step 4d is the world, in three: the depth fill, the interaction
	program and the stencil shadow pass. The first of the three is in, so a 3D
	view now occludes correctly and draws its ambient stages; DrawView says
	where the other two would go.

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
	virtual bool	ReadPixels( int x, int y, int width, int height, byte *rgb );

private:
	// Everything one draw needs that is not the geometry: a material stage's
	// expression, said in the terms idEacpStageProgram takes rather than the
	// terms the .mtr file uses.
	//
	// It is one struct rather than two because the depth fill and the ambient
	// pass are one expression - RB_T_FillDepthBuffer draws a material's
	// alpha-tested stages exactly as RB_STD_T_RenderShaderPasses draws its
	// ambient ones, with the colour set to black and the alpha test on.
	struct eacpStage_t {
		const idImage *		image;			// what is sampled
		int					stateBits;		// GLS_*, which the pipeline is compiled from
		int					cullType;
		Float4				color;			// the constant colour
		stageVertexColor_t	vertexColor;	// how the vertex colour joins it
		const float *		textureMatrix;	// RB_GetShaderTextureMatrix's 16, or NULL
		bool				alphaTest;
		float				alphaTestRef;
	};

	// The pass every draw goes into. One per frame so far - see DrawView on why
	// a view does not get its own yet.
	void			BeginPass( bool clearColor );
	void			EndPass( void );

	// RB_BeginDrawingView: where on the target this view lands, what part of it
	// may be written, and the state the view starts from.
	void			BeginDrawingView( void );
	void			SetViewport( const idScreenRect &rect );
	void			SetScissor( const idScreenRect &rect );

	// The matrix a draw is transformed by, rebuilt when the surface's space or
	// its depth hack changes. This is also where both depth hacks land, each of
	// them being a modified projection matrix and a depth range.
	void			SetSpace( const viewEntity_t *space, float modelDepthHack );

	// RB_STD_FillDepthBuffer and RB_T_FillDepthBuffer, rewritten. What every
	// light after them tests against: the surface at its own depth, so an
	// interaction pass can run at GLS_DEPTHFUNC_EQUAL and touch only the
	// fragments that survived.
	void			FillDepthBuffer( drawSurf_t **drawSurfs, int numDrawSurfs );
	void			FillDepthBufferSurface( const drawSurf_t *surf );

	// RB_ARB2_DrawInteractions, RB_ARB2_CreateDrawInteractions and
	// RB_ARB2_DrawInteraction. The light half of a view: every light in turn,
	// each of its surfaces decomposed into the primitive interactions
	// RB_CreateSingleDrawInteractions produces, added to the frame.
	void			DrawInteractions( void );
	void			CreateDrawInteractions( const drawSurf_t *surf );

public:
	// Reached through R_EacpDrawInteraction below, because
	// RB_CreateSingleDrawInteractions takes a plain function pointer - it was
	// written to be shared by the four backends Doom 3 shipped with, and a
	// member function is not what it asks for.
	void			DrawInteraction( const drawInteraction_t *din );

private:
	// RB_STD_DrawShaderPasses and RB_STD_T_RenderShaderPasses, rewritten. Not
	// ported: what they do with two texture units and six combiner calls is one
	// expression in idEacpStageProgram, and what they do with a matrix stack is
	// one uniform. Returns how many surfaces were consumed, which is what says
	// whether a second pass over the post-process ones is owed.
	int				DrawShaderPasses( drawSurf_t **drawSurfs, int numDrawSurfs );
	void			DrawSurfaceShaderPasses( const drawSurf_t *surf );

	// One stage of one surface: the program and pipeline its state compiles to,
	// the five uniforms that are its expression, and the draw.
	void			DrawStage( const srfTriangles_t *tri, const eacpStage_t &stage );

	// The texture an idImage carries, created on the first upload and
	// destroyed by FreeImage. Held by pointer in idImage::backendTexture,
	// because GPU::Texture has no empty state to default-construct.
	static GPU::Texture *	TextureFor( const idImage *image );
	static void				ReplaceTexture( idImage *image, GPU::Texture *texture );

	GPU::RenderPass *	pass;

	// The image on each texture unit, which is what a draw binds. The GL
	// backend has no equivalent because GL remembers this itself.
	idImage *			boundImages[MAX_MULTITEXTURE_UNITS];

	// What the next DrawIndexed will use, and the reason the GL backend needs
	// no equivalent: in OpenGL every one of these *is* context state, set by
	// whoever last touched it and still there when glDrawElements is reached.
	// Here they are arguments to a draw, so the code that would have set the
	// GL state sets these instead and DrawIndexed reads them.
	//
	// Null means nothing has been prepared, which is what makes a draw arriving
	// from a path this backend has not written yet - the shadow volumes, the
	// fog, the debug tools - a no-op rather than a draw against whatever the
	// last one left bound.
	//
	// The program is a GPU::ShaderProgram rather than either of the two
	// concrete ones, because a draw is issued the same way whichever wrote it:
	// setUniforms and bindTextures are the base's.
	GPU::ShaderProgram *				drawProgram;
	const GPU::RenderPipeline *			drawPipeline;
	const GPU::Buffer *					drawVertices;

	// Clip from model for the space being drawn, rebuilt when the space
	// changes. GL kept this in the matrix stack.
	float				modelViewProjection[16];

	// What a depth hack sets, and the reason it is remembered: it is part of
	// the viewport on this backend, so changing it means re-issuing the
	// viewport, and re-issuing the viewport per surface would be the only
	// per-surface call in the walk.
	float				depthRangeNear;
	float				depthRangeFar;

	// The model depth hack modelViewProjection was last built with. Not read
	// off backEnd.currentSpace, because one surface of a space can refuse the
	// hack the space asks for: a soft particle does, and its neighbour in the
	// same space does not.
	float				appliedDepthHack;

	// One warning per unimplemented path rather than one per frame, which is
	// the difference between a note and an unusable console.
	bool				warnedCopyFramebuffer;
	bool				warnedCompressed;
	bool				warnedExternalFormat;
	bool				warnedTexgen;
	bool				warnedMissingTexture;
	bool				warnedClipPlanes;
	bool				warnedSubviewPass;
	bool				warnedReadPixels;
	bool				warnedShadows;
};

static idRenderBackendEacp	renderBackendEacp;
idRenderBackend *			renderBackend = &renderBackendEacp;

/*
====================
R_EacpDrawInteraction

The trampoline RB_CreateSingleDrawInteractions calls back through. It takes a
plain function pointer - it was written to be shared by the four backends Doom 3
shipped with, and a member function is not what it asks for - so this is here,
where there is a backend to call.
====================
*/
static void R_EacpDrawInteraction( const drawInteraction_t *din ) {
	renderBackendEacp.DrawInteraction( din );
}

idRenderBackendEacp::idRenderBackendEacp() {
	pass = NULL;
	memset( boundImages, 0, sizeof( boundImages ) );
	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
	memset( modelViewProjection, 0, sizeof( modelViewProjection ) );
	depthRangeNear = 0.0f;
	depthRangeFar = 1.0f;
	appliedDepthHack = 0.0f;
	warnedCopyFramebuffer = false;
	warnedCompressed = false;
	warnedExternalFormat = false;
	warnedTexgen = false;
	warnedMissingTexture = false;
	warnedClipPlanes = false;
	warnedSubviewPass = false;
	warnedReadPixels = false;
	warnedShadows = false;
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

	// What the content actually cost, rather than what plan.md section 4.3
	// sized it at: two numbers, at the one moment the whole run is known.
	common->Printf( "eacp: %i material-stage programs and %i pipelines compiled\n",
					eacpRenderProgs.NumPrograms(), eacpRenderProgs.NumPipelines() );

	eacpRenderProgs.Shutdown();
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

	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
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
================================================================================

	Drawing.

	Step 4c drew everything Doom 3 puts on screen without a world - the menus,
	the console, the HUD, the loading screens and the in-world GUIs - which
	arrive as one viewDef with no viewEntitys and go through shader passes
	alone.

	Step 4d is the world, and it is three things rather than one. This is the
	first: the depth fill, which puts every opaque and perforated surface into
	the depth buffer at its own depth and leaves the colour black. On its own
	that draws a silhouette, and the picture is the ambient stages that are
	drawn over it - a sky, a light's own glow, a screen's GUI. What it is
	really for is the two steps after it: an interaction pass can then run at
	GLS_DEPTHFUNC_EQUAL and touch only the fragments that survived, and a shadow
	volume can be counted against a depth buffer that is already right.

================================================================================
*/

/*
======================
R_EacpModelViewProjection

Clip from model, with one correction that is the whole reason this is not a
plain matrix multiply.

OpenGL clips against -w <= z <= w and Metal and D3D12 against 0 <= z <= w, so a
projection matrix written for the first puts half its depth range outside the
second's frustum. The 2D projection is the case that makes it obvious rather
than subtle: glOrtho( 0, 640, 480, 0, 0, 1 ) sends every vertex at z = 0 - which
is every vertex the gui model produces - to z_ndc = -1, exactly the near plane
in OpenGL and just outside the frustum everywhere else. Uncorrected, the menu is
not dim or misplaced; it is entirely clipped away.

The fix is one row: z' = (z + w) / 2, which is the standard mapping and is
applied here rather than in the shader because it is a property of the API the
matrix is going to, not of the geometry it came from.

Both matrices are OpenGL's column-major layout, which is also MSL's, so the
result is uploaded to a Float4x4 uniform as it stands.
======================
*/
static void R_EacpModelViewProjection( const float modelView[16], const float projection[16],
									   float out[16] ) {
	for ( int column = 0 ; column < 4 ; column++ ) {
		for ( int row = 0 ; row < 4 ; row++ ) {
			float	sum = 0.0f;

			for ( int k = 0 ; k < 4 ; k++ ) {
				sum += projection[k*4 + row] * modelView[column*4 + k];
			}

			out[column*4 + row] = sum;
		}
	}

	for ( int column = 0 ; column < 4 ; column++ ) {
		out[column*4 + 2] = 0.5f * ( out[column*4 + 2] + out[column*4 + 3] );
	}
}

/*
======================
idRenderBackendEacp::SetViewport / SetScissor

Doom 3's viewport and scissor are OpenGL's: pixels with the origin at the bottom
left, and inclusive bounds. eacp's are the render target's pixels with the
origin at the top left, which is Metal's and D3D12's - so the y edge has to be
measured from the other end, and the width is x2 - x1 + 1 rather than x2 - x1.

The target's height comes from the pass rather than from glConfig, which is
eacp finding I6's whole point: the pass knows what it is rendering into and a
view's bounds do not.
======================
*/
void idRenderBackendEacp::SetViewport( const idScreenRect &rect ) {
	const float	height = (float)pass->targetHeight();

	// The depth range goes out with the viewport rather than on its own, which
	// is eacp's shape and not Doom 3's: glDepthRange is its own call, and the
	// two depth hacks use it without touching the rectangle. Re-sending the
	// rectangle to change the range costs nothing and keeps one call site.
	pass->setViewport( Graphics::Rect( (float)rect.x1,
									   height - (float)( rect.y2 + 1 ),
									   (float)( rect.x2 + 1 - rect.x1 ),
									   (float)( rect.y2 + 1 - rect.y1 ) ),
					   depthRangeNear, depthRangeFar );
}

void idRenderBackendEacp::SetScissor( const idScreenRect &rect ) {
	// A surface's scissorRect is inside the viewport, which is what the +
	// viewport origin is doing here - qglScissor is given the same sum.
	const idScreenRect &	viewport = backEnd.viewDef->viewport;
	const float				height = (float)pass->targetHeight();

	const float	x = (float)( viewport.x1 + rect.x1 );
	const float	y = (float)( viewport.y1 + rect.y1 );
	const float	w = (float)( rect.x2 + 1 - rect.x1 );
	const float	h = (float)( rect.y2 + 1 - rect.y1 );

	pass->setScissorRect( Graphics::Rect( x, height - ( y + h ), w, h ) );
}

/*
======================
idRenderBackendEacp::DrawView

RB_STD_DrawView. The sequence rather than the calls: fill the depth buffer, add
each light through the stencil, blend the passes that do not depend on a light,
fog. Three of the four are here; the stencil half of the second is 4d.3 and the
fourth is 4e, and each says so where it would be.
======================
*/
void idRenderBackendEacp::DrawView( void ) {
	if ( !pass ) {
		// No frame is open, so this draw came from outside GPUView::render - a
		// level load's own screen update, or a console command. Nothing to draw
		// into, and saying so is better than a pass that presents nothing.
		return;
	}

	drawSurf_t **	drawSurfs = (drawSurf_t **)&backEnd.viewDef->drawSurfs[0];
	const int		numDrawSurfs = backEnd.viewDef->numDrawSurfs;

	// What an interaction pass tests with once the depth fill has run: the
	// surface is already in the depth buffer at exactly its own depth, so a
	// light touches the fragments that survived and no others.
	backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL;

	BeginDrawingView();

	// backEnd.lightScale, which each light's colour is multiplied by. Its
	// sibling backEnd.overBright is always 1 here - tr.backEndRendererMaxLight
	// is 999 for BE_EACP as it is for BE_ARB2 - so RB_STD_LightScale, the
	// full-screen multiply that crutches up a backend whose blending range is
	// eight bits, can never do anything on this path and is not ported.
	RB_DetermineLightScale();

	FillDepthBuffer( drawSurfs, numDrawSurfs );

	DrawInteractions();

	const int	processed = DrawShaderPasses( drawSurfs, numDrawSurfs );

	// Fog and blend lights are 4e, and they belong here - between the two
	// halves of the shader passes, because a post-process surface reads the
	// frame with the fog already in it.

	if ( processed < numDrawSurfs ) {
		DrawShaderPasses( drawSurfs + processed, numDrawSurfs - processed );
	}
}

/*
======================
idRenderBackendEacp::BeginDrawingView

RB_BeginDrawingView, minus the part that is the pass's.

Doom 3 clears the depth and stencil buffers here for a 3D view and leaves them
alone for a 2D one; eacp clears both as a pass opens and there is one pass per
frame, so the clear has already happened by the time this runs. That is right
for one 3D view in a frame and wrong for two: a subview or a mirror is a second
3D view over the same buffers and would find the first view's depth in them.

Not fixed by opening a pass per view, which is the obvious answer and does not
work: this view's colour has to survive into the next pass, and with MSAA on -
which it is - the drawable's multisample texture is resolved and discarded as
each pass ends, so the second pass would load an undefined attachment. It is
step 4e's, with subviews and mirrors, and it is a real eacp question rather than
a Doom 3 one.
======================
*/
void idRenderBackendEacp::BeginDrawingView( void ) {
	depthRangeNear = 0.0f;
	depthRangeFar = 1.0f;

	SetViewport( backEnd.viewDef->viewport );

	backEnd.currentScissor = backEnd.viewDef->scissor;
	SetScissor( backEnd.currentScissor );

	if ( backEnd.viewDef->viewEntitys && !warnedSubviewPass
		 && backEnd.viewDef->isSubview ) {
		warnedSubviewPass = true;
		common->Warning( "eacp: a subview shares the frame's depth buffer with the "
						 "view it is drawn under, so it will occlude wrongly" );
	}

	// RB_BeginDrawingView's last act, and the reason SetCull is reached at all
	// below: the cached value has to disagree with whatever is asked for next.
	backEnd.glState.faceCulling = -1;
	SetCull( CT_FRONT_SIDED );
}

/*
======================
idRenderBackendEacp::SetSpace

The matrix a draw is transformed by, and the depth range it writes into.

On OpenGL these are three separate pieces of state left in the context -
glLoadMatrixf for the modelview, glLoadMatrixf again for the hacked projection,
glDepthRange for the range - and the two depth hacks are why they are one
function here: RB_EnterWeaponDepthHack and RB_EnterModelDepthHack each rebuild
the projection matrix *from the view's* and set a range to go with it, so the
matrix this hands the shader depends on which of them applies.

Their order is the GL path's exactly, which is to say the model hack wins: it
copies the view's projection afresh rather than modifying the weapon hack's, and
puts the range back to [0, 1] as it does so.

The hack is an argument rather than read off the space because a surface can
refuse the one its space asks for - a soft particle does - so the space changing
is not the whole of when this has to be redone.
======================
*/
void idRenderBackendEacp::SetSpace( const viewEntity_t *space, float modelDepthHack ) {
	if ( space == backEnd.currentSpace && modelDepthHack == appliedDepthHack ) {
		return;
	}

	float	projection[16];
	float	nearDepth = 0.0f;
	float	farDepth = 1.0f;

	memcpy( projection, backEnd.viewDef->projectionMatrix, sizeof( projection ) );

	if ( space->weaponDepthHack ) {
		projection[14] *= 0.25f;
		farDepth = 0.5f;
	}

	if ( modelDepthHack != 0.0f ) {
		memcpy( projection, backEnd.viewDef->projectionMatrix, sizeof( projection ) );
		projection[14] -= modelDepthHack;
		farDepth = 1.0f;
	}

	if ( nearDepth != depthRangeNear || farDepth != depthRangeFar ) {
		depthRangeNear = nearDepth;
		depthRangeFar = farDepth;
		SetViewport( backEnd.viewDef->viewport );
	}

	R_EacpModelViewProjection( space->modelViewMatrix, projection, modelViewProjection );

	backEnd.currentSpace = space;
	appliedDepthHack = modelDepthHack;
}

/*
======================
idRenderBackendEacp::FillDepthBuffer

RB_STD_FillDepthBuffer, which is also RB_RenderDrawSurfListWithFunction: the
walk and the function it calls are separate on OpenGL only because four
different passes share the walk, and this is the one of them that survives the
port intact.

Not here, and each is switched off by something this port controls:

  - the mirror clip plane, which is a second texture unit whose alpha texgen
    fails the alpha test behind the plane. It needs a subview to matter, and
    subviews are 4e.
  - the early depth capture (#3877), which feeds _currentDepthImage to the soft
    particle program. r_enableDepthCapture is -1 and r_useSoftParticles only
    reaches a BE_ARB2 backend, so nothing asks.
  - polygon offset for decals, which is eacp gap 6.
======================
*/
void idRenderBackendEacp::FillDepthBuffer( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	// A 2D view has nothing to occlude and writes no depth at all.
	if ( !backEnd.viewDef->viewEntitys ) {
		return;
	}

	if ( backEnd.viewDef->numClipPlanes && !warnedClipPlanes ) {
		warnedClipPlanes = true;
		common->Warning( "eacp: the mirror clip plane is not implemented, so a "
						 "subview will draw what is behind its plane" );
	}

	SelectTexture( 0 );

	backEnd.currentSpace = NULL;

	for ( int i = 0 ; i < numDrawSurfs ; i++ ) {
		const drawSurf_t *	surf = drawSurfs[i];

		SetSpace( surf->space, surf->space->modelDepthHack );

		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			SetScissor( backEnd.currentScissor );
		}

		FillDepthBufferSurface( surf );
	}

	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
}

/*
======================
idRenderBackendEacp::FillDepthBufferSurface

RB_T_FillDepthBuffer. An opaque surface is one draw of the white image in black;
a perforated one is a draw per alpha-tested stage, through the same texture and
the same texture matrix its ambient pass would use, so the holes in a grate are
holes in the depth buffer too.
======================
*/
void idRenderBackendEacp::FillDepthBufferSurface( const drawSurf_t *surf ) {
	const srfTriangles_t *	tri = surf->geo;
	const idMaterial *		shader = surf->material;

	if ( !shader->IsDrawn() ) {
		return;
	}

	// Some deforms disable themselves by setting numIndexes to 0.
	if ( !tri->numIndexes ) {
		return;
	}

	// A translucent surface neither writes depth nor tests against it.
	if ( shader->Coverage() == MC_TRANSLUCENT ) {
		return;
	}

	if ( !tri->ambientCache ) {
		common->Printf( "RB_T_FillDepthBuffer: !tri->ambientCache\n" );
		return;
	}

	const float *	regs = surf->shaderRegisters;

	// If every stage of the material is conditioned off, the surface is not
	// there at all this frame.
	int	stage;

	for ( stage = 0 ; stage < shader->GetNumStages() ; stage++ ) {
		if ( regs[ shader->GetStage( stage )->conditionRegister ] != 0 ) {
			break;
		}
	}

	if ( stage == shader->GetNumStages() ) {
		return;
	}

	eacpStage_t	draw;

	draw.cullType = backEnd.glState.faceCulling;
	draw.vertexColor = SVC_IGNORE;
	draw.textureMatrix = NULL;
	draw.alphaTest = false;
	draw.alphaTestRef = 0.0f;

	if ( shader->GetSort() == SS_SUBVIEW ) {
		// A subview's own surface - the mirror, the monitor - is drawn down by
		// the overbright factor rather than to black, because what will be
		// composited into it later has already been scaled up by it.
		draw.stateBits = GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS;

		const float	down = 1.0f / backEnd.overBright;

		draw.color = asFloat4( down, down, down, 1.0f );
	} else {
		draw.stateBits = GLS_DEPTHFUNC_LESS;
		draw.color = asFloat4( 0.0f, 0.0f, 0.0f, 1.0f );
	}

	// Once per surface rather than once per stage: a perforated material with
	// two alpha-tested stages is two draws over one piece of geometry.
	const idDrawVert *	vertices = (const idDrawVert *)vertexCache.Position( tri->ambientCache );

	drawVertices = &eacpRenderProgs.StreamVertices( vertices,
													(std::size_t)tri->numVerts * sizeof( idDrawVert ) );

	bool	drawSolid = ( shader->Coverage() == MC_OPAQUE );

	if ( shader->Coverage() == MC_PERFORATED ) {
		bool	didDraw = false;

		for ( stage = 0 ; stage < shader->GetNumStages() ; stage++ ) {
			const shaderStage_t *	pStage = shader->GetStage( stage );

			if ( !pStage->hasAlphaTest ) {
				continue;
			}

			if ( regs[ pStage->conditionRegister ] == 0 ) {
				continue;
			}

			// Having tried to draw an alpha-tested stage is what says the
			// surface is not solid, whether or not the stage itself drew.
			didDraw = true;

			draw.color[3] = regs[ pStage->color.registers[3] ];

			if ( draw.color[3] <= 0 ) {
				continue;
			}

			if ( pStage->texture.texgen != TG_EXPLICIT ) {
				if ( !warnedTexgen ) {
					warnedTexgen = true;
					common->Warning( "eacp: texgen %i is not implemented, so '%s' will not draw",
									 pStage->texture.texgen, shader->GetName() );
				}
				continue;
			}

			float	matrix[16];

			if ( pStage->texture.hasMatrix ) {
				RB_GetShaderTextureMatrix( regs, &pStage->texture, matrix );
				draw.textureMatrix = matrix;
			} else {
				draw.textureMatrix = NULL;
			}

			pStage->texture.image->Bind();

			draw.image = boundImages[0];
			draw.alphaTest = true;
			draw.alphaTestRef = regs[ pStage->alphaTestRegister ];

			DrawStage( tri, draw );
		}

		if ( !didDraw ) {
			drawSolid = true;
		}
	}

	if ( drawSolid ) {
		globalImages->whiteImage->Bind();

		draw.image = boundImages[0];
		draw.textureMatrix = NULL;
		draw.alphaTest = false;
		draw.color[3] = 1.0f;

		DrawStage( tri, draw );
	}
}

/*
================================================================================

	The lights.

	Step 4d.2. What the depth fill was for: every fragment is now in the depth
	buffer at exactly its own depth, so a light can be added at
	GLS_DEPTHFUNC_EQUAL and touch the surface that survived and nothing behind
	it.

	The decomposition is shared with the OpenGL backend and deliberately so.
	RB_CreateSingleDrawInteractions turns one surface under one light into the
	sequence of primitive (bump, diffuse, specular) interactions the program can
	draw, which is a hundred lines of material semantics - multi-stage lights,
	multi-layer surfaces, the nospecular parm, the colour registers - and none
	of it is about an API. It was written to be shared by the four backends
	Doom 3 shipped with, and this port is the first thing to take it up on that.

================================================================================
*/

/*
====================
R_EacpAmbientLightVector

An ambient light's direction, which is a constant because an ambient light has
none.

The ARB path expresses that by swapping one texture: the normalization cube map
that turns the interpolated vector to the light into a unit one becomes
_ambient, whose every texel is the same value, so the "direction to the light"
comes out the same everywhere on every surface. This backend has no cube map
(gap 5) and needs none - the whole point of the substitution is that the answer
does not depend on the lookup - so the constant is computed here and handed over
as a uniform.

It is read *back out of* R_AmbientNormalImage rather than taken from
tr.ambientLightVector directly, and the difference is not rounding. That
generator writes the vector's x into the channel a compressed normal map keeps x
in, which is alpha; the fragment program applies that swizzle to the bump map
and not to this one, so what the shader has always received is the texel's rgb
with 1.0 where x should be. Reproducing the vector faithfully means reproducing
that, because it is what the game has looked like since 2004.
====================
*/
static Float4 R_EacpAmbientLightVector( bool ambientLight ) {
	if ( !ambientLight ) {
		// w = 0: the shader keeps the direction it computed.
		return asFloat4( 0.0f, 0.0f, 0.0f, 0.0f );
	}

	const int	red = ( globalImages->image_useNormalCompression.GetInteger() == 1 ) ? 0 : 3;
	const int	alpha = ( red == 0 ) ? 3 : 0;

	byte	texel[4];

	texel[red] = (byte)( 255 * tr.ambientLightVector[0] );
	texel[1] = (byte)( 255 * tr.ambientLightVector[1] );
	texel[2] = (byte)( 255 * tr.ambientLightVector[2] );
	texel[alpha] = 255;

	// The MAD that decodes a normal map's [0, 1] into a direction's [-1, 1].
	return asFloat4( texel[0] / 255.0f * 2.0f - 1.0f,
					 texel[1] / 255.0f * 2.0f - 1.0f,
					 texel[2] / 255.0f * 2.0f - 1.0f,
					 1.0f );
}

/*
======================
idRenderBackendEacp::DrawInteractions

RB_ARB2_DrawInteractions: every light in the view, added to the frame.

Two of its parts are missing and both are step 4d.3's. There is no stencil
shadow pass, so a light lights everything inside its volume whether or not
something stands between - and there is therefore no stencil clear either, which
is the other half of the same step. What is here is the light itself.
======================
*/
void idRenderBackendEacp::DrawInteractions( void ) {
	// A 2D view has no lights, and the loop below would find none - but it also
	// has no depth buffer filled to test against, so saying so is clearer than
	// relying on the list being empty.
	if ( !backEnd.viewDef->viewEntitys ) {
		return;
	}

	for ( viewLight_t *vLight = backEnd.viewDef->viewLights ; vLight ; vLight = vLight->next ) {
		backEnd.vLight = vLight;

		// Both are 4e: a fog light is a volume of two blended passes and a
		// blend light a projected multiply, and neither goes through the
		// interaction program.
		if ( vLight->lightShader->IsFogLight() ) {
			continue;
		}

		if ( vLight->lightShader->IsBlendLight() ) {
			continue;
		}

		if ( !vLight->localInteractions && !vLight->globalInteractions
			 && !vLight->translucentInteractions ) {
			continue;
		}

		// 4d.3: RB_StencilShadowPass( vLight->globalShadows ) runs here and
		// again between the two lists below, with the stencil buffer cleared
		// over the light's scissor rectangle ahead of both. Until it does, a
		// shadow-casting light is drawn as if nothing cast one.
		if ( ( vLight->globalShadows || vLight->localShadows ) && !warnedShadows ) {
			warnedShadows = true;
			common->Warning( "eacp: the stencil shadow pass is not implemented, so a "
							 "light shines through whatever should be shadowing it" );
		}

		CreateDrawInteractions( vLight->localInteractions );
		CreateDrawInteractions( vLight->globalInteractions );

		// A translucent surface is never in the depth buffer the fill wrote, so
		// it cannot be added at EQUAL - and it is never stencil shadowed
		// either, which is why it comes after both shadow passes rather than
		// between them.
		if ( r_skipTranslucent.GetBool() ) {
			continue;
		}

		backEnd.depthFunc = GLS_DEPTHFUNC_LESS;
		CreateDrawInteractions( vLight->translucentInteractions );
		backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL;
	}
}

/*
======================
idRenderBackendEacp::CreateDrawInteractions

RB_ARB2_CreateDrawInteractions: one light's list of surfaces.

Everything the OpenGL version does to bind a program and enable five vertex
attribute arrays is gone - the program is looked up per draw and the attributes
are the vertex layout the shader pulled out of eacpDrawVert_t - and what is left
is the two things that really are per surface: where it is, and where its
vertices went.
======================
*/
void idRenderBackendEacp::CreateDrawInteractions( const drawSurf_t *surf ) {
	if ( !surf ) {
		return;
	}

	// Constant for every draw in this list, and read back off backEnd.glState
	// by DrawInteraction - which is how the state reaches a pipeline on this
	// backend, SetState having nothing to set.
	SetState( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | backEnd.depthFunc );

	for ( ; surf ; surf = surf->nextOnLight ) {
		if ( !surf->geo || !surf->geo->ambientCache ) {
			continue;
		}

		// The three things RB_CreateSingleDrawInteractions used to do with GL
		// calls of its own, and now leaves to whoever is driving it: the space,
		// the scissor and the depth hack. The last is the whole of SetSpace on
		// this backend - a hacked projection matrix and the depth range that
		// goes with it, rather than three pieces of context state - so there is
		// nothing here for an RB_EnterWeaponDepthHack to be, and nothing to
		// leave afterwards either.
		SetSpace( surf->space, surf->space->modelDepthHack );

		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			SetScissor( backEnd.currentScissor );
		}

		// Once per surface, however many primitive interactions it decomposes
		// into: a two-layer material under a two-stage light is four draws over
		// one piece of geometry.
		const idDrawVert *	vertices =
			(const idDrawVert *)vertexCache.Position( surf->geo->ambientCache );

		drawVertices = &eacpRenderProgs.StreamVertices(
			vertices, (std::size_t)surf->geo->numVerts * sizeof( idDrawVert ) );

		RB_CreateSingleDrawInteractions( surf, R_EacpDrawInteraction );
	}

	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
}

/*
======================
idRenderBackendEacp::DrawInteraction

RB_ARB2_DrawInteraction: one bump map, one diffuse map and one specular map,
under one stage of one light.

The OpenGL version is sixteen glProgramEnvParameter4fvARB calls against numbered
program environment slots and five texture binds, with two more textures bound
once per light list around it. Here every parameter has a name and the seven
textures are five, because the two the ARB program could not compute without -
the normalization cube map and the specular ramp - are arithmetic in a language
that has none of that shader's limits.
======================
*/
void idRenderBackendEacp::DrawInteraction( const drawInteraction_t *din ) {
	idEacpRenderProgs::interactionDraw_t	draw =
		eacpRenderProgs.InteractionDraw( din->bumpImage, din->lightFalloffImage,
										 din->lightImage, din->diffuseImage,
										 din->specularImage,
										 backEnd.glState.glStateBits,
										 backEnd.glState.faceCulling );

	if ( !draw.pipeline ) {
		return;
	}

	const idImage *	images[5] = { din->bumpImage, din->lightFalloffImage, din->lightImage,
								  din->diffuseImage, din->specularImage };
	GPU::Texture *	textures[5];

	for ( int i = 0 ; i < 5 ; i++ ) {
		textures[i] = images[i] ? TextureFor( images[i] ) : NULL;

		if ( !textures[i] ) {
			// An image the upload path turned away, exactly as in DrawStage -
			// and one more likely here, since a light's projected image is
			// often a .dds this build cannot read.
			if ( !warnedMissingTexture ) {
				warnedMissingTexture = true;
				common->Warning( "eacp: '%s' has no texture on the GPU, so a surface will not draw",
								 images[i] ? images[i]->imgName.c_str() : "(none)" );
			}
			return;
		}
	}

	idEacpInteractionProgram *	program = draw.program;

	program->modelViewProjection = asFloat4x4( modelViewProjection );

	program->localLightOrigin = asFloat4( din->localLightOrigin[0], din->localLightOrigin[1],
										  din->localLightOrigin[2], din->localLightOrigin[3] );
	program->localViewOrigin = asFloat4( din->localViewOrigin[0], din->localViewOrigin[1],
										 din->localViewOrigin[2], din->localViewOrigin[3] );

	// The four planes of the light's projection, in the surface's own
	// coordinates and with the light stage's texture matrix already baked in by
	// RB_BakeTextureMatrixIntoTexgen.
	const idVec4 *	project = din->lightProjection;

	program->lightProjectionS = asFloat4( project[0][0], project[0][1], project[0][2], project[0][3] );
	program->lightProjectionT = asFloat4( project[1][0], project[1][1], project[1][2], project[1][3] );
	program->lightProjectionQ = asFloat4( project[2][0], project[2][1], project[2][2], project[2][3] );
	program->lightFalloffS = asFloat4( project[3][0], project[3][1], project[3][2], project[3][3] );

	program->bumpMatrixS = asFloat4( din->bumpMatrix[0][0], din->bumpMatrix[0][1],
									 din->bumpMatrix[0][2], din->bumpMatrix[0][3] );
	program->bumpMatrixT = asFloat4( din->bumpMatrix[1][0], din->bumpMatrix[1][1],
									 din->bumpMatrix[1][2], din->bumpMatrix[1][3] );
	program->diffuseMatrixS = asFloat4( din->diffuseMatrix[0][0], din->diffuseMatrix[0][1],
										din->diffuseMatrix[0][2], din->diffuseMatrix[0][3] );
	program->diffuseMatrixT = asFloat4( din->diffuseMatrix[1][0], din->diffuseMatrix[1][1],
										din->diffuseMatrix[1][2], din->diffuseMatrix[1][3] );
	program->specularMatrixS = asFloat4( din->specularMatrix[0][0], din->specularMatrix[0][1],
										 din->specularMatrix[0][2], din->specularMatrix[0][3] );
	program->specularMatrixT = asFloat4( din->specularMatrix[1][0], din->specularMatrix[1][1],
										 din->specularMatrix[1][2], din->specularMatrix[1][3] );

	// The same (modulate, add) pair the generic material stage uses, and here it
	// is ±1 and 0 rather than a colour: the constant colour is not folded in,
	// because it is already the diffuse and specular uniforms below.
	switch ( din->vertexColor ) {
	case SVC_MODULATE:
		program->colorModulate = asFloat4( 1.0f, 1.0f, 1.0f, 1.0f );
		program->colorAdd = asFloat4( 0.0f, 0.0f, 0.0f, 0.0f );
		break;
	case SVC_INVERSE_MODULATE:
		program->colorModulate = asFloat4( -1.0f, -1.0f, -1.0f, -1.0f );
		program->colorAdd = asFloat4( 1.0f, 1.0f, 1.0f, 1.0f );
		break;
	default:
		program->colorModulate = asFloat4( 0.0f, 0.0f, 0.0f, 0.0f );
		program->colorAdd = asFloat4( 1.0f, 1.0f, 1.0f, 1.0f );
		break;
	}

	program->diffuseColor = asFloat4( din->diffuseColor[0], din->diffuseColor[1],
									  din->diffuseColor[2], din->diffuseColor[3] );
	program->specularColor = asFloat4( din->specularColor[0], din->specularColor[1],
									   din->specularColor[2], din->specularColor[3] );

	program->ambientLightVector = R_EacpAmbientLightVector( din->ambientLight != 0 );

	program->bumpImage = *textures[0];
	program->lightFalloffImage = *textures[1];
	program->lightImage = *textures[2];
	program->diffuseImage = *textures[3];
	program->specularImage = *textures[4];

	drawProgram = program;
	drawPipeline = draw.pipeline;

	RB_DrawElementsWithCounters( din->surf->geo );
}

/*
======================
idRenderBackendEacp::DrawShaderPasses

RB_STD_DrawShaderPasses. Two of its parts are not here and neither is a
shortcut: the ARB program environment, this backend having no ARB programs to
give one to, and the _currentRender copy, which the shared code already skips on
any backend that is not BE_ARB2 - so a post-process material samples a stale
_currentRender on this path exactly as it would on OpenGL's, and the fix is step
4e's render target rather than anything here.

Returns how far it got, which is not always the whole list: the post-process
surfaces are deliberately left for a second call, after the fog that has to be
in the frame before they read it.
======================
*/
int idRenderBackendEacp::DrawShaderPasses( drawSurf_t **drawSurfs, int numDrawSurfs ) {
	if ( numDrawSurfs < 1 ) {
		return numDrawSurfs;
	}

	// Only a 3D view obeys it - the console and the menus are drawn by this
	// same path and are not what r_skipAmbient is for.
	if ( backEnd.viewDef->viewEntitys && r_skipAmbient.GetBool() ) {
		return numDrawSurfs;
	}

	if ( drawSurfs[0]->material->GetSort() >= SS_POST_PROCESS ) {
		if ( r_skipPostProcess.GetBool() ) {
			return 0;
		}

		// Nothing to copy from: a 2D view never dumps the framebuffer even on
		// OpenGL, and a 3D view's dump is guarded by BE_ARB2 in the shared
		// code. Saying it has been copied is what keeps the loop below from
		// stopping at the first post-process surface, which is what OpenGL
		// does here for the same reason.
		backEnd.currentRenderCopied = true;
	}

	SelectTexture( 0 );

	int	i;

	backEnd.currentSpace = NULL;

	for ( i = 0 ; i < numDrawSurfs ; i++ ) {
		if ( drawSurfs[i]->material->SuppressInSubview() ) {
			continue;
		}

		if ( backEnd.viewDef->isXraySubview && drawSurfs[i]->space->entityDef ) {
			if ( drawSurfs[i]->space->entityDef->parms.xrayIndex != 2 ) {
				continue;
			}
		}

		if ( drawSurfs[i]->material->GetSort() >= SS_POST_PROCESS
			 && !backEnd.currentRenderCopied ) {
			break;
		}

		DrawSurfaceShaderPasses( drawSurfs[i] );
	}

	SetCull( CT_FRONT_SIDED );

	return i;
}

/*
======================
idRenderBackendEacp::DrawSurfaceShaderPasses

RB_STD_T_RenderShaderPasses. Everything above the stage loop is the surface's -
where it is, what it is clipped to, which way its triangles face and where its
vertices are - and everything inside it is one stage's material expression,
which is a program, a pipeline and five uniforms rather than a combiner.
======================
*/
void idRenderBackendEacp::DrawSurfaceShaderPasses( const drawSurf_t *surf ) {
	const srfTriangles_t *	tri = surf->geo;
	const idMaterial *		shader = surf->material;

	if ( !shader->HasAmbient() ) {
		return;
	}

	if ( shader->IsPortalSky() ) {
		return;
	}

	// #3878: a soft particle refuses the model depth hack, the older and
	// cruder way of softening the same edge. It refuses it on every backend,
	// including this one, where the soft particle program itself is behind
	// BE_ARB2 and so never runs.
	const bool	softParticle = ( surf->dsFlags & DSF_SOFT_PARTICLE ) != 0;

	SetSpace( surf->space, softParticle ? 0.0f : surf->space->modelDepthHack );

	if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
		backEnd.currentScissor = surf->scissorRect;
		SetScissor( backEnd.currentScissor );
	}

	// Some deforms disable themselves by setting numIndexes to 0.
	if ( !tri->numIndexes ) {
		return;
	}

	if ( !tri->ambientCache ) {
		common->Printf( "RB_T_RenderShaderPasses: !tri->ambientCache\n" );
		return;
	}

	const float *	regs = surf->shaderRegisters;

	SetCull( shader->GetCullType() );

	// Gap 6 (plan.md section 5): no depth bias, so a decal on a wall z-fights
	// instead of sitting on it. MF_POLYGONOFFSET and privatePolygonOffset are
	// both this, and both are silent here rather than warned about, because
	// what they produce is a picture that is slightly wrong rather than one
	// that is missing.

	// Doom 3 hands the GPU idDrawVert as it stands, and so does this: the
	// vertex cache holds system memory on this backend, so the whole surface's
	// vertices go into a streaming buffer once and every stage of it draws from
	// there. Once per surface rather than once per stage, because a material
	// with four stages is four draws over one piece of geometry.
	const idDrawVert *	vertices = (const idDrawVert *)vertexCache.Position( tri->ambientCache );

	drawVertices = &eacpRenderProgs.StreamVertices( vertices,
													(std::size_t)tri->numVerts * sizeof( idDrawVert ) );

	for ( int stage = 0 ; stage < shader->GetNumStages() ; stage++ ) {
		const shaderStage_t *	pStage = shader->GetStage( stage );

		if ( regs[ pStage->conditionRegister ] == 0 ) {
			continue;
		}

		if ( pStage->lighting != SL_AMBIENT ) {
			continue;
		}

		// ( GL_ZERO, GL_ONE ) leaves the destination exactly as it was; some
		// alpha masks are written that way.
		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
			 == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE ) ) {
			continue;
		}

		// A new-style stage is a pair of hand-written ARB programs, and the
		// OpenGL path skips them on any backend that is not BE_ARB2 - which
		// this is not. The eacp answer to them is a program per newShaderStage,
		// and it is nobody's step yet.
		if ( pStage->newStage ) {
			continue;
		}

		// Only TG_EXPLICIT here. The screen-space and cube texgens need
		// _currentRender and cube maps respectively (step 4e, and gap 5), and a
		// stage drawn with the wrong coordinates looks like a rendering bug
		// rather than a missing feature - so it is skipped and said once.
		if ( pStage->texture.texgen != TG_EXPLICIT ) {
			if ( !warnedTexgen ) {
				warnedTexgen = true;
				common->Warning( "eacp: texgen %i is not implemented, so '%s' will not draw",
								 pStage->texture.texgen, shader->GetName() );
			}
			continue;
		}

		const Float4	color = asFloat4( regs[ pStage->color.registers[0] ],
										  regs[ pStage->color.registers[1] ],
										  regs[ pStage->color.registers[2] ],
										  regs[ pStage->color.registers[3] ] );

		// An add of black adds nothing, and a blend at zero alpha blends
		// nothing.
		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
			 == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE )
			 && color[0] <= 0 && color[1] <= 0 && color[2] <= 0 ) {
			continue;
		}

		if ( ( pStage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
			 == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
			 && color[3] <= 0 ) {
			continue;
		}

		RB_BindVariableStageImage( &pStage->texture, regs );

		eacpStage_t	draw;

		draw.image = boundImages[0];
		draw.cullType = backEnd.glState.faceCulling;
		draw.vertexColor = pStage->vertexColor;
		draw.color = color;
		draw.alphaTest = false;
		draw.alphaTestRef = 0.0f;

		float	matrix[16];

		if ( pStage->texture.hasMatrix ) {
			RB_GetShaderTextureMatrix( regs, &pStage->texture, matrix );
			draw.textureMatrix = matrix;
		} else {
			draw.textureMatrix = NULL;
		}

		SetState( pStage->drawStateBits );

		draw.stateBits = backEnd.glState.glStateBits;

		// A 2D view runs with the depth test off, which RB_BeginDrawingView
		// does with glDisable( GL_DEPTH_TEST ) rather than through the state
		// bitfield - so the bits say nothing about it and the pipeline has to
		// be told. Always with no write is exactly what a disabled depth test
		// is, and it keeps every 2D draw on one pipeline shape rather than one
		// per material's idea of a depth function.
		//
		// A 3D view is the opposite: the depth buffer is already filled and the
		// bits are the material's own, which is what decides whether a stage
		// sits on the surface the depth fill put there or behind it.
		if ( !backEnd.viewDef->viewEntitys ) {
			draw.stateBits |= GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK;
		}

		DrawStage( tri, draw );
	}

	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
}

/*
======================
idRenderBackendEacp::DrawStage

One draw. What OpenGL says with a matrix stack, a texture matrix, glColor, a
colour array, GL_COMBINE_ARB and six qglTexEnvi calls is a program, a pipeline
and five uniforms - and the program and the pipeline are looked up rather than
built, both being cached on exactly the state that went into them.

The geometry is not an argument: drawVertices is already this surface's, set
once by the caller because a material with four stages is four draws over one
piece of geometry.
======================
*/
void idRenderBackendEacp::DrawStage( const srfTriangles_t *tri, const eacpStage_t &stage ) {
	GPU::Texture *	texture = stage.image ? TextureFor( stage.image ) : NULL;

	if ( !texture ) {
		// An image the upload path turned away - a .dds this build cannot read,
		// or a cube map. UploadImageLevel has already said which and why; this
		// is the draw that would have used it.
		if ( !warnedMissingTexture ) {
			warnedMissingTexture = true;
			common->Warning( "eacp: '%s' has no texture on the GPU, so a surface will not draw",
							 stage.image ? stage.image->imgName.c_str() : "(none)" );
		}
		return;
	}

	idEacpRenderProgs::stageDraw_t	draw =
		eacpRenderProgs.StageDraw( stage.image, stage.stateBits, stage.cullType,
								   stage.alphaTest );

	if ( !draw.pipeline ) {
		return;
	}

	draw.program->modelViewProjection = asFloat4x4( modelViewProjection );

	// The texture matrix, as the two rows of it that are not the identity.
	if ( stage.textureMatrix ) {
		const float *	matrix = stage.textureMatrix;

		draw.program->textureMatrixS = asFloat4( matrix[0], matrix[4], matrix[12], 0.0f );
		draw.program->textureMatrixT = asFloat4( matrix[1], matrix[5], matrix[13], 0.0f );
	} else {
		draw.program->textureMatrixS = asFloat4( 1.0f, 0.0f, 0.0f, 0.0f );
		draw.program->textureMatrixT = asFloat4( 0.0f, 1.0f, 0.0f, 0.0f );
	}

	// The three stageVertexColor_t modes as (modulate, add). OpenGL needs a
	// combiner and a second texture unit bound to the white image to say the
	// same thing; here they are two uniforms and the program is one.
	//
	// SVC_INVERSE_MODULATE inverts the alpha channel along with the three
	// colour ones, which the fixed-function path does not: it sets
	// GL_COMBINE_RGB alone and leaves alpha on the default modulate. That is a
	// deliberate simplification and the one BFG's own port makes - the mode
	// exists for cross-blended terrain, where the alpha is 1 either way, and no
	// material in the demo reaches it at all.
	const Float4 &	color = stage.color;
	const Float4	black = asFloat4( 0.0f, 0.0f, 0.0f, 0.0f );

	switch ( stage.vertexColor ) {
	case SVC_MODULATE:
		draw.program->colorModulate = color;
		draw.program->colorAdd = black;
		break;
	case SVC_INVERSE_MODULATE:
		draw.program->colorModulate = asFloat4( -color[0], -color[1], -color[2], -color[3] );
		draw.program->colorAdd = color;
		break;
	default:
		draw.program->colorModulate = black;
		draw.program->colorAdd = color;
		break;
	}

	draw.program->alphaTestRef = stage.alphaTestRef;
	draw.program->image = *texture;

	drawProgram = draw.program;
	drawPipeline = draw.pipeline;

	// Through the counter wrapper rather than around it: r_showPrimitives and
	// the renderer's own performance counters are read from the same place on
	// both backends, and DrawIndexed is the seam.
	RB_DrawElementsWithCounters( tri );
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
	all of it at once rather than set one field at a time - so what turns these
	into something the GPU can be told is idEacpRenderProgs::StageDraw, at the
	moment a draw is issued, and it looks the answer up in a cache keyed on
	exactly these fields.
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

/*
======================
idRenderBackendEacp::DrawIndexed

Every indexed draw in the renderer arrives here, and what it draws *with* is
whatever the path above it prepared - which on OpenGL is context state and here
is the three drawProgram / drawPipeline / drawVertices fields. A draw from a
path this backend has not written yet finds them null and is a no-op, which is
the difference between an unfinished feature and a draw against whatever the
last one left bound.
======================
*/
void idRenderBackendEacp::DrawIndexed( const srfTriangles_t *tri, int numIndexes ) {
	if ( !pass || !drawProgram || !drawPipeline || !drawVertices ) {
		return;
	}

	if ( numIndexes < 1 ) {
		return;
	}

	if ( r_singleTriangle.GetBool() ) {
		numIndexes = 3;
	}

	// The index cache holds system memory here for the same reason the vertex
	// cache does - this backend generates no buffer objects - so both branches
	// end in a real pointer and the difference is only where it came from.
	const glIndex_t *	indexes = ( tri->indexCache && r_useIndexBuffers.GetBool() )
		? (const glIndex_t *)vertexCache.Position( tri->indexCache )
		: tri->indexes;

	if ( !indexes ) {
		return;
	}

	const GPU::Buffer &	buffer =
		eacpRenderProgs.StreamIndices( indexes, (std::size_t)numIndexes * sizeof( glIndex_t ) );

	pass->setPipeline( *drawPipeline );
	pass->setVertexBuffer( *drawVertices );
	pass->setUniforms( *drawProgram );

	drawProgram->bindTextures( *pass );

	pass->drawIndexed( buffer, numIndexes, GPU::IndexFormat::UInt32 );
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

/*
====================
idRenderBackendEacp::ReadPixels

The same missing thing CopyFramebufferToImage is missing, one step further on:
there is no back buffer to read, only a drawable that has already been handed
back to the compositor by the time anyone asks. It needs the app-owned render
target of step 4e, and until then the honest answer is no.

Which is not a cosmetic gap and was found by a crash rather than by reading:
the game itself calls this, without being asked and at map load, to take the
camera shot its objective screen shows - idObjective::Event_CamShot.
====================
*/
bool idRenderBackendEacp::ReadPixels( int x, int y, int width, int height, byte *rgb ) {
	if ( !warnedReadPixels ) {
		warnedReadPixels = true;
		common->Warning( "eacp: reading the frame back is not implemented yet, so the "
						 "objective camera shots will be missing" );
	}

	return false;
}
