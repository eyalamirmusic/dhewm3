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

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __GLFORMATS_H__
#define __GLFORMATS_H__

/*
================================================================================

	The pixel formats, by OpenGL's names and OpenGL's values, owned by Doom 3.

	Step 5 deleted qgl.h, and with it the <SDL3/SDL_opengl.h> that used to
	supply these. They do not go with it, and RenderBackend.h says why at
	length: Doom 3's format decision is written in these names from
	SelectInternalFormat down through BitsForInternalFormat, the .dds reader
	and the precompressed-file writer, and half of the enum is a *file*
	format's as much as an API's - the .dds files in the pk4s are what
	GL_COMPRESSED_RGBA_S3TC_DXT5_EXT describes. A backend maps them onto what
	it has, which is much the smaller job than moving the decision.

	So the names and the values stay, and this is where they live now. The
	values are OpenGL's, unchanged, because .dds and idImage both round-trip
	them; the names keep their GL_ prefix because renaming forty constants
	would say something about this port that is not true.

================================================================================
*/

enum {
	// Base internal formats and the external formats that go with them.
	GL_ALPHA					= 0x1906,
	GL_RGB						= 0x1907,
	GL_RGBA						= 0x1908,
	GL_LUMINANCE				= 0x1909,
	GL_LUMINANCE_ALPHA			= 0x190A,
	GL_INTENSITY				= 0x8049,
	GL_COLOR_INDEX				= 0x1900,

	// Sized internal formats.
	GL_ALPHA8					= 0x803C,
	GL_LUMINANCE8				= 0x8040,
	GL_LUMINANCE8_ALPHA8		= 0x8045,
	GL_INTENSITY8				= 0x804B,
	GL_RGB5						= 0x8050,
	GL_RGB8						= 0x8051,
	GL_RGBA4					= 0x8056,
	GL_RGBA8					= 0x8058,
	GL_COLOR_INDEX8_EXT			= 0x80E5,

	// Byte orders a .dds can arrive in.
	GL_BGR_EXT					= 0x80E0,
	GL_BGRA_EXT					= 0x80E1,

	// Compressed formats. The four S3TC ones are contiguous and the code
	// range-tests them, so their order is load-bearing.
	GL_COMPRESSED_RGB_S3TC_DXT1_EXT		= 0x83F0,
	GL_COMPRESSED_RGBA_S3TC_DXT1_EXT	= 0x83F1,
	GL_COMPRESSED_RGBA_S3TC_DXT3_EXT	= 0x83F2,
	GL_COMPRESSED_RGBA_S3TC_DXT5_EXT	= 0x83F3,
	GL_COMPRESSED_RGB_ARB				= 0x84ED,
	GL_COMPRESSED_RGBA_ARB				= 0x84EE,
	GL_COMPRESSED_RGBA_BPTC_UNORM		= 0x8E8C,
	GL_COMPRESSED_RGBA_BPTC_UNORM_ARB	= 0x8E8C,

	// Minification and magnification filters. image_filter names these in the
	// console and archives the name, so they are part of the config format too.
	GL_NEAREST					= 0x2600,
	GL_LINEAR					= 0x2601,
	GL_NEAREST_MIPMAP_NEAREST	= 0x2700,
	GL_LINEAR_MIPMAP_NEAREST	= 0x2701,
	GL_NEAREST_MIPMAP_LINEAR	= 0x2702,
	GL_LINEAR_MIPMAP_LINEAR		= 0x2703,

	// The one draw buffer left. RC_SET_BUFFER still carries it; see
	// idRenderSystemLocal::BeginFrame.
	GL_BACK						= 0x0405,

	// The two ARB program targets. Only R_FindARBProgram's stub takes them,
	// and only so that a newStage material parses into the shape it always did.
	GL_VERTEX_PROGRAM_ARB		= 0x8620,
	GL_FRAGMENT_PROGRAM_ARB		= 0x8804
};

#endif	/* !__GLFORMATS_H__ */
