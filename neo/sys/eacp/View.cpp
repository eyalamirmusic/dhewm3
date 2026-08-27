#include "View.h"

namespace dhewm3
{
View::View()
{
	// Doom 3's lighting is stencil shadow volumes, so the drawable needs a
	// stencil plane before any of it can be drawn. Asked for here rather than
	// with the renderer because it sizes the view's own attachments: it is a
	// property of the surface being drawn into, not of a pipeline drawing to
	// it. It implies depth - both APIs carry the two planes in one attachment
	// of one combined format, so a view asking for stencil allocates the depth
	// plane whether or not anything tests it.
	setStencil(true);

	// The engine's frame is driven off the display link rather than on demand:
	// a game redraws every refresh whether or not anything the platform layer
	// can see has changed.
	setContinuous(true);
}

void View::render(GPU::Frame& frame)
{
	// Nothing to draw yet, but the pass still has to be opened and closed: a
	// frame that begins no pass presents whatever the drawable happened to
	// contain, which on a freshly allocated one is undefined.
	frame.beginPass({Graphics::Color {0.05f, 0.05f, 0.06f}});
}
} // namespace dhewm3
