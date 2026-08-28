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

#include "sys/platform.h"

#include "renderer/tr_local.h"
#include "renderer/VertexCache.h"

/*
===============================================================================

	The OpenGL implementation of idRenderBackend.

	Every function body here was moved from tr_backend.cpp, tr_render.cpp and
	RenderSystem_init.cpp unchanged. Phase 1 of the eacp port is the seam and
	nothing else - a change of behaviour hiding inside a move is exactly what
	the demo gate exists to catch, so there is deliberately nothing for it to
	find here.

	The state cache still lives in backEnd.glState rather than in this class.
	It is read from outside the backend - idImage::Bind picks the current
	texture unit out of it, and the tools do too - so moving it is a change to
	those callers, which is Phase 2's work, not this phase's.

===============================================================================
*/

// A workaround for the default framebuffer's alpha channel, which is a property
// of how this backend presents and of nothing above it.
static idCVar r_fillWindowAlphaChan( "r_fillWindowAlphaChan", "-1", CVAR_SYSTEM | CVAR_NOCHEAT | CVAR_ARCHIVE, "Make sure alpha channel of windows default framebuffer is completely opaque at the end of each frame. Needed at least when using Wayland with older drivers.\n 1: do this, 0: don't do it, -1: let dhewm3 decide (default)" );

class idRenderBackendGL : public idRenderBackend {
public:
	virtual const char *Name( void ) const { return "OpenGL"; }
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
};

static idRenderBackendGL	renderBackendGL;
idRenderBackend *			renderBackend = &renderBackendGL;

