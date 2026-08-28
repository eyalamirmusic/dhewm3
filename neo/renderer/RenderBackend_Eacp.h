/*
===========================================================================

dhewm 3 on eacp - the two things the host hands the render backend.

The backend lives in renderer/ and the window lives in sys/eacp/, and this is
the whole of what passes between them. Neither includes the other's headers:
sys/eacp/ calls these two functions, and renderer/RenderBackend_Eacp.cpp is
what they reach.

===========================================================================
*/

#ifndef __RENDERBACKEND_EACP_H__
#define __RENDERBACKEND_EACP_H__

namespace eacp {
namespace GPU {
class Frame;
class GPUView;
}
}

// The view the engine draws into, from the view's own constructor. GLimp_Init
// sizes glConfig from it, and the backend's passes are its drawable's.
void R_EacpSetView( eacp::GPU::GPUView *view );
eacp::GPU::GPUView *R_EacpGetView( void );

// The frame currently being rendered, for exactly as long as common->Frame()
// runs inside GPUView::render. Null outside it, which is what makes a draw
// issued from anywhere else - a level load, a console command - a no-op that
// says so rather than a crash.
void R_EacpSetFrame( eacp::GPU::Frame *frame );

#endif /* !__RENDERBACKEND_EACP_H__ */
