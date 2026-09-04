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

#ifndef __RENDERBACKEND_H__
#define __RENDERBACKEND_H__

/*
===============================================================================

	idRenderBackend

	The seam between the renderer and the graphics API.

	Doom 3's backend is 2000-odd qgl call sites, but they are not 2000 distinct
	things: the parts that decide *what the GPU does* were already funnelled
	through a handful of functions by the original authors, and those functions
	are what this interface is. Draw state arrives as one abstract bitfield
	rather than as GL enums, all indexed geometry leaves through one call, and
	the texture unit is selected in one place.

	Everything here is a choke point that already existed. Nothing was invented
	to make the interface look tidy, and nothing that has not yet been funnelled
	is pretended to be - the fixed-function matrix stack, the ARB programs,
	texture upload and the immediate-mode debug tools still talk to GL directly,
	and porting each of them is its own piece of work.

	The point of naming the seam now, while GL is still the only thing behind
	it, is that the port stops being a rewrite that either lands or does not:
	the game keeps running and keeps being measurable at every step. See
	regression/README.md for what measures it.

===============================================================================
*/

typedef struct srfTriangles_s srfTriangles_t;
typedef struct vertCache_s vertCache_t;
class idImage;

// Dear ImGui's finished frame - the vertex and index buffers, the draw lists,
// the clip rectangles and the textures the settings menu wants uploaded. Named
// rather than included because this header is the seam and Dear ImGui is not
// the renderer's business: sys/sys_imgui.cpp owns the menu, calls ImGui::Render
// and hands the result here.
struct ImDrawData;

// Which of the frontend's paths a backend consumes. Doom 3 had one per family
// of hardware; all but ARB2 were deleted long before this port, and step 5
// deleted that one too, so there is one left. It is not only a backend's own
// business, which is why it is named out here: the *frontend* branches on
// tr.backEndRendererHasVertexPrograms in tr_light.cpp and tr_stencilshadow.cpp
// and builds different data accordingly, so it has to be told which one is
// running.
typedef enum {
	BE_EACP,
	BE_BAD
} backEndName_t;

class idRenderBackend {
public:
	virtual					~idRenderBackend() {}

	// What this backend is called, for the log line R_InitOpenGL prints and for
	// anyone reading a bug report.
	virtual const char *	Name( void ) const = 0;

	// The path above, once Init has run - BE_BAD from a backend that came up
	// unable to draw, which R_InitOpenGL treats as fatal.
	virtual backEndName_t	Path( void ) const = 0;

	// Everything R_InitOpenGL does that belongs to the graphics API rather than
	// to the renderer, run once the window exists: entry points, the device's
	// own limits into glConfig, and whatever programs the backend draws with.
	//
	// Setting glConfig.isInitialized is the backend's last act here, because it
	// is the backend saying it is ready - nothing above it can tell.
	virtual void			Init( void ) = 0;

	// Release whatever Init acquired. Called before GLimp_Shutdown takes the
	// window away, so anything holding a device or a surface has to go here.
	virtual void			Shutdown( void ) = 0;

	// One view - the world, a mirror, a subview or the 2D pass - rendered from
	// backEnd.viewDef, which RB_DrawView has already set along with the frame
	// bookkeeping that goes with it.
	//
	// This is the whole of a backend's drawing and it is deliberately not
	// broken down further. What Doom 3 does inside a view - fill depth, add
	// each light's interactions through the stencil, blend the shader passes,
	// fog - is a sequence of ideas rather than a sequence of API calls, and
	// every one of them is expressed in fixed-function terms that a modern API
	// has no counterpart for. So a second backend reimplements the sequence
	// rather than reimplementing calls underneath it.
	virtual void			DrawView( void ) = 0;

	// The frame's last act, and the mirror of SetDefaultState below: hand the
	// API back to whatever else in the program touches it. Doom 3 shares its
	// context with the editor, so a texture left bound by the last draw is a
	// texture the editor can corrupt.
	virtual void			ReleaseTextures( void ) = 0;

	// Reset everything the rest of the program might have left dirty. Doom 3
	// hands the context to the editor and to ImGui, so the backend cannot
	// assume it is the only thing that touched the API.
	virtual void			SetDefaultState( void ) = 0;

	// Where the frame goes, and the debug clear that goes with it.
	virtual void			SetDrawBuffer( int buffer ) = 0;

	// Present. Does not include the frame's own end-of-frame work - the
	// renderer still drives ImGui around this.
	virtual void			SwapBuffers( void ) = 0;

