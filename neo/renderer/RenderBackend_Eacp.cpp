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
	program and the stencil shadow pass. All three are in, so a 3D view occludes
	correctly, is lit by every light in it, and is shadowed by whatever stands
	between the two.

===============================================================================
*/

using namespace eacp;

// Declared in RenderSystem_init.cpp and not in tr_local.h, the same way
// draw_common.cpp reaches it. r_useStencilOpSeparate is its neighbour there and
// is deliberately not here: both of eacp's backends carry per-face stencil state
// outright, so there is no one-face-at-a-time path for it to choose between.
extern idCVar r_useCarmacksReverse;

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
	virtual bool	ReadPixels( int x, int y, int width, int height, byte *rgb,
								bool presented );

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

		// The subview's near clip plane in this surface's own coordinates, or
		// NULL for every draw outside one - which is every draw but a mirror's
		// depth fill. Read only where alphaTest is set, exactly as Doom 3 reads
		// its own: the notch it clips with modulates an alpha nothing tests
		// unless the surface is perforated.
		const float *		clipPlane;

		// texgen_t, and what decides which of the four programs draws this
		// stage. TG_EXPLICIT is the surface's own (s, t) and everything below
		// is unread.
		int					texgen;

		// The material's bump map, on a TG_REFLECT_CUBE stage whose material has
		// one - which is the whole of what tells bumpyEnvironment.vfp from
		// environment.vfp, in RB_PrepareStageTexturing and here.
		const idImage *		bumpImage;

		// R_WobbleskyTransform's 3x3 for a TG_WOBBLESKY_CUBE stage, and NULL for
		// TG_SKYBOX_CUBE - which the program reads as the identity, that being
		// exactly what the plain sky's generator does.
		const float *		texgenMatrix;
	};

	// The texture the frame is composed into, made on the first pass and
	// remade when the engine's idea of the screen size changes.
	bool			EnsureFrameTarget( void );

	// The pass every draw goes into: one per 3D view, because a pass is the
	// only thing that can clear the depth and stencil buffers and that is what
	// RB_BeginDrawingView asks for per view. See BeginDrawingView.
	void			BeginPass( bool clearColor,
							   GPU::DepthAction depthAction = GPU::DepthAction::Keep );
	void			EndPass( void );

	// The same pass interrupted and put back, which is what copying out of the
	// frame target costs - a texture cannot be sampled by the pass rendering
	// into it. Suspend says whether there was one to put back.
	bool			SuspendPass( void );
	void			ResumePass( void );

	// And the pass that puts the finished one on the screen.
	void			PresentFrameTarget( void );

	// One rectangle of the frame target onto one rectangle of an image, which
	// is what a copy is on a backend that has no copy. Both are in pixels; the
	// source is in Doom 3's coordinates, measured up from the bottom, and the
	// destination in the image's own rows, which run down from the top.
	void			CopyFrameRegion( GPU::RenderPass &into,
									 const idEacpRenderProgs::blitDraw_t &draw,
									 int srcX, int srcY, int srcWidth, int srcHeight,
									 int dstX, int dstY, int dstWidth, int dstHeight );

	// RB_BeginDrawingView: the pass this view is drawn in, where on the target
	// it lands, what part of it may be written, and the state it starts from.
	// False if the view has no pass to draw into, which is the one thing here
	// that can fail - a 3D view after another opens one of its own.
	bool			BeginDrawingView( void );
	void			SetViewport( const idScreenRect &rect );
	void			SetScissor( const idScreenRect &rect );

	// The cull mode a draw in this view is compiled with, which is the one it
	// asked for unless the view is mirrored. GL_Cull does this per draw, by
	// picking the glCullFace enum it hands the context; here the cull mode is
	// part of the pipeline, so the flip has to happen before the pipeline is
	// looked up - which also puts it in the cache key, so a mirrored draw and
	// an unmirrored one of the same material get the two pipelines they need.
	int				ViewCull( int cullType ) const;

	// The matrix a draw is transformed by, rebuilt when the surface's space or
	// its depth hack changes. This is also where both depth hacks land, each of
	// them being a modified projection matrix and a depth range.
	void			SetSpace( const viewEntity_t *space, float modelDepthHack );

	// RB_STD_FillDepthBuffer and RB_T_FillDepthBuffer, rewritten. What every
	// light after them tests against: the surface at its own depth, so an
	// interaction pass can run at GLS_DEPTHFUNC_EQUAL and touch only the
	// fragments that survived.
	void			FillDepthBuffer( drawSurf_t **drawSurfs, int numDrawSurfs );
	void			FillDepthBufferSurface( const drawSurf_t *surf, const float *clipPlane );

	// RB_ARB2_DrawInteractions, RB_ARB2_CreateDrawInteractions and
	// RB_ARB2_DrawInteraction. The light half of a view: every light in turn,
	// each of its surfaces decomposed into the primitive interactions
	// RB_CreateSingleDrawInteractions produces, added to the frame.
	void			DrawInteractions( void );
	void			CreateDrawInteractions( const drawSurf_t *surf );

	// RB_StencilShadowPass and RB_T_Shadow, rewritten. What stands between a
	// light and a surface, counted into the stencil buffer so that the
	// interactions after it can be masked by the count.
	void			StencilShadowPass( const drawSurf_t *drawSurfs );
	void			ShadowSurface( const drawSurf_t *surf );

	// The qglClear( GL_STENCIL_BUFFER_BIT ) each light does before its own
	// volumes are counted, which a pass that has already begun cannot do - so
	// it is a quad drawn over the light's scissor rectangle instead.
	void			ClearStencil( void );

	// What RB_FogPass works out once for the light and RB_T_BasicFog then turns
	// into two texture coordinates per space. The three planes are in *world*
	// coordinates here, exactly as draw_common.cpp's file-static fogPlanes[] is
	// - a fog light's planes are the view's rather than any surface's, so they
	// are computed once and transformed into each space in turn.
	struct eacpFog_t {
		idPlane			density;	// fogPlanes[0]: distance through the fog
		idPlane			enterS;		// fogPlanes[3]: the viewer's height, a constant
		idPlane			enterT;		// fogPlanes[2]: the fragment's height
		Float4			color;
	};

	// What RB_BlendLight works out once per stage of the light material. The
	// projection planes are not here because they are the light's and are the
	// same for every stage - what changes per stage is the image, the blend and
	// the colour, plus the texture matrix that has to be folded into the planes
	// after they reach a surface's space.
	struct eacpBlendLight_t {
		const idImage *	image;			// the stage's projected image
		const idImage *	falloff;		// the light's, so constant over the stages
		int				stateBits;
		Float4			color;
		bool			hasMatrix;		// in backEnd.lightTextureMatrix, as
										// R_SetDrawInteraction leaves it
	};

	// RB_STD_FogAllLights and the two passes it dispatches to, rewritten. The
	// one part of a view that is drawn as a *volume* rather than as a surface,
	// and the reason DrawInteractions skips both light kinds with a continue.
	void			FogAllLights( void );
	void			FogPass( const drawSurf_t *drawSurfs, const drawSurf_t *drawSurfs2 );
	void			FogChain( const drawSurf_t *chain, const eacpFog_t &fog );
	void			FogSurface( const drawSurf_t *surf, const eacpFog_t &fog,
								const idPlane local[3] );

	void			BlendLight( const drawSurf_t *drawSurfs, const drawSurf_t *drawSurfs2 );
	void			BlendLightChain( const drawSurf_t *chain, const eacpBlendLight_t &light );
	void			BlendLightSurface( const drawSurf_t *surf, const eacpBlendLight_t &light,
									   const idPlane local[4] );

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

	// The same for a stage whose texture coordinate is generated rather than
	// read - RB_PrepareStageTexturing's business, which on OpenGL is a texgen
	// enable, a texcoord pointer or a pair of ARB programs and here is a
	// program of its own. One function per program; DrawSurfaceShaderPasses
	// picks between them on the texgen.
	void			DrawCubeStage( const srfTriangles_t *tri, const eacpStage_t &stage,
								   eacpCubeTexgen_t texgen );
	void			DrawBumpyReflectStage( const srfTriangles_t *tri,
										   const eacpStage_t &stage );
	void			DrawScreenStage( const srfTriangles_t *tri, const eacpStage_t &stage );

	// The eye in the space currently being drawn, which three of the four
	// texgens need and which is R_SkyboxTexGen's localViewOrigin and
	// RB_SetProgramEnvironmentSpace's program.env[5] - the same value under two
	// names, because on OpenGL one is computed in the frontend and the other in
	// the backend.
	Float4			LocalViewOrigin( void ) const;

	// stageVertexColor_t as the (modulate, add) pair every one of these programs
	// takes. Static because it reads nothing but the stage; `combiner` is what
	// separates the fixed-function texgens from the two ARB ones and the
	// definition says why.
	static void		StageColor( const eacpStage_t &stage, bool combiner,
								eacp::GPU::Uniform<eacp::GPU::Float4> &modulate,
								eacp::GPU::Uniform<eacp::GPU::Float4> &add );

	// The texture an idImage carries, checked against the shape the program that
	// is about to sample it declared. Nothing on either backend reports a cube
	// bound where a 2D texture was declared or the other way round, so this is
	// where a material that lost its cube map becomes a warning and a skipped
	// draw rather than a black surface.
	const eacp::GPU::Texture *
					TextureForStage( const idImage *image, bool wantCube );

	// The texture an idImage carries, created on the first upload and
	// destroyed by FreeImage. Held by pointer in idImage::backendTexture,
	// because GPU::Texture has no empty state to default-construct - and by a
	// void * one, because that field is in a header both backends compile.
	// ReplaceTexture is where that raw field is owned on both sides of the
	// swap; nothing else touches it.
	static GPU::Texture *	TextureFor( const idImage *image );
	static void				ReplaceTexture( idImage *image,
											OwningPointer<GPU::Texture> texture );

	// The pass, and it is a pointer because a RenderPass cannot be anything
	// else: it has no default state and no move, so the only way to hold one
	// past the expression that opened it is to build it where it will live.
	// Owning, so that ending the pass is dropping it rather than remembering to
	// delete it.
	OwningPointer<GPU::RenderPass>	pass;

	// Where the frame is drawn, which is no longer the thing that is presented.
	// A pointer for the same reason: GPU::Texture owns native objects and has
	// no empty state to default-construct.
	OwningPointer<GPU::Texture>		frameTarget;
	int								frameTargetWidth;
	int								frameTargetHeight;

	// What the open pass was last told to rasterize into, and what the
	// suspended one was told before it closed. Pass state on eacp - a new pass
	// starts on the whole target - and Doom 3 sets both once per view rather
	// than once per draw, so a pass that came back without them would draw the
	// rest of its view over the whole frame.
	Graphics::Rect		appliedViewport;
	Graphics::Rect		appliedScissor;
	bool				hasViewport;
	bool				hasScissor;

	Graphics::Rect		suspendedViewport;
	Graphics::Rect		suspendedScissor;
	bool				suspendedHasViewport;
	bool				suspendedHasScissor;

	// Whether a 3D view has already been drawn into the open pass, and so
	// whether the next one has to open a pass of its own. It is the depth
	// buffer that is being tracked: a 2D view never writes it, so a 2D view
	// leaves the answer alone, and a suspended pass comes back with the same
	// buffer it left, so a resume leaves it alone too.
	bool				passHasWorldView;

	// The image on each texture unit, which is what a draw binds. The GL
	// backend has no equivalent because GL remembers this itself.
	idImage *			boundImages[MAX_MULTITEXTURE_UNITS];

	// The six faces of the cube currently being uploaded, gathered because
	// GenerateCubeImage hands them over one at a time and eacp takes them all at
	// once. A member rather than a local, since it has to survive between the
	// six calls; kept rather than freed, since the next cube is almost always
	// the same size as this one and a level load uploads a dozen of them.
	//
	// cubeFacesFilled is a bit per face, so a loader that gave up half way
	// through leaves the image with no texture rather than with a cube whose
	// missing faces hold the last one's pixels.
	idList<byte>		cubeFaces;
	int					cubeFaceBytes;
	int					cubeFacesFilled;

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

	// How the light being drawn reads the stencil buffer: the mask, if its
	// shadow volumes have just been counted into it, and nothing if it has
	// none. On OpenGL this is a glStencilFunc left in the context between the
	// shadow pass and the interactions; here it is part of the pipeline each
	// interaction is drawn through, so it has to be carried to the draw.
	eacpStencil_t		lightStencil;

	// Whether the shader passes being drawn are the post-process half, which
	// on OpenGL is the isPostProcess RB_STD_DrawShaderPasses hands to
	// RB_SetProgramEnvironment and here is read by the draws that set the gamma
	// uniform.
	bool				postProcessPass;

	// One warning per unimplemented path rather than one per frame, which is
	// the difference between a note and an unusable console.
	bool				warnedCopyFramebuffer;
	bool				warnedCompressed;
	bool				warnedExternalFormat;
	bool				warnedTexgen;
	bool				warnedMissingTexture;
	bool				warnedCubeShape;
	bool				warnedReadPixels;
	bool				warnedShowShadows;
	bool				warnedDepthPassShadows;
	bool				warnedShadowVertexProgram;
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
	frameTargetWidth = 0;
	frameTargetHeight = 0;
	hasViewport = false;
	hasScissor = false;
	suspendedHasViewport = false;
	suspendedHasScissor = false;
	passHasWorldView = false;
	cubeFaceBytes = 0;
	cubeFacesFilled = 0;
	memset( boundImages, 0, sizeof( boundImages ) );
	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
	memset( modelViewProjection, 0, sizeof( modelViewProjection ) );
	depthRangeNear = 0.0f;
	depthRangeFar = 1.0f;
	appliedDepthHack = 0.0f;
	lightStencil = ES_IGNORE;
	postProcessPass = false;
	warnedCopyFramebuffer = false;
	warnedCompressed = false;
	warnedExternalFormat = false;
	warnedTexgen = false;
	warnedMissingTexture = false;
	warnedCubeShape = false;
	warnedReadPixels = false;
	warnedShowShadows = false;
	warnedDepthPassShadows = false;
	warnedShadowVertexProgram = false;
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

	// Gap 5, closed: eacp grew cube textures, so a skybox and a reflection have
	// something to sample. The normalization cube map that was the third user is
	// gone rather than served - step 4d.2 made it arithmetic - so what this
	// switches on is `cameraCubeMap` and `cubeMap` in a material, and the four
	// cube texgens over them.
	glConfig.cubeMapAvailable = true;

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

	// While there is still a device to hand it back to - this backend is a
	// static, and its destructor runs long after the device has gone.
	frameTarget.reset();
	frameTargetWidth = 0;
	frameTargetHeight = 0;

	// What the content actually cost, rather than what plan.md section 4.3
	// sized it at: two numbers, at the one moment the whole run is known.
	common->Printf( "eacp: %i programs and %i pipelines compiled\n",
					eacpRenderProgs.NumPrograms(), eacpRenderProgs.NumPipelines() );

	eacpRenderProgs.Shutdown();
}

