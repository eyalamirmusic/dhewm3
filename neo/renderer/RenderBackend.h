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

class idRenderBackend {
public:
	virtual					~idRenderBackend() {}

	// What this backend is called, for the log line R_InitOpenGL prints and for
	// anyone reading a bug report.
	virtual const char *	Name( void ) const = 0;

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
	// renderer still drives ImGui and r_finish around this.
	virtual void			SwapBuffers( void ) = 0;

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

	// Report anything the API has queued up against us. A no-op on backends
	// that report errors as they happen instead.
	virtual void			CheckErrors( void ) = 0;
};

extern idRenderBackend *	renderBackend;

#endif /* !__RENDERBACKEND_H__ */