/*
======================
idRenderBackendGL::Init

The API half of R_InitOpenGL: the entry points, what the driver says about
itself, and the ARB programs the interaction pass is written in. Everything the
renderer does with the answers - the vertex cache, the frame data, the gamma
ramp - stays above the seam, because none of it is OpenGL's.
======================
*/
void idRenderBackendGL::Init( void ) {
	GLint	temp;

// load qgl function pointers
#define QGLPROC(name, rettype, args) \
	q##name = (rettype(APIENTRYP)args)GLimp_ExtensionPointer(#name); \
	if (!q##name) \
		common->FatalError("Unable to initialize OpenGL (%s)", #name);

#include "renderer/qgl_proc.h"

	// get our config strings
	glConfig.vendor_string = (const char *)qglGetString(GL_VENDOR);
	glConfig.renderer_string = (const char *)qglGetString(GL_RENDERER);
	glConfig.version_string = (const char *)qglGetString(GL_VERSION);
	glConfig.extensions_string = (const char *)qglGetString(GL_EXTENSIONS);

	// OpenGL driver constants
	qglGetIntegerv( GL_MAX_TEXTURE_SIZE, &temp );
	glConfig.maxTextureSize = temp;

	// stubbed or broken drivers may have reported 0...
	if ( glConfig.maxTextureSize <= 0 ) {
		glConfig.maxTextureSize = 256;
	}

	glConfig.isInitialized = true;

	common->Printf("OpenGL vendor: %s\n", glConfig.vendor_string );
	common->Printf("OpenGL renderer: %s\n", glConfig.renderer_string );
	common->Printf("OpenGL version: %s\n", glConfig.version_string );

	// recheck all the extensions (FIXME: this might be dangerous)
	R_CheckPortableExtensions();

	// parse our vertex and fragment programs, possibly disably support for
	// one of the paths if there was an error
	R_ARB2_Init();

	cmdSystem->AddCommand( "reloadARBprograms", R_ReloadARBPrograms_f, CMD_FL_RENDERER, "reloads ARB programs" );
	R_ReloadARBPrograms_f( idCmdArgs() );
}

/*
======================
idRenderBackendGL::Shutdown

Nothing. The context owns every object this backend made and GLimp_Shutdown
destroys the context, which is the one thing a GL backend gets for free and the
one thing the eacp backend will not.
======================
*/
void idRenderBackendGL::Shutdown( void ) {
}

/*
======================
idRenderBackendGL::DrawView
======================
*/
void idRenderBackendGL::DrawView( void ) {
	RB_STD_DrawView();
}

/*
======================
idRenderBackendGL::ReleaseTextures
======================
*/
void idRenderBackendGL::ReleaseTextures( void ) {
	// go back to the default texture so the editor doesn't mess up a bound image
	qglBindTexture( GL_TEXTURE_2D, 0 );
	backEnd.glState.tmu[0].current2DMap = -1;
}

/*
======================
idRenderBackendGL::SetDefaultState

This should initialize all GL state that any part of the entire program
may touch, including the editor.
======================
*/
void idRenderBackendGL::SetDefaultState( void ) {
	int		i;

	qglClearDepth( 1.0f );
	qglColor4f (1,1,1,1);

	// the vertex array is always enabled
	qglEnableClientState( GL_VERTEX_ARRAY );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglDisableClientState( GL_COLOR_ARRAY );

	//
	// make sure our GL state vector is set correctly
	//
	memset( &backEnd.glState, 0, sizeof( backEnd.glState ) );
	backEnd.glState.forceGlState = true;

	qglColorMask( 1, 1, 1, 1 );

	qglEnable( GL_DEPTH_TEST );
	qglEnable( GL_BLEND );
	qglEnable( GL_SCISSOR_TEST );
	qglEnable( GL_CULL_FACE );
	qglDisable( GL_LIGHTING );
	qglDisable( GL_LINE_STIPPLE );
	qglDisable( GL_STENCIL_TEST );

	qglPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	qglDepthMask( GL_TRUE );
	qglDepthFunc( GL_ALWAYS );

	qglCullFace( GL_FRONT_AND_BACK );
	qglShadeModel( GL_SMOOTH );

	if ( r_useScissor.GetBool() ) {
		qglScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	}

	for ( i = glConfig.maxTextureUnits - 1 ; i >= 0 ; i-- ) {
		SelectTexture( i );

		// object linear texgen is our default
		qglTexGenf( GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR );
		qglTexGenf( GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR );
		qglTexGenf( GL_R, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR );
		qglTexGenf( GL_Q, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR );

		SetTexEnv( GL_MODULATE );
		qglDisable( GL_TEXTURE_2D );
		if ( glConfig.texture3DAvailable ) {
			qglDisable( GL_TEXTURE_3D );
		}
		if ( glConfig.cubeMapAvailable ) {
			qglDisable( GL_TEXTURE_CUBE_MAP_EXT );
		}
	}
}

/*
====================
idRenderBackendGL::SelectTexture
====================
*/
void idRenderBackendGL::SelectTexture( int unit ) {
	if ( backEnd.glState.currenttmu == unit ) {
		return;
	}

	if ( unit < 0 || (unit >= glConfig.maxTextureUnits && unit >= glConfig.maxTextureImageUnits) ) {
		common->Warning( "GL_SelectTexture: unit = %i", unit );
		return;
	}

	qglActiveTextureARB( GL_TEXTURE0_ARB + unit );
	qglClientActiveTextureARB( GL_TEXTURE0_ARB + unit );

	backEnd.glState.currenttmu = unit;
}

/*
====================
idRenderBackendGL::SetCull

This handles the flipping needed when the view being
rendered is a mirored view.
====================
*/
void idRenderBackendGL::SetCull( int cullType ) {
	if ( backEnd.glState.faceCulling == cullType ) {
		return;
	}

	if ( cullType == CT_TWO_SIDED ) {
		qglDisable( GL_CULL_FACE );
	} else  {
		if ( backEnd.glState.faceCulling == CT_TWO_SIDED ) {
			qglEnable( GL_CULL_FACE );
		}

		if ( cullType == CT_BACK_SIDED ) {
			if ( backEnd.viewDef->isMirror ) {
				qglCullFace( GL_FRONT );
			} else {
				qglCullFace( GL_BACK );
			}
		} else {
			if ( backEnd.viewDef->isMirror ) {
				qglCullFace( GL_BACK );
			} else {
				qglCullFace( GL_FRONT );
			}
		}
	}

	backEnd.glState.faceCulling = cullType;
}

/*
====================
idRenderBackendGL::SetTexEnv
====================
*/
void idRenderBackendGL::SetTexEnv( int env ) {
	tmu_t	*tmu;

	tmu = &backEnd.glState.tmu[backEnd.glState.currenttmu];
	if ( env == tmu->texEnv ) {
		return;
	}

	tmu->texEnv = env;

	switch ( env ) {
	case GL_COMBINE_EXT:
	case GL_MODULATE:
	case GL_REPLACE:
	case GL_DECAL:
	case GL_ADD:
		qglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env );
		break;
	default:
		common->Error( "GL_TexEnv: invalid env '%d' passed\n", env );
		break;
	}
}

/*
=================
idRenderBackendGL::ClearStateDelta

Clears the state delta bits, so the next SetState
will set every item
=================
*/
void idRenderBackendGL::ClearStateDelta( void ) {
	backEnd.glState.forceGlState = true;
}

/*
====================
idRenderBackendGL::SetState

This routine is responsible for setting the most commonly changed state
====================
*/
void idRenderBackendGL::SetState( int stateBits ) {
	int	diff;

	if ( !r_useStateCaching.GetBool() || backEnd.glState.forceGlState ) {
		// make sure everything is set all the time, so we
		// can see if our delta checking is screwing up
		diff = -1;
		backEnd.glState.forceGlState = false;
	} else {
		diff = stateBits ^ backEnd.glState.glStateBits;
		if ( !diff ) {
			return;
		}
	}

	//
	// check depthFunc bits
	//
	if ( diff & ( GLS_DEPTHFUNC_EQUAL | GLS_DEPTHFUNC_LESS | GLS_DEPTHFUNC_ALWAYS ) ) {
		if ( stateBits & GLS_DEPTHFUNC_EQUAL ) {
			qglDepthFunc( GL_EQUAL );
		} else if ( stateBits & GLS_DEPTHFUNC_ALWAYS ) {
			qglDepthFunc( GL_ALWAYS );
		} else {
			qglDepthFunc( GL_LEQUAL );
		}
	}


	//
	// check blend bits
	//
	if ( diff & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) {
		GLenum srcFactor, dstFactor;

		switch ( stateBits & GLS_SRCBLEND_BITS ) {
		case GLS_SRCBLEND_ZERO:
			srcFactor = GL_ZERO;
			break;
		case GLS_SRCBLEND_ONE:
			srcFactor = GL_ONE;
			break;
		case GLS_SRCBLEND_DST_COLOR:
			srcFactor = GL_DST_COLOR;
			break;
		case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
			srcFactor = GL_ONE_MINUS_DST_COLOR;
			break;
		case GLS_SRCBLEND_SRC_ALPHA:
			srcFactor = GL_SRC_ALPHA;
			break;
		case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
			srcFactor = GL_ONE_MINUS_SRC_ALPHA;
			break;
		case GLS_SRCBLEND_DST_ALPHA:
			srcFactor = GL_DST_ALPHA;
			break;
		case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
			srcFactor = GL_ONE_MINUS_DST_ALPHA;
			break;
		case GLS_SRCBLEND_ALPHA_SATURATE:
			srcFactor = GL_SRC_ALPHA_SATURATE;
			break;
		default:
			srcFactor = GL_ONE;		// to get warning to shut up
			common->Error( "GL_State: invalid src blend state bits\n" );
			break;
		}

		switch ( stateBits & GLS_DSTBLEND_BITS ) {
		case GLS_DSTBLEND_ZERO:
			dstFactor = GL_ZERO;
			break;
		case GLS_DSTBLEND_ONE:
			dstFactor = GL_ONE;
			break;
		case GLS_DSTBLEND_SRC_COLOR:
			dstFactor = GL_SRC_COLOR;
			break;
		case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
			dstFactor = GL_ONE_MINUS_SRC_COLOR;
			break;
		case GLS_DSTBLEND_SRC_ALPHA:
			dstFactor = GL_SRC_ALPHA;
			break;
		case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
			dstFactor = GL_ONE_MINUS_SRC_ALPHA;
			break;
		case GLS_DSTBLEND_DST_ALPHA:
			dstFactor = GL_DST_ALPHA;
			break;
		case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
			dstFactor = GL_ONE_MINUS_DST_ALPHA;
			break;
		default:
			dstFactor = GL_ONE;		// to get warning to shut up
			common->Error( "GL_State: invalid dst blend state bits\n" );
			break;
		}

		qglBlendFunc( srcFactor, dstFactor );
	}

	//
	// check depthmask
	//
	if ( diff & GLS_DEPTHMASK ) {
		if ( stateBits & GLS_DEPTHMASK ) {
			qglDepthMask( GL_FALSE );
		} else {
			qglDepthMask( GL_TRUE );
		}
	}

	//
	// check colormask
	//
	if ( diff & (GLS_REDMASK|GLS_GREENMASK|GLS_BLUEMASK|GLS_ALPHAMASK) ) {
		GLboolean		r, g, b, a;
		r = ( stateBits & GLS_REDMASK ) ? 0 : 1;
		g = ( stateBits & GLS_GREENMASK ) ? 0 : 1;
		b = ( stateBits & GLS_BLUEMASK ) ? 0 : 1;
		a = ( stateBits & GLS_ALPHAMASK ) ? 0 : 1;
		qglColorMask( r, g, b, a );
	}

	//
	// fill/line mode
	//
	if ( diff & GLS_POLYMODE_LINE ) {
		if ( stateBits & GLS_POLYMODE_LINE ) {
			qglPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		} else {
			qglPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
		}
	}

	//
	// alpha test
	//
	if ( diff & GLS_ATEST_BITS ) {
		switch ( stateBits & GLS_ATEST_BITS ) {
		case 0:
			qglDisable( GL_ALPHA_TEST );
			break;
		case GLS_ATEST_EQ_255:
			qglEnable( GL_ALPHA_TEST );
			qglAlphaFunc( GL_EQUAL, 1 );
			break;
		case GLS_ATEST_LT_128:
			qglEnable( GL_ALPHA_TEST );
			qglAlphaFunc( GL_LESS, 0.5 );
			break;
		case GLS_ATEST_GE_128:
			qglEnable( GL_ALPHA_TEST );
			qglAlphaFunc( GL_GEQUAL, 0.5 );
			break;
		default:
			assert( 0 );
			break;
		}
	}

	backEnd.glState.glStateBits = stateBits;
}

/*
=============
idRenderBackendGL::SetDrawBuffer
=============
*/
void idRenderBackendGL::SetDrawBuffer( int buffer ) {
	qglDrawBuffer( buffer );

	// clear screen for debugging
	// automatically enable this with several other debug tools
	// that might leave unrendered portions of the screen
	if ( r_clear.GetFloat() || idStr::Length( r_clear.GetString() ) != 1 || r_lockSurfaces.GetBool() || r_singleArea.GetBool() || r_showOverDraw.GetBool() ) {
		float c[3];
		if ( sscanf( r_clear.GetString(), "%f %f %f", &c[0], &c[1], &c[2] ) == 3 ) {
			qglClearColor( c[0], c[1], c[2], 1 );
		} else if ( r_clear.GetInteger() == 2 ) {
			qglClearColor( 0.0f, 0.0f,  0.0f, 1.0f );
		} else if ( r_showOverDraw.GetBool() ) {
			qglClearColor( 1.0f, 1.0f, 1.0f, 1.0f );
		} else {
			qglClearColor( 0.4f, 0.0f, 0.25f, 1.0f );
		}
		qglClear( GL_COLOR_BUFFER_BIT );
	}
}

/*
=============
idRenderBackendGL::SwapBuffers
=============
*/
void idRenderBackendGL::SwapBuffers( void ) {
	int fillAlpha = r_fillWindowAlphaChan.GetInteger();
	if ( fillAlpha == 1 || (fillAlpha == -1 && glConfig.shouldFillWindowAlpha) )
	{
		// make sure the whole alpha chan of the (default) framebuffer is opaque.
		// at least Wayland needs this, see also the big comment in GLimp_Init()

		bool blendEnabled = qglIsEnabled( GL_BLEND );
		if ( !blendEnabled )
			qglEnable( GL_BLEND );

		// TODO: GL_DEPTH_TEST ? (should be disabled, if it needs changing at all)

		bool scissorEnabled = qglIsEnabled( GL_SCISSOR_TEST );
		if( scissorEnabled )
			qglDisable( GL_SCISSOR_TEST );

		bool tex2Denabled = qglIsEnabled( GL_TEXTURE_2D );
		if( tex2Denabled )
			qglDisable( GL_TEXTURE_2D );

		qglDisable( GL_VERTEX_PROGRAM_ARB );
		qglDisable( GL_FRAGMENT_PROGRAM_ARB );

		qglBlendEquation( GL_FUNC_ADD );

		qglBlendFunc( GL_ONE, GL_ONE );

		// setup transform matrices so we can easily/reliably draw a fullscreen quad
		qglMatrixMode( GL_MODELVIEW );
		qglPushMatrix();
		qglLoadIdentity();

		qglMatrixMode( GL_PROJECTION );
		qglPushMatrix();
		qglLoadIdentity();
		qglOrtho( 0, 1, 0, 1, -1, 1 );

		// draw screen-sized quad with color (0.0, 0.0, 0.0, 1.0)
		const float x=0, y=0, w=1, h=1;
		qglColor4f( 0.0f, 0.0f, 0.0f, 1.0f );
		// debug values:
		//const float x = 0.1, y = 0.1, w = 0.8, h = 0.8;
		//qglColor4f( 0.0f, 0.0f, 0.5f, 1.0f );

		qglBegin( GL_QUADS );
			qglVertex2f( x,   y   ); // ( 0,0 );
			qglVertex2f( x,   y+h ); // ( 0,1 );
			qglVertex2f( x+w, y+h ); // ( 1,1 );
			qglVertex2f( x+w, y   ); // ( 1,0 );
		qglEnd();

		// restore previous transform matrix states
		qglPopMatrix(); // for projection
		qglMatrixMode( GL_MODELVIEW );
		qglPopMatrix(); // for modelview

		// restore default or previous states
		qglBlendEquation( GL_FUNC_ADD );
		if ( !blendEnabled )
			qglDisable( GL_BLEND );
		if( tex2Denabled )
			qglEnable( GL_TEXTURE_2D );
		if( scissorEnabled )
			qglEnable( GL_SCISSOR_TEST );
	}

	// force a gl sync if requested
	if ( r_finish.GetBool() ) {
		qglFinish();
	}

	// don't flip if drawing to front buffer
	if ( !r_frontBuffer.GetBool() ) {
		GLimp_SwapBuffers();
	}
}

/*
================
idRenderBackendGL::DrawIndexed

The caller decides how much of the surface to draw: the shadow path passes a
count smaller than the surface holds when the caps can be skipped.
================
*/
void idRenderBackendGL::DrawIndexed( const srfTriangles_t *tri, int numIndexes ) {
	if ( tri->indexCache && r_useIndexBuffers.GetBool() ) {
		qglDrawElements( GL_TRIANGLES,
						r_singleTriangle.GetBool() ? 3 : numIndexes,
						GL_INDEX_TYPE,
						(int *)vertexCache.Position( tri->indexCache ) );
		backEnd.pc.c_vboIndexes += numIndexes;
	} else {
		if ( r_useIndexBuffers.GetBool() ) {
			vertexCache.UnbindIndex();
		}
		qglDrawElements( GL_TRIANGLES,
						r_singleTriangle.GetBool() ? 3 : numIndexes,
						GL_INDEX_TYPE,
						tri->indexes );
	}
}

/*
=================
idRenderBackendGL::CheckErrors
=================
*/
void idRenderBackendGL::CheckErrors( void ) {
	int		err;
	char	s[64];
	int		i;

	// check for up to 10 errors pending
	for ( i = 0 ; i < 10 ; i++ ) {
		err = qglGetError();
		if ( err == GL_NO_ERROR ) {
			return;
		}
		switch( err ) {
			case GL_INVALID_ENUM:
				strcpy( s, "GL_INVALID_ENUM" );
				break;
			case GL_INVALID_VALUE:
				strcpy( s, "GL_INVALID_VALUE" );
				break;
			case GL_INVALID_OPERATION:
				strcpy( s, "GL_INVALID_OPERATION" );
				break;
			case GL_STACK_OVERFLOW:
				strcpy( s, "GL_STACK_OVERFLOW" );
				break;
			case GL_STACK_UNDERFLOW:
				strcpy( s, "GL_STACK_UNDERFLOW" );
				break;
			case GL_OUT_OF_MEMORY:
				strcpy( s, "GL_OUT_OF_MEMORY" );
				break;
			default:
				idStr::snPrintf( s, sizeof(s), "%i", err);
				break;
		}

		if ( !r_ignoreGLErrors.GetBool() ) {
			common->Printf( "GL_CheckErrors: %s\n", s );
		}
	}
}

/*
===============================================================================

	Images.

	Moved from Image_load.cpp and Image_init.cpp unchanged. What stayed behind
	is every decision - which pixels, at what size, in what format, and when to
	throw them away.

	Five image paths were deliberately not moved, because none of them can run
	on a backend that is not OpenGL and each is already switched off by
	something the port controls rather than by a new flag:

	  Generate3DImage            never called at all - idImage::TT_3D does not
	                             happen (plan.md, section 5)
	  GenerateCubeImage          glConfig.cubeMapAvailable
	  UploadCompressedNormalMap  glConfig.sharedTexturePaletteAvailable
	  idImageManager::SetNormalPalette   the same
	  WritePrecompressedImage    image_useOfflineCompression, which is 0 and is
	                             a content tool rather than a rendering path

	They stay in the image layer with their qgl calls in them. A backend that
	wants cube maps takes GenerateCubeImage then, with something to map it
	onto; moving it now would only produce an entry point nothing can call.

===============================================================================
*/

/*
====================
idRenderBackendGL::AllocImage / FreeImage
====================
*/
void idRenderBackendGL::AllocImage( idImage *image ) {
	qglGenTextures( 1, &image->texnum );
}

void idRenderBackendGL::FreeImage( idImage *image ) {
	qglDeleteTextures( 1, &image->texnum );
}

/*
====================
idRenderBackendGL::BindImage

Automatically enables 2D mapping, cube mapping, or 3D texturing if needed.
====================
*/
void idRenderBackendGL::BindImage( idImage *image ) {
	tmu_t	*tmu = &backEnd.glState.tmu[backEnd.glState.currenttmu];

	// enable or disable apropriate texture modes
	if ( tmu->textureType != image->type && ( backEnd.glState.currenttmu <	glConfig.maxTextureUnits ) ) {
		if ( tmu->textureType == TT_CUBIC ) {
			qglDisable( GL_TEXTURE_CUBE_MAP_EXT );
		} else if ( tmu->textureType == TT_3D ) {
			qglDisable( GL_TEXTURE_3D );
		} else if ( tmu->textureType == TT_2D ) {
			qglDisable( GL_TEXTURE_2D );
		}

		if ( image->type == TT_CUBIC ) {
			qglEnable( GL_TEXTURE_CUBE_MAP_EXT );
		} else if ( image->type == TT_3D ) {
			qglEnable( GL_TEXTURE_3D );
		} else if ( image->type == TT_2D ) {
			qglEnable( GL_TEXTURE_2D );
		}
		tmu->textureType = image->type;
	}

	// bind the texture
	if ( image->type == TT_2D ) {
		if ( tmu->current2DMap != (int)image->texnum ) {
			tmu->current2DMap = image->texnum;
			qglBindTexture( GL_TEXTURE_2D, image->texnum );
		}
	} else if ( image->type == TT_CUBIC ) {
		if ( tmu->currentCubeMap != (int)image->texnum ) {
			tmu->currentCubeMap = image->texnum;
			qglBindTexture( GL_TEXTURE_CUBE_MAP_EXT, image->texnum );
		}
	} else if ( image->type == TT_3D ) {
		if ( tmu->current3DMap != (int)image->texnum ) {
			tmu->current3DMap = image->texnum;
			qglBindTexture( GL_TEXTURE_3D, image->texnum );
		}
	}

	if ( com_purgeAll.GetBool() ) {
		GLclampf priority = 1.0f;
		qglPrioritizeTextures( 1, &image->texnum, &priority );
	}
}

/*
====================
idRenderBackendGL::BindImageFragment

Fragment programs explicitly say which type of map they want, so we don't need
to do any enable / disable changes.
====================
*/
void idRenderBackendGL::BindImageFragment( idImage *image ) {
	if ( image->type == TT_2D ) {
		qglBindTexture( GL_TEXTURE_2D, image->texnum );
	} else if ( image->type == TT_RECT ) {
		qglBindTexture( GL_TEXTURE_RECTANGLE_NV, image->texnum );
	} else if ( image->type == TT_CUBIC ) {
		qglBindTexture( GL_TEXTURE_CUBE_MAP_EXT, image->texnum );
	} else if ( image->type == TT_3D ) {
		qglBindTexture( GL_TEXTURE_3D, image->texnum );
	}
}

/*
====================
idRenderBackendGL::BindNoImage
====================
*/
void idRenderBackendGL::BindNoImage( void ) {
	tmu_t	*tmu = &backEnd.glState.tmu[backEnd.glState.currenttmu];

	if ( tmu->textureType == TT_CUBIC ) {
		qglDisable( GL_TEXTURE_CUBE_MAP_EXT );
	} else if ( tmu->textureType == TT_3D ) {
		qglDisable( GL_TEXTURE_3D );
	} else if ( tmu->textureType == TT_2D ) {
		qglDisable( GL_TEXTURE_2D );
	}
	tmu->textureType = TT_DISABLED;
}

/*
====================
idRenderBackendGL::UploadImageLevel
====================
*/
void idRenderBackendGL::UploadImageLevel( idImage *image, int face, int level, int internalFormat,
										  int width, int height, int externalFormat,
										  const byte *pixels ) {
	GLenum	target = ( image->type == TT_CUBIC )
					  ? ( GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT + face )
					  : GL_TEXTURE_2D;

	qglTexImage2D( target, level, internalFormat, width, height, 0,
				   externalFormat, GL_UNSIGNED_BYTE, pixels );
}

/*
====================
idRenderBackendGL::UploadCompressedImageLevel
====================
*/
void idRenderBackendGL::UploadCompressedImageLevel( idImage *image, int level, int internalFormat,
													int width, int height, int numBytes,
													const byte *data ) {
	qglCompressedTexImage2DARB( GL_TEXTURE_2D, level, internalFormat, width, height, 0, numBytes, data );
}

/*
====================
idRenderBackendGL::SetImageMaxLevel
====================
*/
void idRenderBackendGL::SetImageMaxLevel( idImage *image, int maxLevel ) {
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLevel );
}