/*
======================
idRenderBackendEacp::EnsureFrameTarget

The texture the frame is composed into, which since step 4e is where the
renderer draws rather than the drawable.

**The size is the engine's, not the window's.** glConfig.vidWidth is what every
viewport, scissor and screen-space coordinate in the renderer is measured
against, so a target of any other size would put the picture somewhere the
renderer does not think it is. The blit then maps that rectangle onto whatever
the drawable happens to be, which is also what makes a window resize scale the
frame rather than corrupt it - GLimp_SetScreenParms refuses to resize (gap 8),
so the two really can differ.

BGRA8Unorm because that is the drawable's format and the pipelines are compiled
against one number for both; stencil because the shadow pass needs the plane,
and asking for it brings the depth buffer the world needs with it.
======================
*/
bool idRenderBackendEacp::EnsureFrameTarget( void ) {
	if ( glConfig.vidWidth < 1 || glConfig.vidHeight < 1 ) {
		return false;
	}

	if ( frameTarget && frameTargetWidth == glConfig.vidWidth
		 && frameTargetHeight == glConfig.vidHeight ) {
		return true;
	}

	frameTarget.reset();

	GPU::TextureDescriptor	descriptor;

	descriptor.width = glConfig.vidWidth;
	descriptor.height = glConfig.vidHeight;
	descriptor.format = GPU::TextureFormat::BGRA8Unorm;
	descriptor.renderTarget = true;
	descriptor.stencil = true;

	frameTarget.create( GPU::Device::shared(), descriptor, (const void *)NULL );

	if ( !frameTarget->isValid() || !frameTarget->isRenderTarget() ) {
		common->Warning( "eacp: no %ix%i render target, so nothing can be drawn",
						 glConfig.vidWidth, glConfig.vidHeight );
		frameTarget.reset();
		return false;
	}

	frameTargetWidth = glConfig.vidWidth;
	frameTargetHeight = glConfig.vidHeight;

	return true;
}

/*
======================
idRenderBackendEacp::BeginPass / EndPass

One eacp pass per Doom 3 3D view, and the mapping is not arbitrary: a pass
clears depth and stencil as it opens and can be told whether to clear colour,
which is exactly what RB_BeginDrawingView asks for - the depth buffer and the
stencil buffer emptied for this view, the colour left where the views before it
put it. BeginDrawingView is where the per-view part of that is decided.

Into the render target rather than the drawable, which is step 4e's whole
change and is what a second pass on one frame needs: a texture target is stored
and can be loaded back, where a multisampled drawable resolves and keeps nothing
(§5, gap 18).

**Every pass opened here keeps its depth and stencil planes on the way out**,
which is DepthAction::Keep and is what SuspendPass below needs: a copy has to
close the pass, and the pass that takes over from it can only load what the one
before it was told to store. Whether a frame is going to copy is not knowable
when its pass opens - the command that asks arrives later - so this is paid
always rather than predicted. What it costs is the target's depth-stencil buffer
written out to memory once per pass instead of being discarded in tile memory,
which is eight bytes a pixel on Metal and nothing at all on D3D12, where the
buffer is a resource that keeps what was written to it either way.
======================
*/
void idRenderBackendEacp::BeginPass( bool clearColor, GPU::DepthAction depthAction ) {
	if ( pass ) {
		return;
	}

	if ( !eacpFrame ) {
		return;
	}

	if ( !EnsureFrameTarget() ) {
		return;
	}

	GPU::RenderPassDescriptor	descriptor;

	descriptor.clear = clearColor;
	descriptor.depthAction = depthAction;

	// What Doom 3 clears the stencil buffer to, and it is deliberately not
	// zero: a shadow volume's count goes down as well as up, and the buffer is
	// unsigned, so the algorithm starts half way up its range.
	descriptor.clearStencil = (unsigned char)( 1 << ( glConfig.stencilBits - 1 ) );

	// Built where it will live rather than moved into place, which is the one
	// thing a RenderPass allows: beginPass returns a prvalue, so this
	// constructs it in the allocation and never copies or moves it.
	pass.reset( new GPU::RenderPass( eacpFrame->beginPass( *frameTarget, descriptor ) ) );

	// A pass that cleared the depth buffer is a pass no view has drawn into
	// yet; one that resumed is the same pass as before and keeps the answer it
	// already had.
	if ( depthAction != GPU::DepthAction::Resume ) {
		passHasWorldView = false;
	}

	// The one value every stencil comparison in the frame is against, and the
	// one StencilOp::Replace writes - which is why it can be said once here
	// rather than per draw. Doom 3 passes 128 to every glStencilFunc that
	// matters and 1 to the one that does not: RB_StencilShadowPass sets
	// GL_ALWAYS while the count is being taken, where the reference is not
	// read at all.
	pass->setStencilReference( (unsigned int)descriptor.clearStencil );
}

void idRenderBackendEacp::EndPass( void ) {
	if ( !pass ) {
		return;
	}

	pass->end();
	pass.reset();

	hasViewport = false;
	hasScissor = false;
}

/*
======================
idRenderBackendEacp::SuspendPass / ResumePass

The pass interrupted and put back, which is what a copy out of the frame target
costs: a texture cannot be sampled by the pass rendering into it, so the copy
has to happen between two passes rather than inside one.

**What comes back is the pass, not a new one.** The colour is loaded rather than
cleared, the depth and stencil planes are the ones the suspended pass stored
(DepthAction::Resume, and eacp gap 22 is what closing that took), and the
viewport and the scissor are re-sent - those being pass state on eacp, which a
new pass resets, and Doom 3 setting them once per view rather than once per
draw. Everything else a draw needs is an argument to the draw on this backend
and so was never the pass's to lose.

Suspend answers whether there was a pass at all, because a copy asked for
outside one - the level load's own screen update, a console command - has
nothing to put back.
======================
*/
bool idRenderBackendEacp::SuspendPass( void ) {
	if ( !pass ) {
		return false;
	}

	const Graphics::Rect	viewport = appliedViewport;
	const Graphics::Rect	scissor = appliedScissor;
	const bool				hadViewport = hasViewport;
	const bool				hadScissor = hasScissor;

	EndPass();

	suspendedViewport = viewport;
	suspendedScissor = scissor;
	suspendedHasViewport = hadViewport;
	suspendedHasScissor = hadScissor;

	return true;
}

void idRenderBackendEacp::ResumePass( void ) {
	BeginPass( false, GPU::DepthAction::Resume );

	if ( !pass ) {
		return;
	}

	if ( suspendedHasViewport ) {
		pass->setViewport( suspendedViewport, depthRangeNear, depthRangeFar );
		appliedViewport = suspendedViewport;
		hasViewport = true;
	}

	if ( suspendedHasScissor ) {
		pass->setScissorRect( suspendedScissor );
		appliedScissor = suspendedScissor;
		hasScissor = true;
	}
}

