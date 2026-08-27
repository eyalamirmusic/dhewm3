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
};

static idRenderBackendGL	renderBackendGL;
idRenderBackend *			renderBackend = &renderBackendGL;

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