	// Dear ImGui's renderer backend: the settings menu drawn into the frame the
	// engine has just finished, from RB_SwapBuffers and therefore before the
	// present above.
	//
	// It is on the seam rather than in sys/sys_imgui.cpp because that file is
	// the menu and this is a renderer - triangles with a texture, a scissor
	// rectangle and a blend, which is a thing only a backend can say. ImGui
	// ships an OpenGL backend and a Metal one and neither fits: the port's
	// renderer is an idRenderBackend rather than either API, and what the menu
	// has to be drawn *into* is the frame this backend is composing.
	//
	// A backend implements it by doing whatever its API's own Dear ImGui backend
	// does, plus the two things that being *inside somebody else's frame* adds:
	// the draws land in the target the frame is composed into, and whatever
	// state the last view left - a viewport, a scissor - is given back first.
	//
	// What to declare on the ImGuiIO about it is the host's business rather than
	// this file's, because only the host knows which backend it linked; here
	// that is sys/eacp/ImGuiBackend.cpp.
	//
	// A no-op on a backend that has no answer, which is what the whole of this
	// interface does with a path it has not been taught.
	virtual void			DrawImGui( ImDrawData *drawData ) = 0;

	// The GLS_* bitfield from tr_local.h: blend, depth func, depth and colour
	// masks, alpha test, polygon mode. This is the whole of Doom 3's per-draw
	// pipeline state, and it is already API-independent - which is why it is
	// the one piece of the port that needs translating rather than rewriting.
	virtual void			SetState( int stateBits ) = 0;

	// Forget the cached state so the next SetState writes every field. Needed
	// whenever something outside the backend has touched the API.
	virtual void			ClearStateDelta( void ) = 0;

	// CT_FRONT_SIDED / CT_BACK_SIDED / CT_TWO_SIDED, with the flip a mirrored
	// view needs folded in.
	virtual void			SetCull( int cullType ) = 0;

	// Fixed-function texture combiner mode. Has no counterpart in a modern API
	// and does not survive the port - it is here because it is state, and
	// leaving it out would have made the seam look cleaner than it is.
	virtual void			SetTexEnv( int env ) = 0;

	// Which texture unit subsequent binds and texture state apply to.
	virtual void			SelectTexture( int unit ) = 0;

	// Every indexed draw in the renderer arrives here. numIndexes is passed
	// separately because the shadow path deliberately draws less of the surface
	// than it holds, depending on whether the caps are needed.
	virtual void			DrawIndexed( const srfTriangles_t *tri, int numIndexes ) = 0;

	// A vertex cache block is about to stop existing, and whatever the backend
	// put on the GPU for it has to go with it. Called from
	// idVertexCache::ActuallyFree, which is the only place a block dies, and
	// only for a block whose backendBuffer is not NULL - so a backend that
	// never puts anything there never hears from it.
	//
	// It is the mirror of FreeImage: the block is the identity, the backend
	// owns whatever hangs off it, and this is the ONLY place that is released.
	// The block is still readable here and stops being so immediately after.
	virtual void			FreeVertexCacheBuffer( vertCache_t *block ) = 0;

	// Report anything the API has queued up against us. A no-op on backends
	// that report errors as they happen instead.
	virtual void			CheckErrors( void ) = 0;

	/*
	-------------------------------------------------------------------------
		Images

		idImage decides which pixels reach the GPU - the downsizing, the
		format choice, the mip chain, the LRU that keeps a texture resident
		and the purge that does not. None of that is repeated per backend.
		What is below is the part that puts them there and takes them away.

		The formats are OpenGL's names for things - GL_RGBA8, GL_ALPHA8,
		GL_COMPRESSED_RGBA_S3TC_DXT5_EXT - and that is deliberate rather than
		unfinished. Doom 3's format decision is written in them from
		SelectInternalFormat down through BitsForInternalFormat, the .dds
		reader and the precompressed-file writer, and the .dds files in the
		pk4s are the reason: half of that enum is a file format's as much as
		an API's. A second backend maps them onto what it has, which is much
		the smaller job than moving the decision.
	-------------------------------------------------------------------------
	*/

	// A name for a texture that does not exist yet, and its release. The
	// release is the ONLY place a texture is ever destroyed.
	virtual void			AllocImage( idImage *image ) = 0;
	virtual void			FreeImage( idImage *image ) = 0;

	// Make this image the one the current texture unit samples. Bind goes
	// through the fixed-function texture enables, which the paths that predate
	// programs still need; BindFragment does not, because a program says for
	// itself which kind of map it wants.
	//
	// Both are the tail of idImage::Bind and idImage::BindFragment. The LRU
	// bookkeeping and the load-on-demand above them are the image layer's and
	// stay there.
	virtual void			BindImage( idImage *image ) = 0;
	virtual void			BindImageFragment( idImage *image ) = 0;

	// Nothing on this unit.
	virtual void			BindNoImage( void ) = 0;

	// One mip level of one face of the bound image. `pixels` is tightly packed
	// bytes in externalFormat's channel order - GL_RGBA for anything the engine
	// generated, GL_BGRA_EXT or GL_ALPHA for an uncompressed .dds - whatever
	// internalFormat asks the GPU to keep them as.
	//
	// `face` is 0 for a 2D image and 0..5 for a cube map's six. Which of the
	// two this is comes from image->type, which the caller has already set.
	virtual void			UploadImageLevel( idImage *image, int face, int level, int internalFormat,
											  int width, int height, int externalFormat,
											  const byte *pixels ) = 0;