/*
====================
idRenderBackendGL::UploadScratchImage

If rows = cols * 6, assume it is a cube map animation.
====================
*/
void idRenderBackendGL::UploadScratchImage( idImage *image, const byte *data, int cols, int rows ) {
	int			i;

	// if rows = cols * 6, assume it is a cube map animation
	if ( rows == cols * 6 ) {
		if ( image->type != TT_CUBIC ) {
			image->type = TT_CUBIC;
			image->uploadWidth = -1;	// for a non-sub upload
		}

		image->Bind();

		rows /= 6;
		// if the scratchImage isn't in the format we want, specify it as a new texture
		if ( cols != image->uploadWidth || rows != image->uploadHeight ) {
			image->uploadWidth = cols;
			image->uploadHeight = rows;

			// upload the base level
			for ( i = 0 ; i < 6 ; i++ ) {
				qglTexImage2D( GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT+i, 0, GL_RGB8, cols, rows, 0,
					GL_RGBA, GL_UNSIGNED_BYTE, data + cols*rows*4*i );
			}
		} else {
			// otherwise, just subimage upload it so that drivers can tell we are going to be changing
			// it and don't try and do a texture compression
			for ( i = 0 ; i < 6 ; i++ ) {
				qglTexSubImage2D( GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT+i, 0, 0, 0, cols, rows,
					GL_RGBA, GL_UNSIGNED_BYTE, data + cols*rows*4*i );
			}
		}
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		// no other clamp mode makes sense
		qglTexParameteri(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		qglTexParameteri(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	} else {
		// otherwise, it is a 2D image
		if ( image->type != TT_2D ) {
			image->type = TT_2D;
			image->uploadWidth = -1;	// for a non-sub upload
		}

		image->Bind();

		// if the scratchImage isn't in the format we want, specify it as a new texture
		if ( cols != image->uploadWidth || rows != image->uploadHeight ) {
			image->uploadWidth = cols;
			image->uploadHeight = rows;
			qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, cols, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, data );
		} else {
			// otherwise, just subimage upload it so that drivers can tell we are going to be changing
			// it and don't try and do a texture compression
			qglTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, cols, rows, GL_RGBA, GL_UNSIGNED_BYTE, data );
		}
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		// these probably should be clamp, but we have a lot of issues with editor
		// geometry coming out with texcoords slightly off one side, resulting in
		// a smear across the entire polygon
#if 1
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
#else
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
#endif
	}
}

