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
#include "sys/platform.h"
#include "sys/sys_imgui.h"

#include "renderer/tr_local.h"

frameData_t		*frameData;
backEndState_t	backEnd;

/*
======================
RB_SetDefaultGLState

This should initialize all GL state that any part of the entire program
may touch, including the editor.
======================
*/
void RB_SetDefaultGLState( void ) {
	renderBackend->SetDefaultState();
}

/*
====================
GL_SelectTexture
====================
*/
void GL_SelectTexture( int unit ) {
	renderBackend->SelectTexture( unit );
}

/*
====================
GL_Cull
====================
*/
void GL_Cull( int cullType ) {
	renderBackend->SetCull( cullType );
}

/*
====================
GL_TexEnv
====================
*/
void GL_TexEnv( int env ) {
	renderBackend->SetTexEnv( env );
}

/*
=================
GL_ClearStateDelta
=================
*/
void GL_ClearStateDelta( void ) {
	renderBackend->ClearStateDelta();
}

/*
====================
GL_State
====================
*/
void GL_State( int stateBits ) {
	renderBackend->SetState( stateBits );
}

//=============================================================================

/*
============================================================================

RENDER BACK END THREAD FUNCTIONS

============================================================================
*/

/*
=============
RB_SetGL2D

This is not used by the normal game paths, just by some tools
=============
*/
void RB_SetGL2D( void ) {
	// set 2D virtual screen size
	qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	if ( r_useScissor.GetBool() ) {
		qglScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	}
	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity();
	qglOrtho( 0, 640, 480, 0, 0, 1 );		// always assume 640x480 virtual coordinates
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity();

	GL_State( GLS_DEPTHFUNC_ALWAYS |
			  GLS_SRCBLEND_SRC_ALPHA |
			  GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );

	GL_Cull( CT_TWO_SIDED );

	qglDisable( GL_DEPTH_TEST );
	qglDisable( GL_STENCIL_TEST );
}



/*
=============
RB_SetBuffer

=============
*/
static void	RB_SetBuffer( const void *data ) {
	const setBufferCommand_t	*cmd;

	// see which draw buffer we want to render the frame to

	cmd = (const setBufferCommand_t *)data;

	backEnd.frameCount = cmd->frameCount;

	renderBackend->SetDrawBuffer( cmd->buffer );
}

/*
=============
RB_SwapBuffers

=============
*/
const void	RB_SwapBuffers( const void *data ) {
	D3::ImGuiHooks::EndFrame();

	renderBackend->SwapBuffers();
}

/*
=============
RB_CopyRender

Copy part of the current framebuffer to an image
=============
*/
const void	RB_CopyRender( const void *data ) {
	const copyRenderCommand_t	*cmd;

	cmd = (const copyRenderCommand_t *)data;

	if ( r_skipCopyTexture.GetBool() ) {
		return;
	}

	if (cmd->image) {
		cmd->image->CopyFramebuffer( cmd->x, cmd->y, cmd->imageWidth, cmd->imageHeight, false );
	}
}

/*
====================
RB_ExecuteBackEndCommands

This function will be called syncronously if running without
smp extensions, or asyncronously by another thread.
====================
*/
int		backEndStartTime, backEndFinishTime;
void RB_ExecuteBackEndCommands( const emptyCommand_t *cmds ) {
	// r_debugRenderToTexture
	int	c_draw3d = 0, c_draw2d = 0, c_setBuffers = 0, c_swapBuffers = 0, c_copyRenders = 0;

	if ( cmds->commandId == RC_NOP && !cmds->next ) {
		return;
	}

	backEndStartTime = Sys_Milliseconds();

	// needed for editor rendering
	RB_SetDefaultGLState();

	// upload any image loads that have completed
	globalImages->CompleteBackgroundImageLoads();

	for ( ; cmds ; cmds = (const emptyCommand_t *)cmds->next ) {
		switch ( cmds->commandId ) {
		case RC_NOP:
			break;
		case RC_DRAW_VIEW:
			RB_DrawView( cmds );
			if ( ((const drawSurfsCommand_t *)cmds)->viewDef->viewEntitys ) {
				c_draw3d++;
			}
			else {
				c_draw2d++;
			}
			break;
		case RC_SET_BUFFER:
			RB_SetBuffer( cmds );
			c_setBuffers++;
			break;
		case RC_SWAP_BUFFERS:
			RB_SwapBuffers( cmds );
			c_swapBuffers++;
			break;
		case RC_COPY_RENDER:
			RB_CopyRender( cmds );
			c_copyRenders++;
			break;
		default:
			common->Error( "RB_ExecuteBackEndCommands: bad commandId" );
			break;
		}
	}

	// go back to the default texture so the editor doesn't mess up a bound image
	renderBackend->ReleaseTextures();

	// stop rendering on this thread
	backEndFinishTime = Sys_Milliseconds();
	backEnd.pc.msec = backEndFinishTime - backEndStartTime;

	if ( r_debugRenderToTexture.GetInteger() == 1 ) {
		common->Printf( "3d: %i, 2d: %i, SetBuf: %i, SwpBuf: %i, CpyRenders: %i, CpyFrameBuf: %i\n", c_draw3d, c_draw2d, c_setBuffers, c_swapBuffers, c_copyRenders, backEnd.c_copyFrameBuffer );
		backEnd.c_copyFrameBuffer = 0;
	}
}