	// The same for texels that arrive already compressed out of a .dds, where
	// the level's size in bytes is not implied by its dimensions.
	virtual void			UploadCompressedImageLevel( idImage *image, int level, int internalFormat,
														int width, int height, int numBytes,
														const byte *data ) = 0;

	// A .dds whose mip chain stops before 1x1 - the level below which nothing
	// was written, so that sampling does not fall off the end of it and come
	// back black.
	virtual void			SetImageMaxLevel( idImage *image, int maxLevel ) = 0;

	// Every level of every face of this image has now been handed over: the
	// three loaders that upload one - GenerateImage, GenerateCubeImage and
	// UploadPrecompressedImage - each call this as their last act, and nothing
	// else does.
	//
	// It exists because "upload a level" and "the upload is finished" are not
	// the same statement and OpenGL never had to distinguish them. glTexImage2D
	// takes a level into a texture object that already exists, so a chain can
	// arrive a level at a time with no end marker at all; an API that fixes a
	// texture's level count when the resource is created cannot create anything
	// until it knows how many levels are coming. A backend on such an API
	// accumulates the levels and builds the texture here.
	//
	// Naming it rather than giving SetImageFilterAndRepeat the second meaning is
	// the deliberate half. That call *looks* like the end of an upload in two of
	// the three loaders, and is not: GenerateCubeImage makes its own
	// SetCubeImageFilterAndRepeat call *before* uploading a single face. A seam
	// whose contract depends on which loader is running is a seam that will be
	// read wrong.
	//
	// A no-op on a backend that creates its texture as the first level arrives,
	// which is what the OpenGL one did.
	virtual void			FinishImage( idImage *image ) = 0;

	// The video and cinematic path: a whole image replaced every frame at a
	// size that usually has not changed, which is worth telling the API
	// because it is what lets it keep the storage it already has.
	virtual void			UploadScratchImage( idImage *image, const byte *data, int cols, int rows ) = 0;

	// The filtering and wrapping the image asks for. Two entry points rather
	// than one because Doom 3 sets them differently on a cube map - forced
	// clamp, and none of the anisotropy or LOD bias - and folding the two
	// together would be a change of behaviour hiding inside a move.
	virtual void			SetImageFilterAndRepeat( const idImage *image ) = 0;
	virtual void			SetCubeImageFilterAndRepeat( const idImage *image ) = 0;

	// Re-read the global filtering cvars into every already-uploaded image,
	// which is what image_filter and image_anisotropy do when they change.
	virtual void			RefreshImageFilter( const idImage *image ) = 0;

	// The border a TR_CLAMP_TO_BORDER image samples outside itself. One user,
	// the generated _borderClamp image, whose own edge texels are already zero.
	virtual void			SetImageBorderColor( const idImage *image, const float rgba[4] ) = 0;

	// The frame, into an image: _currentRender for the post-process and
	// mirror passes, _currentDepth for soft particles. Whether the image has
	// to be reallocated to hold it is the backend's, because only the backend
	// knows what it already has.
	virtual void			CopyFramebufferToImage( idImage *image, int x, int y,
													int imageWidth, int imageHeight,
													bool useOversizedBuffer ) = 0;
	virtual void			CopyDepthbufferToImage( idImage *image, int x, int y,
													int imageWidth, int imageHeight,
													bool useOversizedBuffer ) = 0;

	// The frame, back on the CPU as RGB bytes with the origin at the bottom
	// left - which is OpenGL's convention and what R_WriteTGA's own
	// flipVertical argument is written against. Each row is padded to a
	// four-byte boundary, which is GL_PACK_ALIGNMENT's default and why the
	// caller allocates ( width + 3 ) * height * 3 rather than width * height *
	// 3; a backend that is not OpenGL still has to pad, because one of the two
	// callers unpacks the padding and the other assumes it is not there.
	//
	// `presented` says *which* frame is wanted. False is the one drawn but not
	// yet shown, which CaptureRenderToFile reads straight after issuing its
	// commands; true is the one already on the screen, which R_ReadTiledPixels
	// reads after session->UpdateScreen has swapped. The distinction is double
	// buffering's, so it belongs to OpenGL alone: a backend that composes its
	// frame into a target it owns has one picture either way, and nothing it
	// can do with the question but ignore it.
	//
	// False from a backend that cannot do it at all. The callers - the
	// objective camshots the game takes for its own UI, the screenshots and
	// aviDemo - would rather write nothing than crash.
	virtual bool			ReadPixels( int x, int y, int width, int height, byte *rgb,
										bool presented ) = 0;
};

extern idRenderBackend *	renderBackend;

#endif /* !__RENDERBACKEND_H__ */