/*
====================
idRenderBackendGL::SetImageFilterAndRepeat
====================
*/
void idRenderBackendGL::SetImageFilterAndRepeat( const idImage *image ) {
	// set the minimize / maximize filtering
	switch( image->filter ) {
	case TF_DEFAULT:
		qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, globalImages->textureMinFilter );
		qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, globalImages->textureMaxFilter );
		break;
	case TF_LINEAR:
		qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		break;
	case TF_NEAREST:
		qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		break;
	default:
		common->FatalError( "R_CreateImage: bad texture filter" );
	}

	if ( glConfig.anisotropicAvailable ) {
		// only do aniso filtering on mip mapped images
		if ( image->filter == TF_DEFAULT ) {
			qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, globalImages->textureAnisotropy );
		} else {
			qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1 );
		}
	}
	if ( glConfig.textureLODBiasAvailable ) {
		qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS_EXT, globalImages->textureLODBias );
	}

	// set the wrap/clamp modes
	switch( image->repeat ) {
	case TR_REPEAT:
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
		break;
	case TR_CLAMP_TO_BORDER:
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER );
		break;
	case TR_CLAMP_TO_ZERO:
	case TR_CLAMP_TO_ZERO_ALPHA:
	case TR_CLAMP:
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		break;
	default:
		common->FatalError( "R_CreateImage: bad texture repeat" );
	}
}