/*
======================
idRenderBackendEacp::PresentFrameTarget

The frame onto the screen: a second pass on the same frame, over the drawable,
drawing the target the first one composed into.

**It is a pass on the same frame rather than a frame of its own**, which is what
makes it legal to sample here what was written there: passes on one frame are
ordered by the queue, so neither backend needs a fence to say the target is
finished. What is *not* legal is sampling a texture from the pass rendering into
it, which is why this cannot be folded into the pass above and why EndPass has
to have run before it.

The quad is written with the target's row 0 at the top of the screen: clip space
has y up and a texture's rows start at the top, so the corner at y = +1 is the
one that samples t = 0.
======================
*/
void idRenderBackendEacp::PresentFrameTarget( void ) {
	if ( !eacpFrame || !frameTarget ) {
		return;
	}

	idEacpRenderProgs::blitDraw_t	draw = eacpRenderProgs.BlitDraw();

	if ( !draw.pipeline ) {
		return;
	}

	static const eacpBlitVert_t	quad[6] = {
		{ { -1.0f,  1.0f }, { 0.0f, 0.0f } },
		{ { -1.0f, -1.0f }, { 0.0f, 1.0f } },
		{ {  1.0f, -1.0f }, { 1.0f, 1.0f } },
		{ { -1.0f,  1.0f }, { 0.0f, 0.0f } },
		{ {  1.0f, -1.0f }, { 1.0f, 1.0f } },
		{ {  1.0f,  1.0f }, { 1.0f, 0.0f } },
	};

	draw.program->image = *frameTarget;

	const GPU::Buffer &	vertices = eacpRenderProgs.StreamVertices( quad, sizeof( quad ) );

	GPU::RenderPassDescriptor	descriptor;

	// Black behind it, which is what shows if the drawable and the target are
	// ever different shapes.
	descriptor.clear = true;

	GPU::RenderPass	screen = eacpFrame->beginPass( descriptor );

	screen.setPipeline( *draw.pipeline );
	screen.setVertexBuffer( vertices );

	draw.program->bindTextures( screen );

	screen.draw( 6 );
	screen.end();
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

Where the frame goes. There is one place it can go - the render target the frame
is composed into - so what is left of this is the debug clear, which several of
the r_show* tools rely on to blank the parts of the screen they do not draw.

The clear is the only reason this opens a pass at all, and it opens the one the
frame's first view will use rather than one of its own: a view that finds a pass
nothing has drawn a world into keeps it (BeginDrawingView), so the common frame -
one 3D view and the 2D over it - is still one pass from here to SwapBuffers.
======================
*/
void idRenderBackendEacp::SetDrawBuffer( int buffer ) {
	EndPass();
	BeginPass( true );
}

/*
======================
idRenderBackendEacp::SwapBuffers

Still not a present - eacp's Frame presents itself when GPUView::render returns,
which is after common->Frame() has run - but since step 4e it is where the frame
reaches the drawable, because the drawable is not what it was drawn into.

Closing the pass first is not optional: a pass outliving the frame that made it
is undefined, and the one below samples what this one wrote.
======================
*/
void idRenderBackendEacp::SwapBuffers( void ) {
	EndPass();
	PresentFrameTarget();
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

	appliedViewport = Graphics::Rect( (float)rect.x1,
									  height - (float)( rect.y2 + 1 ),
									  (float)( rect.x2 + 1 - rect.x1 ),
									  (float)( rect.y2 + 1 - rect.y1 ) );
	hasViewport = true;

	// The depth range goes out with the viewport rather than on its own, which
	// is eacp's shape and not Doom 3's: glDepthRange is its own call, and the
	// two depth hacks use it without touching the rectangle. Re-sending the
	// rectangle to change the range costs nothing and keeps one call site.
	pass->setViewport( appliedViewport, depthRangeNear, depthRangeFar );
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

	appliedScissor = Graphics::Rect( x, height - ( y + h ), w, h );
	hasScissor = true;

	pass->setScissorRect( appliedScissor );
}

/*
======================
idRenderBackendEacp::DrawView

RB_STD_DrawView. The sequence rather than the calls: fill the depth buffer, add
each light through the stencil, blend the passes that do not depend on a light,
fog. All four are here.

What is not, and neither is a shortcut: RB_STD_LightScale, which multiplies the
whole frame up to crutch a backend whose blending range is eight bits and which
can never fire here (backEnd.overBright is 1, tr.backEndRendererMaxLight being
999 for BE_EACP as it is for BE_ARB2), and RB_RenderDebugTools, which is the
r_show* visualisations and has no eacp counterpart yet.
======================
*/
void idRenderBackendEacp::DrawView( void ) {
	if ( !pass ) {
		// A view with no pass open, which is not the same thing as a view with
		// nowhere to go. The frame's own pass is opened by SetDrawBuffer, and
		// the engine draws a view before that has run: idObjective::Event_CamShot
		// renders its camera into a crop from the *game's* think, several
		// commands before the frame that shows it begins.
		//
		// So a pass is opened here, and without the colour clear, because what
		// this view is being added to is whatever the last frame composed - the
		// same thing OpenGL's back buffer holds at this moment, and for the same
		// reason. Found by the camshot coming back black.
		BeginPass( false );
	}

	if ( !pass ) {
		// And this is the case the check above used to be: no frame at all, so
		// the draw came from outside GPUView::render - a level load's own screen
		// update, or a console command. Nothing to draw into, and saying so is
		// better than a pass that presents nothing.
		return;
	}

	drawSurf_t **	drawSurfs = (drawSurf_t **)&backEnd.viewDef->drawSurfs[0];
	const int		numDrawSurfs = backEnd.viewDef->numDrawSurfs;

	// What an interaction pass tests with once the depth fill has run: the
	// surface is already in the depth buffer at exactly its own depth, so a
	// light touches the fragments that survived and no others.
	backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL;

	if ( !BeginDrawingView() ) {
		return;
	}

	// backEnd.lightScale, which each light's colour is multiplied by. Its
	// sibling backEnd.overBright is always 1 here - tr.backEndRendererMaxLight
	// is 999 for BE_EACP as it is for BE_ARB2 - so RB_STD_LightScale, the
	// full-screen multiply that crutches up a backend whose blending range is
	// eight bits, can never do anything on this path and is not ported.
	RB_DetermineLightScale();

	FillDepthBuffer( drawSurfs, numDrawSurfs );

	DrawInteractions();

	const int	processed = DrawShaderPasses( drawSurfs, numDrawSurfs );

	// Between the two halves of the shader passes, because a post-process
	// surface reads the frame with the fog already in it - which is why
	// DrawShaderPasses returns how far it got rather than drawing the list.
	FogAllLights();

	if ( processed < numDrawSurfs ) {
		DrawShaderPasses( drawSurfs + processed, numDrawSurfs - processed );
	}
}

/*
======================
idRenderBackendEacp::BeginDrawingView

RB_BeginDrawingView, and the whole of what it clears.

Doom 3 empties the depth and stencil buffers here for a 3D view and leaves them
alone for a 2D one. Neither buffer can be cleared once a pass has begun - the
clear is a property of the attachment being loaded, on both of eacp's backends -
so a 3D view that finds a pass another 3D view has already drawn into ends it
and opens its own. That is what makes a frame's second 3D view - a mirror, a
subview, a remote camera - see an empty depth buffer rather than the first
view's, and it is the whole of step 4e.4.

**What the new pass keeps is the colour, and that is the part that needed
eacp.** Every view after the first draws over what the ones before it left: the
mirror is rendered first, into the scissor rectangle of the surface it belongs
to, and the view above it then multiplies that rectangle down by the overbright
factor rather than filling it black. So the second pass has to load the first
one's colour, which is `clear = false` and which gap 18 says is a lie on a
multisampled drawable - the MSAA attachment resolves and stores nothing, so the
load reads an attachment nobody kept. Step 4e.1 moved the frame into a texture
target, which is single-sampled and stored, and `clear = false` means what it
says there.

Only a 3D view counts, because only a 3D view writes depth: a 2D one is forced
to GLS_DEPTHFUNC_ALWAYS with no write in DrawShaderPasses, which is what
RB_BeginDrawingView's glDisable( GL_DEPTH_TEST ) amounts to.
======================
*/
bool idRenderBackendEacp::BeginDrawingView( void ) {
	const bool	worldView = ( backEnd.viewDef->viewEntitys != NULL );

	if ( worldView ) {
		if ( passHasWorldView ) {
			EndPass();
			BeginPass( false );

			// The pass is what SetViewport and SetScissor below measure
			// against, so a view whose own pass did not open has nowhere to be
			// drawn rather than somewhere odd.
			if ( !pass ) {
				return false;
			}
		}

		passHasWorldView = true;
	}

	depthRangeNear = 0.0f;
	depthRangeFar = 1.0f;

	SetViewport( backEnd.viewDef->viewport );

	backEnd.currentScissor = backEnd.viewDef->scissor;
	SetScissor( backEnd.currentScissor );

	// RB_BeginDrawingView's last act, and the reason SetCull is reached at all
	// below: the cached value has to disagree with whatever is asked for next.
	backEnd.glState.faceCulling = -1;
	SetCull( CT_FRONT_SIDED );

	return true;
}

/*
======================
idRenderBackendEacp::ViewCull

Which face a draw in this view culls, which is the one the material asked for
unless the view is mirrored - a mirror reverses the handedness of the camera, so
every triangle presents the winding it did not before, and front and back swap.

GL_Cull does this per draw, by choosing the enum it hands glCullFace. Here the
cull mode is compiled into a pipeline, so it has to be folded in before the
pipeline is looked up rather than after - which is also what puts it in the
cache key, so the mirrored and unmirrored draws of one material get the two
pipelines they need instead of sharing the first one compiled.

backEnd.glState.faceCulling therefore stays what the renderer asked for and
never what is drawn, which matters because the shared code sets it to -1 to
force the next SetCull through.
======================
*/
int idRenderBackendEacp::ViewCull( int cullType ) const {
	if ( !backEnd.viewDef || !backEnd.viewDef->isMirror ) {
		return cullType;
	}

	if ( cullType == CT_FRONT_SIDED ) {
		return CT_BACK_SIDED;
	}

	if ( cullType == CT_BACK_SIDED ) {
		return CT_FRONT_SIDED;
	}

	return cullType;
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

A subview's near clip plane is here, and it is the one thing this function does
that the walk below does not: the plane is the view's, in world coordinates, and
what a draw needs is the same plane in the surface's own - so it is transformed
per space, exactly where OpenGL re-sends its texgen.

Not here, and each is switched off by something this port controls:

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

	SelectTexture( 0 );

	backEnd.currentSpace = NULL;

	// The subview's near clip plane, carried into each space in turn. Rebuilt
	// on the space change rather than per surface, which is the same condition
	// RB_T_FillDepthBuffer re-sends its texgen plane on - and currentSpace
	// having just been cleared is what makes the first surface rebuild it.
	const bool	clipping = ( backEnd.viewDef->numClipPlanes != 0 );
	idPlane		localClipPlane;

	for ( int i = 0 ; i < numDrawSurfs ; i++ ) {
		const drawSurf_t *	surf = drawSurfs[i];

		if ( clipping && surf->space != backEnd.currentSpace ) {
			R_GlobalPlaneToLocal( surf->space->modelMatrix,
								  backEnd.viewDef->clipPlanes[0], localClipPlane );
		}

		SetSpace( surf->space, surf->space->modelDepthHack );

		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			SetScissor( backEnd.currentScissor );
		}

		FillDepthBufferSurface( surf, clipping ? localClipPlane.ToFloatPtr() : NULL );
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

clipPlane is the subview's near plane in this surface's coordinates, or NULL,
and it reaches only the perforated draws - which is not a shortcut but what
OpenGL does: the notch modulates the fragment's alpha, and nothing tests the
alpha of a surface that is not alpha-tested.
======================
*/
void idRenderBackendEacp::FillDepthBufferSurface( const drawSurf_t *surf,
												  const float *clipPlane ) {
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
	draw.clipPlane = clipPlane;

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
comes out the same everywhere on every surface. This backend needs no cube map
for it - the whole point of the substitution is that the answer does not depend
on the lookup - so the constant is computed here and handed over as a uniform.

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
====================
R_EacpGammaBrightness

program.env[21] as RB_SetProgramEnvironment and RB_ARB2_DrawInteraction fill it:
r_brightness and 1 / r_gamma with r_gammaInShader on, and the identity with it
off - or inside the post-process half of the shader passes, where the shared
code sets the identity too "to avoid applying them twice", a post-process stage
reading a _currentRender that has already been corrected.

The identity is also what a hardware ramp would leave the shader doing, and it
is all r_gammaInShader 0 can mean on this host: GLimp_SetGamma has no display
ramp to set and says so.

The fourth value is the flag R_EacpGammaCorrected branches on, folded here so the
shader compares one number: 1 when either setting is away from 1, and 0 when
both are, which is when the correction would be pow( x, 1 ) - a value the shader
is not asked to compute for the reason that function gives.
====================
*/
static Float4 R_EacpGammaBrightness( bool postProcess ) {
	if ( !r_gammaInShader.GetBool() || postProcess ) {
		return asFloat4( 1.0f, 1.0f, 0.0f, 0.0f );
	}

	const float	brightness = r_brightness.GetFloat();
	const float	gamma = r_gamma.GetFloat();
	const bool	apply = ( brightness != 1.0f ) || ( gamma != 1.0f );

	return asFloat4( brightness, 1.0f / gamma, apply ? 1.0f : 0.0f, 0.0f );
}

/*
======================
idRenderBackendEacp::DrawInteractions

RB_ARB2_DrawInteractions: every light in the view, added to the frame.

The order of the four calls inside the loop is not a detail. Doom 3 counts the
shadows a light casts onto *other* entities, adds the surfaces of the entity
casting them, counts the shadows it casts onto itself, and adds everything else
- which is what lets a monster's own shadow miss its own face while still
falling on the floor. MF_NOSELFSHADOW is what sorts a surface into which list,
and the interleaving is what makes the flag mean anything.
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

		// Neither goes through the interaction program: a fog light is a volume
		// of two blended passes and a blend light a projected multiply, and
		// both are drawn by FogAllLights after the ambient passes. These two
		// continues are what leaves them to it.
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

		const bool	shadows = ( vLight->globalShadows || vLight->localShadows )
			&& r_shadows.GetBool();

		if ( shadows ) {
			// The count starts from the same number under every light, which is
			// what the clear is for - and it is over the light's own scissor
			// rectangle, because that is the only part of the buffer this light
			// will read.
			backEnd.currentScissor = vLight->scissorRect;

			if ( r_useScissor.GetBool() ) {
				SetScissor( backEnd.currentScissor );
			}

			ClearStencil();

			lightStencil = ES_LIT;
		} else {
			// Nothing has been counted, so nothing may be masked: a light with
			// no shadow-casting surface in view has to reach every fragment
			// inside it, including the ones an earlier light's volumes left a
			// count in. glStencilFunc( GL_ALWAYS, 128, 255 ) is how OpenGL says
			// that and a pipeline that ignores the buffer is how this does.
			lightStencil = ES_IGNORE;
		}

		StencilShadowPass( vLight->globalShadows );
		CreateDrawInteractions( vLight->localInteractions );
		StencilShadowPass( vLight->localShadows );
		CreateDrawInteractions( vLight->globalInteractions );

		// A translucent surface is never in the depth buffer the fill wrote, so
		// it cannot be added at EQUAL - and it is never stencil shadowed
		// either, which is why it comes after both shadow passes rather than
		// between them.
		if ( r_skipTranslucent.GetBool() ) {
			continue;
		}

		lightStencil = ES_IGNORE;

		backEnd.depthFunc = GLS_DEPTHFUNC_LESS;
		CreateDrawInteractions( vLight->translucentInteractions );
		backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL;
	}

	lightStencil = ES_IGNORE;
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
================================================================================

	The shadows.

	Step 4d.3, and the last of the three the world was broken into. What the
	other two leave undone: 4d.1 put every surface in the depth buffer at its
	own depth and 4d.2 added each light to the surfaces inside it, but nothing
	yet asks whether something stands between the two.

	The answer is a count. Every shadow-casting surface under a light is
	extruded away from it into a closed volume, that volume is rasterized into
	the stencil buffer with the two facings counting in opposite directions, and
	a fragment whose count came back to where it started is a fragment no volume
	closed over. The interactions are then drawn through that count as a mask.

	Three things about it are eacp's rather than Doom 3's:

	  - **the two facings are one pipeline**, which is what glStencilOpSeparate
	    buys and what the four-way branch in RB_T_Shadow spends its length
	    working around. Both of eacp's backends have per-face stencil state
	    outright, so this is the one branch of the four.
	  - **the clear is a draw**, because a pass cannot be cleared once it has
	    begun. See ClearStencil.
	  - **the extrusion is the only vertex program Doom 3 has that this backend
	    keeps as a program.** shadow.vp is two instructions and no fragment
	    program, and it survives because what it computes is geometry rather
	    than a fixed-function state that a modern API spells differently.

================================================================================
*/

/*
======================
idRenderBackendEacp::ClearStencil

qglClear( GL_STENCIL_BUFFER_BIT ) inside the light's scissor rectangle, which is
what every light does before its own volumes are counted.

**A pass cannot be cleared once it has begun.** The clear is a property of the
attachment being loaded on both of eacp's backends, decided as the pass opens,
so the only way to empty a rectangle of the stencil buffer in the middle of a
frame is to draw over it - a quad, with StencilOp::Replace writing the pass's
reference value everywhere it covers. The scissor does the rest: it is already
the light's, so the quad reaches exactly the pixels the clear would have.

The quad goes through the shadow program, which sounds like a stretch and is
not: with the transform the identity and the light at the origin, the extrusion
that program computes is the identity too, so what it draws is the corners it
was handed. They are in clip space already, which is what the identity matrix
means here.
======================
*/
void idRenderBackendEacp::ClearStencil( void ) {
	if ( !pass ) {
		return;
	}

	// Two triangles over the whole of clip space at the near plane. The depth
	// is never read - the pipeline below tests Always and writes nothing - so
	// what it is chosen for is only being inside the frustum.
	static const eacpShadowVert_t	corners[6] = {
		{ { -1.0f, -1.0f, 0.0f, 1.0f } },
		{ {  1.0f, -1.0f, 0.0f, 1.0f } },
		{ {  1.0f,  1.0f, 0.0f, 1.0f } },
		{ { -1.0f, -1.0f, 0.0f, 1.0f } },
		{ {  1.0f,  1.0f, 0.0f, 1.0f } },
		{ { -1.0f,  1.0f, 0.0f, 1.0f } },
	};

	// No colour, no depth, and no cull - the quad's winding is nobody's
	// business, and CT_TWO_SIDED is what says so.
	const int	stateBits = GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK
		| GLS_DEPTHFUNC_ALWAYS;

	idEacpRenderProgs::shadowDraw_t	draw =
		eacpRenderProgs.ShadowDraw( stateBits, CT_TWO_SIDED, ES_CLEAR );

	if ( !draw.pipeline ) {
		return;
	}

	static const float	identity[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	draw.program->modelViewProjection = asFloat4x4( identity );
	draw.program->localLightOrigin = asFloat4( 0.0f, 0.0f, 0.0f, 0.0f );

	const GPU::Buffer &	vertices =
		eacpRenderProgs.StreamVertices( corners, sizeof( corners ) );

	pass->setPipeline( *draw.pipeline );
	pass->setVertexBuffer( vertices );
	pass->setUniforms( *draw.program );

	pass->draw( 6 );

	// Not a draw the renderer knows about, so nothing it counts - and the three
	// fields a draw is issued from are left as this found them, which is null:
	// the shadow surfaces set their own.
	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
}

/*
======================
idRenderBackendEacp::StencilShadowPass

RB_StencilShadowPass, and the walk RB_RenderDrawSurfChainWithFunction did for it.

Four things it does that are not here, and each is switched off by something
this port controls rather than skipped:

  - **the polygon offset** ( r_shadowPolygonFactor, r_shadowPolygonOffset ),
    which is eacp gap 6 - the same gap the decals want, and this is its second
    user. It matters because a volume's near cap is the occluder's own
    triangles: they are rebuilt through the extrusion's subtract-and-add rather
    than copied, so the cap lands within an ulp of the depth the surface itself
    wrote and which side of LessEqual it falls on is decided by rounding. Doom 3
    biases it one unit away and settles the question; nothing here does, and
    nothing in the frames measured shows it. If a shadow ever creeps onto the
    face that casts it, this is the reason.
  - **the depth bounds test**, which is a scope cut (plan.md section 7) and is
    already off: glConfig.depthBoundsTestAvailable is false.
  - **r_showShadows**, the debug visualisation, which draws the volumes
    visibly - as lines for two of its three values, and GLS_POLYMODE_LINE has
    no eacp counterpart any more than r_showTris' does.
  - **r_useCarmacksReverse 0**, the depth-pass-only algorithm from before the
    patent expired. What it needs beyond what is here is a third pair of
    stencil ops for its "preload", and nothing runs it: the cvar has defaulted
    to 1 since 2019.
======================
*/
void idRenderBackendEacp::StencilShadowPass( const drawSurf_t *drawSurfs ) {
	if ( !r_shadows.GetBool() ) {
		return;
	}

	if ( !drawSurfs ) {
		return;
	}

	if ( r_showShadows.GetInteger() && !warnedShowShadows ) {
		warnedShowShadows = true;
		common->Warning( "eacp: r_showShadows is not implemented, so the shadow "
						 "volumes are counted rather than drawn" );
	}

	if ( !r_useCarmacksReverse.GetBool() && !warnedDepthPassShadows ) {
		warnedDepthPassShadows = true;
		common->Warning( "eacp: r_useCarmacksReverse 0 is not implemented, so the "
						 "shadows are counted depth-fail either way" );
	}

	if ( !r_useShadowVertexProgram.GetBool() && !warnedShadowVertexProgram ) {
		warnedShadowVertexProgram = true;
		common->Warning( "eacp: r_useShadowVertexProgram 0 has no counterpart here - "
						 "the extrusion is the only way this backend projects a "
						 "shadow volume" );
	}

	// Write nothing but the stencil plane. The depth test still runs, and has
	// to: which side of it a fragment falls on is the whole of what is being
	// counted.
	SetState( GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS );

	// Both facings rasterized, which is what makes one pass over the volume
	// enough. GL_Cull's own state has to agree, because it is what the next
	// thing to draw compares against.
	SetCull( CT_TWO_SIDED );

	backEnd.currentSpace = NULL;

	for ( const drawSurf_t *surf = drawSurfs ; surf ; surf = surf->nextOnLight ) {
		SetSpace( surf->space, surf->space->modelDepthHack );

		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			SetScissor( backEnd.currentScissor );
		}

		ShadowSurface( surf );
	}

	SetCull( CT_FRONT_SIDED );

	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
}

/*
======================
idRenderBackendEacp::ShadowSurface

RB_T_Shadow: one surface's volume, counted.

Two decisions are made per surface and both are the frontend's work being read
back rather than anything computed here.

**How much of the volume to draw.** A shadow volume is built as sil planes
first, then the cap on the surface facing the light, then the cap at infinity -
so a shorter index count is a volume without its caps, and the frontend has
already worked out which surfaces can do without them. R_PotentiallyInsideInfiniteShadow
is what sets DSF_VIEW_INSIDE_SHADOW, and a volume the view is outside of needs
no caps at all.

**Which way to count.** A volume the view is outside of can be counted on the
fragments that passed the depth test, which is the older and cheaper algorithm;
one the view is inside has to be counted on the fragments that failed it, which
is Carmack's reverse and is why the caps were needed in the first place. The
`external` flag is the same one in both decisions, which is not a coincidence -
it is one question asked once.
======================
*/
void idRenderBackendEacp::ShadowSurface( const drawSurf_t *surf ) {
	const srfTriangles_t *	tri = surf->geo;

	if ( !tri->shadowCache ) {
		return;
	}

	int		numIndexes;
	bool	external = false;

	if ( !r_useExternalShadows.GetInteger() ) {
		numIndexes = tri->numIndexes;
	} else if ( r_useExternalShadows.GetInteger() == 2 ) {	// force no caps, for testing
		numIndexes = tri->numShadowIndexesNoCaps;
	} else if ( !( surf->dsFlags & DSF_VIEW_INSIDE_SHADOW ) ) {
		// outside the shadow projection: no caps are ever needed
		numIndexes = tri->numShadowIndexesNoCaps;
		external = true;
	} else if ( !backEnd.vLight->viewInsideLight
				&& !( surf->geo->shadowCapPlaneBits & SHADOW_CAP_INFINITE ) ) {
		// inside the projection but outside the light, on a volume that ends:
		// some of the caps can still go
		if ( backEnd.vLight->viewSeesShadowPlaneBits & surf->geo->shadowCapPlaneBits ) {
			numIndexes = tri->numShadowIndexesNoFrontCaps;
		} else {
			numIndexes = tri->numShadowIndexesNoCaps;
		}
		external = true;
	} else {
		numIndexes = tri->numIndexes;
	}

	const bool	mirrored = backEnd.viewDef->isMirror;

	eacpStencil_t	stencil;

	if ( external ) {
		stencil = mirrored ? ES_COUNT_DEPTH_PASS_MIRRORED : ES_COUNT_DEPTH_PASS;
	} else {
		stencil = mirrored ? ES_COUNT_DEPTH_FAIL_MIRRORED : ES_COUNT_DEPTH_FAIL;
	}

	idEacpRenderProgs::shadowDraw_t	draw =
		eacpRenderProgs.ShadowDraw( backEnd.glState.glStateBits,
									ViewCull( backEnd.glState.faceCulling ), stencil );

	if ( !draw.pipeline ) {
		return;
	}

	draw.program->modelViewProjection = asFloat4x4( modelViewProjection );

	// The light in the surface's own coordinates, which is the whole of what
	// the extrusion needs. w = 0 is not decoration: shadow.vp relies on it to
	// leave a vertex's own w alone through the subtraction, and
	// R_GlobalPointToLocal writes three floats.
	idVec4	localLight;

	R_GlobalPointToLocal( surf->space->modelMatrix, backEnd.vLight->globalLightOrigin,
						  localLight.ToVec3() );
	localLight.w = 0.0f;

	draw.program->localLightOrigin = asFloat4( localLight[0], localLight[1],
											   localLight[2], localLight[3] );

	// The shadow cache is the vertex cache's own block and its size is what
	// says how much of it there is: a volume that extrudes on the GPU shares
	// the ambient surface's doubled cache, so the surface's own numVerts is not
	// the count.
	const void *	vertices = vertexCache.Position( tri->shadowCache );

	drawVertices = &eacpRenderProgs.StreamVertices( vertices,
													(std::size_t)tri->shadowCache->size );

	drawProgram = draw.program;
	drawPipeline = draw.pipeline;

	RB_DrawShadowElementsWithCounters( tri, numIndexes );
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
										 ViewCull( backEnd.glState.faceCulling ),
										 lightStencil );

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

	// Never the post-process half: an interaction is drawn before the shader
	// passes begin, which is what RB_ARB2_DrawInteraction's own unconditional
	// env[21] amounts to.
	program->gammaBrightness = R_EacpGammaBrightness( false );

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

RB_STD_DrawShaderPasses. One of its parts is not here and is not a shortcut: the
ARB program environment, this backend having no ARB programs to give one to.

The other one - the _currentRender copy before the post-process surfaces - is
here since step 4e.3, and the shared code's own is still guarded by BE_ARB2, so
the two paths do it once each rather than one of them twice.

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

	// RB_STD_DrawShaderPasses' isPostProcess, kept for the draws below rather
	// than handed to a program environment: it decides whether a reflection
	// stage gets the gamma correction, and the shared code's answer is that the
	// post-process half does not.
	postProcessPass = ( drawSurfs[0]->material->GetSort() >= SS_POST_PROCESS );

	if ( postProcessPass ) {
		if ( r_skipPostProcess.GetBool() ) {
			return 0;
		}

		// Only in a 3D view, which is the shared path's rule too: the console
		// and the menus are drawn by this same function and there is nothing
		// behind them to refract.
		if ( backEnd.viewDef->viewEntitys ) {
			globalImages->currentRenderImage->CopyFramebuffer(
				backEnd.viewDef->viewport.x1,
				backEnd.viewDef->viewport.y1,
				backEnd.viewDef->viewport.x2 - backEnd.viewDef->viewport.x1 + 1,
				backEnd.viewDef->viewport.y2 - backEnd.viewDef->viewport.y1 + 1,
				true );
		}

		// Said whether or not anything was copied, because what it stops is the
		// loop below breaking at the first post-process surface - which is how
		// the two calls to this function divide the list between them.
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

		// TG_GLASSWARP is the one texgen still skipped, and it is not really a
		// texgen: it is a hand-written ARB fragment program that happens to be
		// selected by one, sampling _scratch and _scratch2 through a distortion.
		// It belongs with the newStage above rather than with the four below,
		// and nothing in the demo pk4 declares it at all.
		if ( pStage->texture.texgen == TG_GLASSWARP ) {
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
		draw.clipPlane = NULL;
		draw.texgen = pStage->texture.texgen;
		draw.bumpImage = NULL;
		draw.texgenMatrix = NULL;

		float	matrix[16];

		if ( pStage->texture.hasMatrix ) {
			RB_GetShaderTextureMatrix( regs, &pStage->texture, matrix );
			draw.textureMatrix = matrix;
		} else {
			draw.textureMatrix = NULL;
		}

		float	wobble[16];

		if ( draw.texgen == TG_WOBBLESKY_CUBE ) {
			R_WobbleskyTransform( surf, backEnd.viewDef->floatTime, wobble );
			draw.texgenMatrix = wobble;
		}

		// Which of the two reflection programs this stage wants, decided on the
		// same test RB_PrepareStageTexturing makes and nothing else: a material
		// with a bump stage reflects through its normal map in global space, one
		// without reflects its own vertex normal in model space.
		if ( draw.texgen == TG_REFLECT_CUBE ) {
			const shaderStage_t *	bumpStage = shader->GetBumpStage();

			if ( bumpStage ) {
				draw.bumpImage = bumpStage->texture.image;
			}
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

		// Where OpenGL calls RB_PrepareStageTexturing and then one shared draw,
		// this picks the program the texgen names and then draws. Same
		// decision, made a step earlier - because on a backend where the
		// coordinate generator is compiled into a shader, choosing it *is*
		// choosing the program.
		switch ( draw.texgen ) {
		case TG_SKYBOX_CUBE:
		case TG_WOBBLESKY_CUBE:
			DrawCubeStage( tri, draw, ECT_SKY );
			break;
		case TG_DIFFUSE_CUBE:
			DrawCubeStage( tri, draw, ECT_DIFFUSE );
			break;
		case TG_REFLECT_CUBE:
			if ( draw.bumpImage ) {
				DrawBumpyReflectStage( tri, draw );
			} else {
				DrawCubeStage( tri, draw, ECT_REFLECT );
			}
			break;
		case TG_SCREEN:
		case TG_SCREEN2:
			DrawScreenStage( tri, draw );
			break;
		default:
			DrawStage( tri, draw );
			break;
		}
	}

	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
}

/*
======================
idRenderBackendEacp::StageColor

The three stageVertexColor_t modes as the (modulate, add) pair every one of
these programs takes. OpenGL needs a combiner and a second texture unit bound to
the white image to say the same thing; here they are two uniforms.

SVC_INVERSE_MODULATE inverts the alpha channel along with the three colour ones,
which the fixed-function path does not: it sets GL_COMBINE_RGB alone and leaves
alpha on the default modulate. That is a deliberate simplification and the one
BFG's own port makes - the mode exists for cross-blended terrain, where the alpha
is 1 either way, and no material in the demo reaches it at all.

**`combiner` is what separates the fixed-function stages from the two ARB ones,
and it is not a shortcut.** environment.vfp and bumpyEnvironment.vfp are fragment
programs, and a fragment program replaces the texture-env combiner outright - so
on a reflect stage the second texture unit the loop above binds the white image
on does nothing, GL_COMBINE_RGB does nothing, and what reaches the shader is
`vertex.color`: the stage's constant colour where SVC_IGNORE put it there with
glColor4fv, and the vertex colour array otherwise. Which is (0, c) and (1, 0) -
the same two uniforms, computed from a different rule.
======================
*/
void idRenderBackendEacp::StageColor( const eacpStage_t &stage, bool combiner,
									  GPU::Uniform<GPU::Float4> &modulate,
									  GPU::Uniform<GPU::Float4> &add ) {
	const Float4 &	color = stage.color;
	const Float4	black = asFloat4( 0.0f, 0.0f, 0.0f, 0.0f );
	const Float4	white = asFloat4( 1.0f, 1.0f, 1.0f, 1.0f );

	if ( !combiner ) {
		if ( stage.vertexColor == SVC_IGNORE ) {
			modulate = black;
			add = color;
		} else {
			modulate = white;
			add = black;
		}

		return;
	}

	switch ( stage.vertexColor ) {
	case SVC_MODULATE:
		modulate = color;
		add = black;
		break;
	case SVC_INVERSE_MODULATE:
		modulate = asFloat4( -color[0], -color[1], -color[2], -color[3] );
		add = color;
		break;
	default:
		modulate = black;
		add = color;
		break;
	}
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
	// A 2D image, and the shape is part of the question rather than an
	// assumption: a material may declare `cubeMap` and no texgen at all, which
	// is a cube bound where this program declared a 2D texture. TextureForStage
	// says so and the draw is skipped, where before cube textures existed it
	// simply had no texture to bind.
	const GPU::Texture *	texture = TextureForStage( stage.image, false );

	if ( !texture ) {
		return;
	}

	idEacpRenderProgs::stageDraw_t	draw =
		eacpRenderProgs.StageDraw( stage.image, stage.stateBits, ViewCull( stage.cullType ),
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

	StageColor( stage, true, draw.program->colorModulate, draw.program->colorAdd );

	draw.program->alphaTestRef = stage.alphaTestRef;

	// The subview's near plane, or the plane every vertex is a unit in front
	// of - which is what says "no clipping" without a variant to say it in.
	if ( stage.clipPlane ) {
		draw.program->clipPlane = asFloat4( stage.clipPlane[0], stage.clipPlane[1],
											stage.clipPlane[2], stage.clipPlane[3] );
	} else {
		draw.program->clipPlane = asFloat4( 0.0f, 0.0f, 0.0f, 1.0f );
	}

	draw.program->image = *texture;

	drawProgram = draw.program;
	drawPipeline = draw.pipeline;

	// Through the counter wrapper rather than around it: r_showPrimitives and
	// the renderer's own performance counters are read from the same place on
	// both backends, and DrawIndexed is the seam.
	RB_DrawElementsWithCounters( tri );
}

/*
================================================================================

	The fog and the blend lights.

	Step 4e.5, and the last kind of light in a view. A fog light and a blend
	light are both in backEnd.viewDef->viewLights beside the real ones and both
	are skipped by DrawInteractions with a continue, because neither is an
	interaction: they do not light a surface, they *tint* everything inside a
	volume. So they are drawn here, after the ambient passes and before the
	post-process ones, which is where RB_STD_DrawView puts them and for the
	reason its own comment gives - a surface that reads _currentRender has to
	find the fog already in it.

	Three things they share, and each is why they are one section rather than
	two:

	  - **their coordinates are planes rather than vertex attributes.** Neither
	    program reads the texture coordinate the surface carries; both dot the
	    vertex against planes that belong to the light or to the view. On
	    OpenGL that is glTexGen with GL_OBJECT_LINEAR and a plane re-sent
	    whenever the space changes, and here it is a uniform rebuilt at the same
	    moment, exactly as FillDepthBuffer already does with a subview's clip
	    plane.
	  - **they walk the light's interaction lists**, the same globalInteractions
	    and localInteractions the interaction program walks - so a surface is
	    fogged if and only if it is lit, which is what makes a fog volume stop
	    at a wall.
	  - **the stencil is off for both.** RB_STD_FogAllLights brackets the whole
	    loop with glDisable( GL_STENCIL_TEST ), which is ES_IGNORE here: a fog
	    light casts no shadow and reads no count.

================================================================================
*/

/*
======================
idRenderBackendEacp::FogAllLights

RB_STD_FogAllLights: every fog and blend light in the view, in the order the
frontend listed them.

Four skips, all the shared code's own - and the first of them is wider than its
name. **r_skipFogLights turns off the blend lights too**, because
RB_STD_FogAllLights tests it before the loop that dispatches to either kind,
while r_skipBlendLights is tested inside RB_BlendLight alone. So
`r_skipFogLights 1` is "draw neither" and `r_skipBlendLights 1` is "draw the fog
but not the blend lights", which is not the pair of switches the two names
suggest. Reproduced rather than tidied up, because a debug cvar that means
something different on the two backends is worse than one that is oddly named
on both - and because `r_skipFogLights 1` is what reproduces the picture this
build drew before this step, which is how the step was measured.

The stencil clear the original has between the lights is inside an #if 0 that
_D3XP turned off - it guaranteed no pixel could be double-fogged thousands of
units from the origin - so there is nothing here to be its counterpart, and a
per-light stencil clear would be a scissored quad (see ClearStencil) if there
ever were.
======================
*/
void idRenderBackendEacp::FogAllLights( void ) {
	if ( r_skipFogLights.GetBool() ) {
		return;
	}

	if ( r_showOverDraw.GetInteger() != 0 ) {
		return;
	}

	// An xray view is the world seen through a different set of entities, and
	// fogging it would fog the thing being seen through rather than the scene.
	if ( backEnd.viewDef->isXraySubview ) {
		return;
	}

	// A 2D view has no lights at all, and no depth buffer for a fog volume to
	// be occluded by.
	if ( !backEnd.viewDef->viewEntitys ) {
		return;
	}

	for ( viewLight_t *vLight = backEnd.viewDef->viewLights ; vLight ; vLight = vLight->next ) {
		backEnd.vLight = vLight;

		if ( vLight->lightShader->IsFogLight() ) {
			FogPass( vLight->globalInteractions, vLight->localInteractions );
		} else if ( vLight->lightShader->IsBlendLight() ) {
			BlendLight( vLight->globalInteractions, vLight->localInteractions );
		}
	}

	backEnd.vLight = NULL;

	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
}

/*
======================
idRenderBackendEacp::FogPass

RB_FogPass: one fog light, as the surfaces inside it plus the volume itself.

**The two draws are two different things and the second is the whole trick.**
The surfaces are drawn at GLS_DEPTHFUNC_EQUAL over what the depth fill wrote, so
each of them is tinted by how far the eye is looking through the fog to reach
it. That covers everything the fog stands in front of - but not the *sky*, or
anything else the view sees past the far side of the volume, because nothing was
written into the depth buffer there. So the light's own frustum is drawn as one
more surface, back faces only and at GLS_DEPTHFUNC_LESS, which fills in exactly
the fragments no surface claimed. tr_light.cpp gives the frustum an ambient
cache for this and for nothing else.

Three numbers are the light's rather than the surface's and are computed once
here:

  - **the density plane**, which is the view's forward axis scaled so that a
    fragment a fog distance away reads the edge of the _fog image. The distance
    is the light material's alpha register, which is why a fog light's alpha is
    not an opacity - and why the default value of 1 means "500 units" rather
    than "fully opaque".
  - **the fog volume's top plane**, scaled by 0.001 so that a height in units
    lands inside the _fogEnter image, and
  - **the viewer's own height above it**, which is the same plane evaluated at
    the eye. That is a number rather than a plane, and it is carried as a plane
    with no normal so that both coordinates reach the shader the same way.

FOG_ENTER is added to the last two, and it is half a texel plus a half: the
image is 64 wide and the terminator has to land exactly on the boundary between
its two halves, which the comment beside FOG_ENTER in tr_local.h calls "picky to
get the bilerp correct".
======================
*/
void idRenderBackendEacp::FogPass( const drawSurf_t *drawSurfs, const drawSurf_t *drawSurfs2 ) {
	// No r_skipFogLights here: RB_FogPass has none either, the switch being
	// FogAllLights' one line above.

	// The light's frustum, drawn side out. If the vertex cache had no room for
	// it the volume cannot be closed, and half a fog light is worse than none.
	const srfTriangles_t *	frustumTris = backEnd.vLight->frustumTris;

	if ( !frustumTris || !frustumTris->ambientCache ) {
		return;
	}

	drawSurf_t	ds;

	memset( &ds, 0, sizeof( ds ) );
	ds.space = &backEnd.viewDef->worldSpace;
	ds.geo = frustumTris;
	ds.scissorRect = backEnd.viewDef->scissor;

	// A fog light's material has one stage by assumption - RB_FogPass says so
	// in a comment and reads GetStage( 0 ) without looking - and its four
	// colour registers are the fog's colour and its distance.
	const idMaterial *		lightShader = backEnd.vLight->lightShader;
	const float *			regs = backEnd.vLight->shaderRegisters;
	const shaderStage_t *	stage = lightShader->GetStage( 0 );

	eacpFog_t	fog;

	const float	distance = regs[ stage->color.registers[3] ];

	// Three channels and a 1, because qglColor3fv is what sends this and GL
	// fills the fourth in. The alpha the register holds is the distance below.
	fog.color = asFloat4( regs[ stage->color.registers[0] ],
						  regs[ stage->color.registers[1] ],
						  regs[ stage->color.registers[2] ],
						  1.0f );

	// "If they left the default value on, set a fog distance of 500."
	const float	a = ( distance <= 1.0f ) ? ( -0.5f / DEFAULT_FOG_DISTANCE )
										 : ( -0.5f / distance );

	// The third row of the view's modelview matrix, which is the distance along
	// the view axis - so this plane evaluated at a vertex is how far in front of
	// the eye it is, scaled into the image's half-width.
	const float *	modelView = backEnd.viewDef->worldSpace.modelViewMatrix;

	fog.density[0] = a * modelView[2];
	fog.density[1] = a * modelView[6];
	fog.density[2] = a * modelView[10];
	fog.density[3] = a * modelView[14];

	// The fog volume's fade plane - "always the top plane on unrotated lights",
	// which is what makes a fog volume something a player can stand half inside.
	fog.enterT[0] = 0.001f * backEnd.vLight->fogPlane[0];
	fog.enterT[1] = 0.001f * backEnd.vLight->fogPlane[1];
	fog.enterT[2] = 0.001f * backEnd.vLight->fogPlane[2];
	fog.enterT[3] = 0.001f * backEnd.vLight->fogPlane[3];

	const float	viewHeight =
		backEnd.viewDef->renderView.vieworg * fog.enterT.Normal() + fog.enterT[3];

	fog.enterS[0] = 0.0f;
	fog.enterS[1] = 0.0f;
	fog.enterS[2] = 0.0f;
	fog.enterS[3] = FOG_ENTER + viewHeight;

	// **fogPlanes[1] is not here, and its absence is the original's own
	// decision rather than this port's.** RB_T_BasicFog computes a second
	// density plane off the view's right axis - which would make the lookup a
	// real two-dimensional distance - and then overwrites the texgen plane with
	// ( 0, 0, 0, 0.5 ) on the very next line, with the two lines that would have
	// used it commented out above. So the _fog image's t is a constant, the
	// program says 0.5 outright, and computing the plane here would be
	// computing something nothing reads.

	SetState( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA
			  | GLS_DEPTHFUNC_EQUAL );

	FogChain( drawSurfs, fog );
	FogChain( drawSurfs2, fog );

	// The frustum is not in the depth buffer, so it cannot be drawn at EQUAL -
	// and it is drawn back faces only, because what is wanted is where the
	// volume *ends* behind everything else in the view.
	SetState( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA
			  | GLS_DEPTHFUNC_LESS );
	SetCull( CT_BACK_SIDED );

	FogChain( &ds, fog );

	SetCull( CT_FRONT_SIDED );
}

/*
======================
idRenderBackendEacp::FogChain

RB_RenderDrawSurfChainWithFunction over RB_T_BasicFog, which cannot be the
shared walker because that one makes four qgl calls of its own - the modelview
matrix, the two depth hacks and the scissor. Here all four are SetSpace and
SetScissor, which is the same collapse CreateDrawInteractions made in step 4d.2.

The three planes are transformed into each space as it arrives rather than per
surface, which is exactly the condition RB_T_BasicFog re-sends its texgens on:
backEnd.currentSpace changing. Clearing it first is what makes the first surface
of every chain rebuild them, and it is what the shared walker does too.
======================
*/
void idRenderBackendEacp::FogChain( const drawSurf_t *chain, const eacpFog_t &fog ) {
	if ( !chain ) {
		return;
	}

	backEnd.currentSpace = NULL;

	idPlane	local[3];

	for ( const drawSurf_t *surf = chain ; surf ; surf = surf->nextOnLight ) {
		if ( surf->space != backEnd.currentSpace ) {
			// The half texel that puts a vertex at the eye in the middle of the
			// _fog image, and the FOG_ENTER offsets that centre the terminator
			// in the _fogEnter one. Added after the transform rather than
			// before it, because R_GlobalPlaneToLocal moves the constant term
			// and these are offsets in the image's coordinates rather than in
			// the world's.
			R_GlobalPlaneToLocal( surf->space->modelMatrix, fog.density, local[0] );
			local[0][3] += 0.5f;

			R_GlobalPlaneToLocal( surf->space->modelMatrix, fog.enterS, local[1] );

			R_GlobalPlaneToLocal( surf->space->modelMatrix, fog.enterT, local[2] );
			local[2][3] += FOG_ENTER;
		}

		SetSpace( surf->space, surf->space->modelDepthHack );

		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			SetScissor( backEnd.currentScissor );
		}

		FogSurface( surf, fog, local );
	}

	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
}

/*
======================
idRenderBackendEacp::FogSurface

RB_T_BasicFog's draw, which is one surface's ambient cache through the fog
program - no texture coordinate, no normal and no colour read from it, only the
position, because every coordinate the program samples at is a plane dotted with
that position.
======================
*/
void idRenderBackendEacp::FogSurface( const drawSurf_t *surf, const eacpFog_t &fog,
									  const idPlane local[3] ) {
	const srfTriangles_t *	tri = surf->geo;

	if ( !tri || !tri->numIndexes || !tri->ambientCache ) {
		return;
	}

	idEacpRenderProgs::fogDraw_t	draw =
		eacpRenderProgs.FogDraw( backEnd.glState.glStateBits,
								 ViewCull( backEnd.glState.faceCulling ) );

	if ( !draw.pipeline ) {
		return;
	}

	GPU::Texture *	fogTexture = TextureFor( globalImages->fogImage );
	GPU::Texture *	enterTexture = TextureFor( globalImages->fogEnterImage );

	if ( !fogTexture || !enterTexture ) {
		if ( !warnedMissingTexture ) {
			warnedMissingTexture = true;
			common->Warning( "eacp: the fog lookup images have no texture on the GPU, "
							 "so the fog will not draw" );
		}
		return;
	}

	draw.program->modelViewProjection = asFloat4x4( modelViewProjection );

	draw.program->fogPlane = asFloat4( local[0][0], local[0][1], local[0][2], local[0][3] );
	draw.program->fogEnterPlaneS = asFloat4( local[1][0], local[1][1], local[1][2], local[1][3] );
	draw.program->fogEnterPlaneT = asFloat4( local[2][0], local[2][1], local[2][2], local[2][3] );

	draw.program->fogColor = fog.color;

	draw.program->fogImage = *fogTexture;
	draw.program->fogEnterImage = *enterTexture;

	drawProgram = draw.program;
	drawPipeline = draw.pipeline;

	const idDrawVert *	vertices = (const idDrawVert *)vertexCache.Position( tri->ambientCache );

	drawVertices = &eacpRenderProgs.StreamVertices(
		vertices, (std::size_t)tri->numVerts * sizeof( idDrawVert ) );

	RB_DrawElementsWithCounters( tri );
}

/*
======================
idRenderBackendEacp::BlendLight

RB_BlendLight: one blend light, once per stage of its material.

**The stage loop is outside the surface walk and not inside it**, which is the
opposite of everything else in this backend and is the original's arrangement:
a two-stage blend light draws every surface twice rather than each surface
twice. It matters because the stages blend into each other - the second is drawn
over the first's result across the whole volume - and it costs the vertex
streaming nothing here, because the walk re-streams a surface's vertices per
pass either way.

There is no frustum draw. A blend light tints what is inside it and stops; only
the fog needs a volume to close, because only the fog is a function of *distance
through* rather than of position.
======================
*/
void idRenderBackendEacp::BlendLight( const drawSurf_t *drawSurfs, const drawSurf_t *drawSurfs2 ) {
	// **The first list decides whether either is drawn**, which is RB_BlendLight's
	// own first line and reads like a slip: a light whose only surfaces are the
	// no-self-shadow ones in localInteractions draws nothing. Kept, because it
	// is what the game does - and it is nearly unreachable anyway, a blend light
	// material carrying noShadows, which sends every surface to
	// globalInteractions in idInteraction::AddActiveInteraction.
	if ( !drawSurfs ) {
		return;
	}

	if ( r_skipBlendLights.GetBool() ) {
		return;
	}

	const idMaterial *	lightShader = backEnd.vLight->lightShader;
	const float *		regs = backEnd.vLight->shaderRegisters;

	for ( int i = 0 ; i < lightShader->GetNumStages() ; i++ ) {
		const shaderStage_t *	stage = lightShader->GetStage( i );

		if ( !regs[ stage->conditionRegister ] ) {
			continue;
		}

		eacpBlendLight_t	light;

		light.image = stage->texture.image;
		light.falloff = backEnd.vLight->falloffImage;
		light.stateBits = GLS_DEPTHMASK | stage->drawStateBits | GLS_DEPTHFUNC_EQUAL;

		// "Get the modulate values from the light, including alpha, unlike
		// normal lights" - the stage's own comment, and the difference is that
		// a blend light's alpha reaches the blend function rather than being
		// thrown away by an additive one.
		light.color = asFloat4( regs[ stage->color.registers[0] ],
								regs[ stage->color.registers[1] ],
								regs[ stage->color.registers[2] ],
								regs[ stage->color.registers[3] ] );

		// The texture matrix belongs to the *generated* coordinate rather than
		// to a vertex attribute, so it cannot be a matrix on the way in the
		// generic stage program's is. It is folded into the projection planes
		// once they reach a surface's space, which is what
		// RB_BakeTextureMatrixIntoTexgen is for and where
		// R_SetDrawInteraction already calls it for a real light. Left in
		// backEnd.lightTextureMatrix, which is that function's own input.
		light.hasMatrix = stage->texture.hasMatrix;

		if ( light.hasMatrix ) {
			RB_GetShaderTextureMatrix( regs, &stage->texture, backEnd.lightTextureMatrix );
		}

		SetState( light.stateBits );

		BlendLightChain( drawSurfs, light );
		BlendLightChain( drawSurfs2, light );
	}
}

/*
======================
idRenderBackendEacp::BlendLightChain

The same walk FogChain does, with the light's four projection planes carried
into each space instead of the fog's three.

**RB_T_BlendLight has a second vertex path this does not, and it is dead.** It
takes a surface's shadowCache when there is no ambientCache, under a comment
saying it "gets used for both blend lights and shadow draws" - which is a
leftover from a shadow path that no longer calls it, RB_T_Shadow having its own.
A blend light's globalInteractions and localInteractions are built by
idInteraction::AddActiveInteraction, which sets lightTris->ambientCache from the
surface's own before linking it and links shadow volumes into globalShadows and
localShadows instead, so a surface in these two lists always has an ambient
cache and never arrives with only a shadow one.
======================
*/
void idRenderBackendEacp::BlendLightChain( const drawSurf_t *chain,
										   const eacpBlendLight_t &light ) {
	if ( !chain ) {
		return;
	}

	backEnd.currentSpace = NULL;

	idPlane	local[4];

	for ( const drawSurf_t *surf = chain ; surf ; surf = surf->nextOnLight ) {
		if ( surf->space != backEnd.currentSpace ) {
			for ( int i = 0 ; i < 4 ; i++ ) {
				R_GlobalPlaneToLocal( surf->space->modelMatrix,
									  backEnd.vLight->lightProject[i], local[i] );
			}

			// After the transform, because the matrix acts on the coordinate
			// the planes generate rather than on the plane - which is the order
			// R_SetDrawInteraction uses on the same four planes.
			if ( light.hasMatrix ) {
				RB_BakeTextureMatrixIntoTexgen( local, backEnd.lightTextureMatrix );
			}
		}

		SetSpace( surf->space, surf->space->modelDepthHack );

		if ( r_useScissor.GetBool() && !backEnd.currentScissor.Equals( surf->scissorRect ) ) {
			backEnd.currentScissor = surf->scissorRect;
			SetScissor( backEnd.currentScissor );
		}

		BlendLightSurface( surf, light, local );
	}

	drawProgram = NULL;
	drawPipeline = NULL;
	drawVertices = NULL;
}

/*
======================
idRenderBackendEacp::BlendLightSurface

RB_T_BlendLight's draw: one surface's ambient cache through the blend light
program, at the four planes this space was given.
======================
*/
void idRenderBackendEacp::BlendLightSurface( const drawSurf_t *surf,
											 const eacpBlendLight_t &light,
											 const idPlane local[4] ) {
	const srfTriangles_t *	tri = surf->geo;

	if ( !tri || !tri->numIndexes || !tri->ambientCache ) {
		return;
	}

	if ( !light.image || !light.falloff ) {
		return;
	}

	idEacpRenderProgs::blendLightDraw_t	draw =
		eacpRenderProgs.BlendLightDraw( light.image, light.falloff,
										backEnd.glState.glStateBits,
										ViewCull( backEnd.glState.faceCulling ) );

	if ( !draw.pipeline ) {
		return;
	}

	GPU::Texture *	projection = TextureFor( light.image );
	GPU::Texture *	falloff = TextureFor( light.falloff );

	if ( !projection || !falloff ) {
		// A light's projected image is often a .dds this build cannot read, so
		// this is the same warning DrawInteraction carries and for the same
		// reason.
		if ( !warnedMissingTexture ) {
			warnedMissingTexture = true;
			common->Warning( "eacp: '%s' has no texture on the GPU, so a blend light "
							 "will not draw",
							 projection ? light.falloff->imgName.c_str()
										: light.image->imgName.c_str() );
		}
		return;
	}

	draw.program->modelViewProjection = asFloat4x4( modelViewProjection );

	draw.program->lightProjectionS = asFloat4( local[0][0], local[0][1], local[0][2], local[0][3] );
	draw.program->lightProjectionT = asFloat4( local[1][0], local[1][1], local[1][2], local[1][3] );
	draw.program->lightProjectionQ = asFloat4( local[2][0], local[2][1], local[2][2], local[2][3] );
	draw.program->lightFalloffS = asFloat4( local[3][0], local[3][1], local[3][2], local[3][3] );

	draw.program->color = light.color;

	draw.program->lightImage = *projection;
	draw.program->lightFalloffImage = *falloff;

	drawProgram = draw.program;
	drawPipeline = draw.pipeline;

	const idDrawVert *	vertices = (const idDrawVert *)vertexCache.Position( tri->ambientCache );

	drawVertices = &eacpRenderProgs.StreamVertices(
		vertices, (std::size_t)tri->numVerts * sizeof( idDrawVert ) );

	RB_DrawElementsWithCounters( tri );
}

/*
======================
idRenderBackendEacp::TextureForStage

The texture an image carries, and whether it is the shape the program about to
sample it declared.

The check is here because nothing else does it. eacp binds a cube and a 2D
texture through the same call on the same slot, so a mismatch is not a bind
error - on Metal the sampler reads a texture whose type the shader does not
expect and on D3D12 an SRV of the wrong dimension, and neither says a word. What
it looks like is a black surface.

It is reachable rather than theoretical: `cubeMap` in a material whose six files
are not all there loads nothing, and idImage falls back to the default image,
which is 2D.
======================
*/
const GPU::Texture *idRenderBackendEacp::TextureForStage( const idImage *image, bool wantCube ) {
	GPU::Texture *	texture = image ? TextureFor( image ) : NULL;

	if ( !texture ) {
		// An image the upload path turned away - a .dds this build cannot read,
		// or a cube whose six faces did not all arrive.
		if ( !warnedMissingTexture ) {
			warnedMissingTexture = true;
			common->Warning( "eacp: '%s' has no texture on the GPU, so a surface will not draw",
							 image ? image->imgName.c_str() : "(none)" );
		}
		return NULL;
	}

	if ( texture->isCube() != wantCube ) {
		if ( !warnedCubeShape ) {
			warnedCubeShape = true;
			common->Warning( "eacp: '%s' is %s where the stage wanted %s, so a surface "
							 "will not draw",
							 image->imgName.c_str(),
							 texture->isCube() ? "a cube map" : "a 2D image",
							 wantCube ? "a cube map" : "a 2D image" );
		}
		return NULL;
	}

	return texture;
}

/*
======================
idRenderBackendEacp::LocalViewOrigin

The eye in the space being drawn. R_SkyboxTexGen computes this in the frontend
and RB_SetProgramEnvironmentSpace computes it again in the backend, from the same
two values; here there is one caller's worth of it and it is the backend's.

w is 1 because the ARB programs' program.env[5] carries 1, and because nothing
here reads it - the shaders take .xyz.
======================
*/
Float4 idRenderBackendEacp::LocalViewOrigin( void ) const {
	idVec3	local;

	R_GlobalPointToLocal( backEnd.currentSpace->modelMatrix,
						  backEnd.viewDef->renderView.vieworg, local );

	return asFloat4( local[0], local[1], local[2], 1.0f );
}

/*
======================
idRenderBackendEacp::DrawCubeStage

A sky, a diffuse cube or an unbumped reflection: one draw through
idEacpCubeProgram, whose texgen decides which.

What OpenGL does instead is three different things - two of them a
glTexCoordPointer at a buffer the frontend filled, and the third a pair of ARB
programs - and none of them is a fourth kind of draw. So the shape here is
DrawStage's exactly: the two lookups, the uniforms, and RB_DrawElementsWithCounters
over the geometry the caller streamed.

The texture matrix is not applied, and that is a decision rather than an
omission. GL's texture matrix multiplies the *whole* coordinate, and a cube's is
a direction: Doom 3's matrix mixes s and t into each other and adds a translation
built for the [0, 1] of an image, which on a direction vector is a rotation about
z plus an offset that has no meaning. Two materials in the demo pk4 combine the
two - `shaderDemos/cloudySky` and `shaderDemos/skybox`, both with `rotate` - and
neither of the demo's maps places either of them, so there is nothing here to
check a faithful reproduction of the mixture against.
======================
*/
void idRenderBackendEacp::DrawCubeStage( const srfTriangles_t *tri, const eacpStage_t &stage,
										 eacpCubeTexgen_t texgen ) {
	const GPU::Texture *	texture = TextureForStage( stage.image, true );

	if ( !texture ) {
		return;
	}

	idEacpRenderProgs::cubeDraw_t	draw =
		eacpRenderProgs.CubeDraw( texgen, stage.image, stage.stateBits,
								  ViewCull( stage.cullType ) );

	if ( !draw.pipeline ) {
		return;
	}

	draw.program->modelViewProjection = asFloat4x4( modelViewProjection );
	draw.program->localViewOrigin = LocalViewOrigin();

	// R_WobbleskyTexGen's transform, or the identity - which is what
	// R_SkyboxTexGen amounts to, and why the two are one program.
	static const float	identity[16] = { 1, 0, 0, 0,  0, 1, 0, 0,
										 0, 0, 1, 0,  0, 0, 0, 1 };

	draw.program->texgenMatrix =
		asFloat4x4( stage.texgenMatrix ? stage.texgenMatrix : identity );

	// A reflect stage is environment.vfp, where a fragment program has replaced
	// the combiner - so the constant colour reaches the shader only where
	// SVC_IGNORE put it in glColor. StageColor's own comment has the argument.
	StageColor( stage, texgen != ECT_REFLECT,
				draw.program->colorModulate, draw.program->colorAdd );

	draw.program->cubeImage = *texture;

	// Read by the reflect texgen alone, and set on every variant because the
	// uniform block is the same size whichever of them is drawing.
	draw.program->gammaBrightness = R_EacpGammaBrightness( postProcessPass );

	drawProgram = draw.program;
	drawPipeline = draw.pipeline;

	RB_DrawElementsWithCounters( tri );
}

/*
======================
idRenderBackendEacp::DrawBumpyReflectStage

bumpyEnvironment.vfp: the same reflection through the material's normal map, in
global space.

Two textures, so two samplings in the program's key, and the bump map is bound
straight off the bump stage rather than through RB_BindVariableStageImage - which
is what RB_PrepareStageTexturing does too, a bump stage having no cinematic or
dynamic form to resolve.
======================
*/
void idRenderBackendEacp::DrawBumpyReflectStage( const srfTriangles_t *tri,
												 const eacpStage_t &stage ) {
	const GPU::Texture *	cube = TextureForStage( stage.image, true );
	const GPU::Texture *	bump = TextureForStage( stage.bumpImage, false );

	if ( !cube || !bump ) {
		return;
	}

	idEacpRenderProgs::bumpyReflectDraw_t	draw =
		eacpRenderProgs.BumpyReflectDraw( stage.image, stage.bumpImage, stage.stateBits,
										  ViewCull( stage.cullType ) );

	if ( !draw.pipeline ) {
		return;
	}

	draw.program->modelViewProjection = asFloat4x4( modelViewProjection );
	draw.program->localViewOrigin = LocalViewOrigin();

	// program.env[6], [7] and [8]: the model matrix's rows, which are its
	// columns read across - Doom 3 keeps matrices in OpenGL's column-major
	// order, so row i is elements i, i+4, i+8, i+12.
	const float *	model = backEnd.currentSpace->modelMatrix;

	draw.program->modelRowX = asFloat4( model[0], model[4], model[8], model[12] );
	draw.program->modelRowY = asFloat4( model[1], model[5], model[9], model[13] );
	draw.program->modelRowZ = asFloat4( model[2], model[6], model[10], model[14] );

	// No colour at all, which is what the original writes - see the program.

	draw.program->cubeImage = *cube;
	draw.program->bumpImage = *bump;

	draw.program->gammaBrightness = R_EacpGammaBrightness( postProcessPass );

	drawProgram = draw.program;
	drawPipeline = draw.pipeline;

	RB_DrawElementsWithCounters( tri );
}

/*
======================
idRenderBackendEacp::DrawScreenStage

TG_SCREEN and TG_SCREEN2: the surface sampling whatever the stage names at the
point on the screen it covers, which is almost always `_currentRender`.

The three planes are rows 0, 1 and 3 of modelView x projection, exactly as
RB_PrepareStageTexturing builds them - and this is object-linear texgen, so they
are dotted with the vertex in its own coordinates rather than in the eye's, which
is what makes the result the clip-space position and the whole thing work.

**Unmeasured, and it has to be said.** No material in the demo pk4 declares
`texgen screen` at all outside a newStage, so nothing in the three maps draws
through this and there is no reference picture to compare against. What it has
instead is the copy step 4e.3 built, whose own note about `uploadWidth` and the
power-of-two padding is what the plane rows above would have to be scaled by if
the image sampled here were ever smaller than the frame - and it is not, on this
backend, because CopyFramebufferToImage keeps the padded size the shared code
expects.
======================
*/
void idRenderBackendEacp::DrawScreenStage( const srfTriangles_t *tri,
										   const eacpStage_t &stage ) {
	const GPU::Texture *	texture = TextureForStage( stage.image, false );

	if ( !texture ) {
		return;
	}

	idEacpRenderProgs::screenDraw_t	draw =
		eacpRenderProgs.ScreenDraw( stage.image, stage.stateBits,
									ViewCull( stage.cullType ) );

	if ( !draw.pipeline ) {
		return;
	}

	draw.program->modelViewProjection = asFloat4x4( modelViewProjection );

	float	mat[16];

	myGlMultMatrix( backEnd.currentSpace->modelViewMatrix,
					backEnd.viewDef->projectionMatrix, mat );

	draw.program->screenPlaneS = asFloat4( mat[0], mat[4], mat[8], mat[12] );
	draw.program->screenPlaneT = asFloat4( mat[1], mat[5], mat[9], mat[13] );
	draw.program->screenPlaneQ = asFloat4( mat[3], mat[7], mat[11], mat[15] );

	// The same two rows the explicit stage sends, and they mean the same thing -
	// what differs is the third component they are dotted with, which is the
	// generated q here and the constant 1 there. The program is where that is
	// said.
	if ( stage.textureMatrix ) {
		const float *	matrix = stage.textureMatrix;

		draw.program->textureMatrixS = asFloat4( matrix[0], matrix[4], matrix[12], 0.0f );
		draw.program->textureMatrixT = asFloat4( matrix[1], matrix[5], matrix[13], 0.0f );
	} else {
		draw.program->textureMatrixS = asFloat4( 1.0f, 0.0f, 0.0f, 0.0f );
		draw.program->textureMatrixT = asFloat4( 0.0f, 1.0f, 0.0f, 0.0f );
	}

	StageColor( stage, true, draw.program->colorModulate, draw.program->colorAdd );

	draw.program->image = *texture;

	drawProgram = draw.program;
	drawPipeline = draw.pipeline;

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

/*
====================
idRenderBackendEacp::ReplaceTexture

The one place an image's texture changes hands, and the only place ownership has
to be carried by hand rather than by a type.

idImage::backendTexture is a void * in a header both backends compile, which is
what keeps eacp's types out of Doom 3's headers - so what an idImage holds
cannot itself be an owning pointer. What it can be is owned on both sides of
this function: the incoming one arrives owned and is released into the field,
and the outgoing one is adopted by a local that frees it as it goes out of
scope. Neither end of the swap names a delete, and there is no window in which
one of them is owned twice.
====================
*/
void idRenderBackendEacp::ReplaceTexture( idImage *image,
										  OwningPointer<GPU::Texture> texture ) {
	const OwningPointer<GPU::Texture>	previous( (GPU::Texture *)image->backendTexture );

	image->backendTexture = texture.release();
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

**A cube arrives as six of these calls and leaves as one texture**, which is the
one place this function is not a single upload. GenerateCubeImage uploads level 0
of faces 0 through 5 in order and then their mip chains; eacp takes a cube as one
block of six faces, in that same order, so the six are accumulated into a buffer
and the texture is created on the sixth. The buffer is a member rather than a
local for exactly that reason - the calls are separate and the creation is not.
====================
*/
void idRenderBackendEacp::UploadImageLevel( idImage *image, int face, int level, int internalFormat,
											int width, int height, int externalFormat,
											const byte *pixels ) {
	if ( level != 0 ) {
		return;
	}

	if ( image->type != TT_CUBIC && face != 0 ) {
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

	const int	facePixels = width * height;

	if ( image->type == TT_CUBIC ) {
		// Six faces of one size, so a face arriving with a different size than
		// the run in progress is a new cube and the buffer starts again. Face 0
		// does the same thing, which is what makes an interrupted upload - an
		// image reloaded over itself - start clean rather than mix two cubes.
		const int	needed = facePixels * 4 * 6;

		if ( face == 0 || cubeFaceBytes != needed ) {
			cubeFaces.SetNum( needed, false );
			cubeFaceBytes = needed;
			cubeFacesFilled = 0;
		}

		if ( face < 0 || face > 5 ) {
			return;
		}

		R_EacpSwizzleToRGBA( cubeFaces.Ptr() + face * facePixels * 4, pixels,
							 facePixels, internalFormat );

		cubeFacesFilled |= 1 << face;

		// Created on the last one, and only once all six are in - a cube whose
		// loader gave up part way through leaves the image with no texture,
		// which DrawStage reports rather than drawing something half-built.
		if ( cubeFacesFilled != 0x3f ) {
			return;
		}

		GPU::TextureDescriptor	descriptor;

		descriptor.width = width;
		descriptor.height = height;
		descriptor.format = GPU::TextureFormat::RGBA8Unorm;
		descriptor.cube = true;
		descriptor.mipmapped = ( image->filter == TF_DEFAULT );

		ReplaceTexture( image, makeOwned<GPU::Texture>( GPU::Device::shared(), descriptor,
														cubeFaces.Ptr() ) );
		return;
	}

	byte *		swizzled = (byte *)R_StaticAlloc( facePixels * 4 );
	R_EacpSwizzleToRGBA( swizzled, pixels, facePixels, internalFormat );

	GPU::TextureDescriptor	descriptor;

	descriptor.width = width;
	descriptor.height = height;
	descriptor.format = GPU::TextureFormat::RGBA8Unorm;

	// Only where something will sample them. TF_DEFAULT is Doom 3's name for
	// "the mipmapped one"; TF_LINEAR and TF_NEAREST both sample level 0 alone,
	// so a chain built for them is a third more upload and memory for nothing.
	descriptor.mipmapped = ( image->filter == TF_DEFAULT );

	ReplaceTexture( image, makeOwned<GPU::Texture>( GPU::Device::shared(), descriptor,
													swizzled ) );

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
	if ( cols <= 0 || rows <= 0 || data == NULL ) {
		return;
	}

	// A cube map animation - six square faces stacked into one tall image, which
	// is how the GL path recognises one too. The bytes are already exactly what
	// eacp takes: six faces of cols x cols, one after another, in the order a
	// cube is uploaded in. So this is one call rather than the six
	// glTexSubImage2Ds the GL backend makes.
	if ( rows == cols * 6 ) {
		if ( image->type != TT_CUBIC ) {
			image->type = TT_CUBIC;
			ReplaceTexture( image, NULL );
		}

		rows /= 6;

		GPU::Texture *	cube = TextureFor( image );

		if ( cube && cube->isCube() && cube->width() == cols && cube->height() == rows ) {
			cube->update( data );
		} else {
			GPU::TextureDescriptor	descriptor;

			descriptor.width = cols;
			descriptor.height = rows;
			descriptor.format = GPU::TextureFormat::RGBA8Unorm;
			descriptor.cube = true;

			ReplaceTexture( image, makeOwned<GPU::Texture>( GPU::Device::shared(),
															descriptor, data ) );
		}

		image->uploadWidth = cols;
		image->uploadHeight = rows;

		BindImage( image );
		return;
	}

	image->type = TT_2D;

	GPU::Texture *	texture = TextureFor( image );

	// The shape is part of the match, not only the size: a cinematic that was a
	// cube map animation and is now a flat one would otherwise be re-uploaded
	// into six faces through an entry point that hands over one image.
	if ( texture && !texture->isCube()
		 && texture->width() == cols && texture->height() == rows ) {
		texture->update( data );
	} else {
		GPU::TextureDescriptor	descriptor;

		descriptor.width = cols;
		descriptor.height = rows;
		descriptor.format = GPU::TextureFormat::RGBA8Unorm;

		ReplaceTexture( image, makeOwned<GPU::Texture>( GPU::Device::shared(), descriptor,
														data ) );
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
	// Nothing here either, but the cube's own rule is not the same as the 2D
	// one and it does not disappear - it moves. The GL version forces
	// GL_CLAMP_TO_EDGE whatever the material declared, because "no other clamp
	// mode makes sense" across a seam; here the address mode is baked into a
	// shader, so forcing it is choosing which compiled variant the draw goes
	// through. R_EacpSampling in RenderProgs_Eacp.cpp is where that happens.
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

_currentRender and _scratch: the frame so far, into an image's own texture. Step
4e.3, and it is the same blit PresentFrameTarget does with the destination
changed - which is why it needed 4e.1 first. On OpenGL the frame is in the back
buffer and glCopyTexSubImage2D reads it; here it is in a texture this backend
owns, and a texture is copied by drawing it.

**The rectangle is GL's, so it arrives upside down and stays that way.** x and y
measure from the bottom left of the frame, and glCopyTexImage2D puts the row at
y into the destination's row 0 - so what a material samples at t = 0 is the
*bottom* of the screen. Every caller is written against that: the wipe and the
player-view effects draw _scratch with t going 1 to 0. So the quad below maps
the source region's bottom edge onto the destination's first row, which is the
same picture the GL build produces rather than the same one flipped.

**The power-of-two dance is kept even though nothing here needs it.** eacp
samples a texture of any size, but uploadWidth and uploadHeight are what the
renderer above the seam scales screen coordinates by (RB_SetProgramEnvironment's
first two parameters), so a backend that sized these differently would be
answering a question the shared code asks in another unit. The extra row and
column GL duplicates along the edges go with it, for the bilerp that samples
just past the copied region.

Nothing is drawn here if the destination pipeline would not compile, which is
the same answer every other draw gives - the picture is missing rather than
wrong.
====================
*/
void idRenderBackendEacp::CopyFramebufferToImage( idImage *image, int x, int y,
												  int imageWidth, int imageHeight,
												  bool useOversizedBuffer ) {
	backEnd.c_copyFrameBuffer++;

	if ( image == NULL || !eacpFrame || !frameTarget ) {
		// Outside a frame, which is where a level load's own screen update and
		// a console command run. There is nothing composed to copy.
		if ( !warnedCopyFramebuffer ) {
			warnedCopyFramebuffer = true;
			common->Warning( "eacp: the frame can only be copied into an image "
							 "from inside one" );
		}

		return;
	}

	if ( cvarSystem->GetCVarBool( "g_lowresFullscreenFX" ) ) {
		imageWidth = 512;
		imageHeight = 512;
	}

	int	potWidth = MakePowerOfTwo( imageWidth );
	int	potHeight = MakePowerOfTwo( imageHeight );

	image->GetDownsize( imageWidth, imageHeight );
	image->GetDownsize( potWidth, potHeight );

	if ( imageWidth < 1 || imageHeight < 1 ) {
		return;
	}

	// The region in the target's own rows, which run the other way. Refused
	// rather than clamped if it does not fit, for the reason eacp's own
	// Texture::update gives: a clamped region goes on reading at the width it
	// was asked for and quietly produces a skewed picture.
	const int	top = frameTargetHeight - ( y + imageHeight );

	if ( x < 0 || top < 0 || x + imageWidth > frameTargetWidth
		 || y + imageHeight > frameTargetHeight ) {
		return;
	}

	idEacpRenderProgs::blitDraw_t	draw = eacpRenderProgs.CaptureDraw();

	if ( !draw.pipeline ) {
		return;
	}

	GPU::Texture *	texture = TextureFor( image );

	// Only resize if what is there cannot hold it at all, or - when the caller
	// is not asking for an oversized buffer - if it is not the size asked for.
	// The first is what stops a subview rendering at one size from thrashing
	// the texture a full view left. The third case is this backend's own: an
	// image that has only ever been uploaded to has a texture that cannot be
	// rendered into, whatever size it is.
	const bool	rebuild = texture == NULL
		|| !texture->isRenderTarget()
		|| ( useOversizedBuffer
			 && ( image->uploadWidth < potWidth || image->uploadHeight < potHeight ) )
		|| ( !useOversizedBuffer
			 && ( image->uploadWidth != potWidth || image->uploadHeight != potHeight ) );

	if ( rebuild ) {
		image->uploadWidth = potWidth;
		image->uploadHeight = potHeight;

		GPU::TextureDescriptor	descriptor;

		descriptor.width = potWidth;
		descriptor.height = potHeight;
		descriptor.format = GPU::TextureFormat::RGBA8Unorm;
		descriptor.renderTarget = true;

		ReplaceTexture( image, makeOwned<GPU::Texture>( GPU::Device::shared(),
														descriptor, (const void *)NULL ) );

		texture = TextureFor( image );

		if ( !texture->isValid() || !texture->isRenderTarget() ) {
			common->Warning( "eacp: no %ix%i render target for image '%s'",
							 potWidth, potHeight, image->imgName.c_str() );
			ReplaceTexture( image, NULL );
			return;
		}
	}

	image->type = TT_2D;

	// What the GL backend sets on the texture object at this same point, said
	// in the two fields this backend reads its sampling from - eacp bakes the
	// sampler into the shader, so an image's filter and repeat are not state to
	// set on it but the choice of which compiled variant samples it. Linear and
	// clamp either way, which is what a screen-sized copy wants and is not what
	// the image was created with: CaptureRenderToImage asks for TR_REPEAT.
	image->filter = TF_LINEAR;
	image->repeat = TR_CLAMP;

	// The pass this is drawn from cannot be the one that is drawing the frame,
	// so the frame's pass is put down here and picked up at the end with
	// everything it had - see SuspendPass.
	const bool	resume = SuspendPass();

	draw.program->image = *frameTarget;

	GPU::RenderPassDescriptor	descriptor;

	// Cleared only when the texture is new, which is where GL memsets its
	// power-of-two allocation to zero; a texture that is being copied over
	// again keeps whatever the last copy left outside the region, exactly as
	// glCopyTexSubImage2D does.
	descriptor.clear = rebuild;

	GPU::RenderPass	into = eacpFrame->beginPass( *texture, descriptor );

	// The region, then the row and the column GL duplicates past its edges when
	// the copy does not fill the power-of-two allocation - so a bilerp one texel
	// outside the picture reads the picture's own edge rather than the zeroes
	// beside it.
	CopyFrameRegion( into, draw, x, y, imageWidth, imageHeight,
					 0, 0, imageWidth, imageHeight );

	if ( imageWidth != potWidth ) {
		CopyFrameRegion( into, draw, x + imageWidth - 1, y, 1, imageHeight,
						 imageWidth, 0, 1, imageHeight );
	}

	if ( imageHeight != potHeight ) {
		CopyFrameRegion( into, draw, x, y + imageHeight - 1, imageWidth, 1,
						 0, imageHeight, imageWidth, 1 );
	}

	into.end();

	if ( resume ) {
		ResumePass();
	}
}

/*
====================
idRenderBackendEacp::CopyFrameRegion

One rectangle of the frame target onto one rectangle of an image, and the whole
of what a "copy" is on a backend that has no copy: a quad, a viewport and two
texture coordinates.

The source is in GL's coordinates - measured up from the bottom - because that
is what CopyFramebufferToImage was handed; the destination is in the image's own
rows, which run down from the top. So the source's *bottom* edge is the
destination's first row, and that inversion is the whole reason the two
rectangles are not simply the same numbers.
====================
*/
void idRenderBackendEacp::CopyFrameRegion( GPU::RenderPass &into,
										   const idEacpRenderProgs::blitDraw_t &draw,
										   int srcX, int srcY, int srcWidth, int srcHeight,
										   int dstX, int dstY, int dstWidth, int dstHeight ) {
	const float	s0 = (float)srcX / (float)frameTargetWidth;
	const float	s1 = (float)( srcX + srcWidth ) / (float)frameTargetWidth;

	// The region's bottom edge and its top edge, as rows of the target.
	const float	tBottom = (float)( frameTargetHeight - srcY ) / (float)frameTargetHeight;
	const float	tTop = (float)( frameTargetHeight - ( srcY + srcHeight ) )
					   / (float)frameTargetHeight;

	const eacpBlitVert_t	quad[6] = {
		{ { -1.0f,  1.0f }, { s0, tBottom } },
		{ { -1.0f, -1.0f }, { s0, tTop } },
		{ {  1.0f, -1.0f }, { s1, tTop } },
		{ { -1.0f,  1.0f }, { s0, tBottom } },
		{ {  1.0f, -1.0f }, { s1, tTop } },
		{ {  1.0f,  1.0f }, { s1, tBottom } },
	};

	const GPU::Buffer &	vertices = eacpRenderProgs.StreamVertices( quad, sizeof( quad ) );

	into.setViewport( Graphics::Rect( (float)dstX, (float)dstY,
									  (float)dstWidth, (float)dstHeight ) );

	into.setPipeline( *draw.pipeline );
	into.setVertexBuffer( vertices );

	draw.program->bindTextures( into );

	into.draw( 6 );
}

void idRenderBackendEacp::CopyDepthbufferToImage( idImage *image, int x, int y,
												  int imageWidth, int imageHeight,
												  bool useOversizedBuffer ) {
	// #3877's early depth capture, which feeds _currentDepthImage to the soft
	// particle program - and that program is behind BE_ARB2, so nothing on this
	// backend samples what this would write. r_enableDepthCapture is what asks,
	// and FillDepthBuffer says where it does not.
}

/*
====================
idRenderBackendEacp::ReadPixels

The frame back on the CPU, which on this backend is the render target step 4e.1
composes into read through eacp's Texture::read. There is no back buffer to ask
for and never was: what OpenGL keeps in one, this keeps in a texture it owns,
which is why 4e.1 had to land before this could.

`presented` is OpenGL's question and has no answer here. The target is not
swapped or discarded when the drawable is drawn from it, so the frame that is on
the screen and the frame that has been composed are the same pixels in the same
texture - which is what the seam says the distinction is worth on a backend that
composes into its own target.

**The two calls before the copy are not optional and are not the same thing.**
Ending the pass is because a texture cannot be read while it is the thing being
rendered into. Flushing the frame is because a frame's commands do not reach the
GPU until it ends, so without it the copy would return the frame before this one
- and the game asks for this from inside the frame it wants, every time.

What comes out is OpenGL's layout rather than the texture's, because that is
what the callers unpack: rows from the bottom up, three bytes a pixel, each row
padded to four.
====================
*/
bool idRenderBackendEacp::ReadPixels( int x, int y, int width, int height, byte *rgb,
									  bool presented ) {
	if ( rgb == NULL || width < 1 || height < 1 ) {
		return false;
	}

	if ( !eacpFrame || !frameTarget ) {
		// Outside a frame, which is where a console command or a level load's
		// own screen update runs. There is nothing composed to read.
		if ( !warnedReadPixels ) {
			warnedReadPixels = true;
			common->Warning( "eacp: the frame can only be read back from inside one" );
		}

		return false;
	}

	// Doom 3 measures from the bottom left of the frame; a texture's rows start
	// at the top.
	const int	top = frameTargetHeight - ( y + height );

	if ( x < 0 || top < 0 || x + width > frameTargetWidth
		 || y + height > frameTargetHeight ) {
		return false;
	}

	const bool	resume = SuspendPass();

	eacpFrame->flush();

	byte *	bgra = (byte *)R_StaticAlloc( width * height * 4 );

	frameTarget->read( Graphics::Rect( (float)x, (float)top,
									   (float)width, (float)height ), bgra );

	if ( resume ) {
		ResumePass();
	}

	const int	row = ( width * 3 + 3 ) & ~3;

	for ( int r = 0 ; r < height ; r++ ) {
		const byte *	src = bgra + ( height - 1 - r ) * width * 4;
		byte *			dst = rgb + r * row;

		for ( int c = 0 ; c < width ; c++ ) {
			dst[ c * 3 + 0 ] = src[ c * 4 + 2 ];
			dst[ c * 3 + 1 ] = src[ c * 4 + 1 ];
			dst[ c * 3 + 2 ] = src[ c * 4 + 0 ];
		}
	}

	R_StaticFree( bgra );

	return true;
}