/*
====================
idRenderBackendGL::SetCubeImageFilterAndRepeat

The cube map's own, and not the same as the 2D one: the wrap is forced to clamp
because no other mode makes sense across a seam, and neither the anisotropy nor
the LOD bias is applied.
====================
*/
void idRenderBackendGL::SetCubeImageFilterAndRepeat( const idImage *image ) {
	// no other clamp mode makes sense
	qglTexParameteri(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	qglTexParameteri(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// set the minimize / maximize filtering
	switch( image->filter ) {
	case TF_DEFAULT:
		qglTexParameterf(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_MIN_FILTER, globalImages->textureMinFilter );
		qglTexParameterf(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_MAG_FILTER, globalImages->textureMaxFilter );
		break;
	case TF_LINEAR:
		qglTexParameterf(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameterf(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		break;
	case TF_NEAREST:
		qglTexParameterf(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		qglTexParameterf(GL_TEXTURE_CUBE_MAP_EXT, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		break;
	default:
		common->FatalError( "R_CreateImage: bad texture filter" );
	}
}

/*
====================
idRenderBackendGL::RefreshImageFilter
====================
*/
void idRenderBackendGL::RefreshImageFilter( const idImage *image ) {
	unsigned int	texEnum = GL_TEXTURE_2D;

	switch( image->type ) {
	case TT_2D:
		texEnum = GL_TEXTURE_2D;
		break;
	case TT_3D:
		texEnum = GL_TEXTURE_3D;
		break;
	case TT_CUBIC:
		texEnum = GL_TEXTURE_CUBE_MAP_EXT;
		break;
	default:
		break;
	}

	if ( image->filter == TF_DEFAULT ) {
		qglTexParameterf(texEnum, GL_TEXTURE_MIN_FILTER, globalImages->textureMinFilter );
		qglTexParameterf(texEnum, GL_TEXTURE_MAG_FILTER, globalImages->textureMaxFilter );
	}
	if ( glConfig.anisotropicAvailable ) {
		qglTexParameterf(texEnum, GL_TEXTURE_MAX_ANISOTROPY_EXT, globalImages->textureAnisotropy );
	}
	if ( glConfig.textureLODBiasAvailable ) {
		qglTexParameterf(texEnum, GL_TEXTURE_LOD_BIAS_EXT, globalImages->textureLODBias );
	}
}

/*
====================
idRenderBackendGL::SetImageBorderColor
====================
*/
void idRenderBackendGL::SetImageBorderColor( const idImage *image, const float rgba[4] ) {
	qglTexParameterfv( GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, rgba );
}

/*
====================
idRenderBackendGL::CopyFramebufferToImage
====================
*/
void idRenderBackendGL::CopyFramebufferToImage( idImage *image, int x, int y,
												int imageWidth, int imageHeight,
												bool useOversizedBuffer ) {
	image->Bind();

	if ( cvarSystem->GetCVarBool( "g_lowresFullscreenFX" ) ) {
		imageWidth = 512;
		imageHeight = 512;
	}

	// if the size isn't a power of 2, the image must be increased in size
	int	potWidth, potHeight;

	potWidth = MakePowerOfTwo( imageWidth );
	potHeight = MakePowerOfTwo( imageHeight );

	image->GetDownsize( imageWidth, imageHeight );
	image->GetDownsize( potWidth, potHeight );

	qglReadBuffer( GL_BACK );

	// only resize if the current dimensions can't hold it at all,
	// otherwise subview renderings could thrash this
	if ( ( useOversizedBuffer && ( image->uploadWidth < potWidth || image->uploadHeight < potHeight ) )
		|| ( !useOversizedBuffer && ( image->uploadWidth != potWidth || image->uploadHeight != potHeight ) ) ) {
		image->uploadWidth = potWidth;
		image->uploadHeight = potHeight;
		if ( potWidth == imageWidth && potHeight == imageHeight ) {
			qglCopyTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, x, y, imageWidth, imageHeight, 0 );
		} else {
			byte	*junk;
			// we need to create a dummy image with power of two dimensions,
			// then do a qglCopyTexSubImage2D of the data we want
			// this might be a 16+ meg allocation, which could fail on _alloca
			junk = (byte *)Mem_Alloc( potWidth * potHeight * 4 );
			memset( junk, 0, potWidth * potHeight * 4 );		//!@#
#if 0 // Disabling because it's unnecessary and introduces a green strip on edge of _currentRender
			for ( int i = 0 ; i < potWidth * potHeight * 4 ; i+=4 ) {
				junk[i+1] = 255;
			}
#endif
			qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGB, potWidth, potHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, junk );
			Mem_Free( junk );

			qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, imageWidth, imageHeight );
		}
	} else {
		// otherwise, just subimage upload it so that drivers can tell we are going to be changing
		// it and don't try and do a texture compression or some other silliness
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, imageWidth, imageHeight );
	}

	// if the image isn't a full power of two, duplicate an extra row and/or column to fix bilerps
	if ( imageWidth != potWidth ) {
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, imageWidth, 0, x+imageWidth-1, y, 1, imageHeight );
	}
	if ( imageHeight != potHeight ) {
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, imageHeight, x, y+imageHeight-1, imageWidth, 1 );
	}

	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	backEnd.c_copyFrameBuffer++;
}

/*
====================
idRenderBackendGL::CopyDepthbufferToImage

This should just be part of CopyFramebufferToImage once we have a proper image
type field.
====================
*/
void idRenderBackendGL::CopyDepthbufferToImage( idImage *image, int x, int y,
												int imageWidth, int imageHeight,
												bool useOversizedBuffer ) {
	image->Bind();

	// if the size isn't a power of 2, the image must be increased in size
	int	potWidth, potHeight;

	potWidth = MakePowerOfTwo( imageWidth );
	potHeight = MakePowerOfTwo( imageHeight );
	image->GetDownsize( imageWidth, imageHeight );
	image->GetDownsize( potWidth, potHeight );
	// Ensure we are reading from the back buffer:
	qglReadBuffer( GL_BACK );
	// only resize if the current dimensions can't hold it at all,
	// otherwise subview renderings could thrash this
	if ( ( useOversizedBuffer && ( image->uploadWidth < potWidth || image->uploadHeight < potHeight ) ) || ( !useOversizedBuffer && ( image->uploadWidth != potWidth || image->uploadHeight != potHeight ) ) )
	{
		image->uploadWidth = potWidth;
		image->uploadHeight = potHeight;
		// This bit runs once only at map start, because it tests whether the image is too small to hold the screen.
		// It resizes the texture to a power of two that can hold the screen,
		// and then subsequent captures to the texture put the depth component into the RGB channels
		qglTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24_ARB, potWidth, potHeight, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, NULL );
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, imageWidth, imageHeight );

	} else {
		// otherwise, just subimage upload it so that drivers can tell we are going to be changing
		// it and don't try and do a texture compression or some other silliness
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, imageWidth, imageHeight );
	}

	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );

	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
}
