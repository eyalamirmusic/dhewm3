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

// eacp's headers, and this one, before any of Doom 3's - see the note at the
// top of RenderProgs_Eacp.h.
#include "renderer/RenderProgs_Eacp.h"

#include "sys/platform.h"

#include "renderer/tr_local.h"
#include "renderer/RenderBackend_Eacp.h"

using namespace eacp;

idEacpRenderProgs	eacpRenderProgs;

/*
================================================================================

	The vertex.

================================================================================
*/

// The layout is idDrawVert's, byte for byte, so a draw uploads the engine's own
// vertices rather than a repacked copy of them. If either side moves, this is
// where it is caught.
static_assert( sizeof( eacpDrawVert_t ) == sizeof( idDrawVert ),
			   "eacpDrawVert_t must be idDrawVert's layout exactly" );
static_assert( offsetof( eacpDrawVert_t, st ) == offsetof( idDrawVert, st ),
			   "eacpDrawVert_t::st must sit where idDrawVert::st does" );
static_assert( offsetof( eacpDrawVert_t, color ) == offsetof( idDrawVert, color ),
			   "eacpDrawVert_t::color must sit where idDrawVert::color does" );

/*
================================================================================

	idEacpStageProgram

================================================================================
*/

idEacpStageProgram::idEacpStageProgram( GPU::TextureSampling sampling, bool alphaTest ) {
	// Before compile(), not after: the build walk reads this to place the
	// static sampler the generated shader points at, and eacp bakes the choice
	// into the source rather than leaving it to a bind.
	image.sampling = sampling;

	discards = alphaTest;

	compile();
}

/*
====================
idEacpStageProgram::define

The whole of what Doom 3 draws without a light, in four lines. What it replaces
is a matrix stack, a texture matrix, glColor, a colour array and - for
SVC_INVERSE_MODULATE and a non-white constant colour together - a second texture
unit bound to the white image purely so a combiner had somewhere to put the
multiply.
====================
*/
void idEacpStageProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );
	auto	texcoord = vertexInput( &eacpDrawVert_t::st );
	auto	vertexColor = vertexInput( &eacpDrawVert_t::color );

	setPosition( modelViewProjection * GPU::float4( position, 1.0f ) );

	// Doom 3's texture matrix is a 4x4 holding a 2x3: RB_GetShaderTextureMatrix
	// writes s and t as affine functions of (s, t) and leaves the other two rows
	// as the identity. So the transform is two dot products against (s, t, 1, 0)
	// rather than a matrix multiply.
	auto	homogeneous = GPU::float4( texcoord, 1.0f, 0.0f );

	auto	transformed = GPU::float2( GPU::dot( homogeneous, textureMatrixS ),
									   GPU::dot( homogeneous, textureMatrixT ) );

	auto	fragment = GPU::sample( image, varying( transformed ) )
		* varying( vertexColor * colorModulate + colorAdd );

	// glAlphaFunc( GL_GREATER, ref ) against the fragment's own alpha, which is
	// what the fixed-function pipeline compares: the default texture env is
	// modulate, so the alpha reaching the test is the texture's times the
	// constant colour's - which is this expression's w.
	//
	// setDiscardBelow's threshold is a compile-time float and Doom 3's is a
	// shader register, so the uniform goes on the other side of the comparison:
	// discarding below zero on ( alpha - ref ) is discarding below ref on
	// alpha. The one difference from GL is at exact equality, which GL_GREATER
	// discards and this keeps.
	if ( discards ) {
		setDiscardBelow( fragment.w() - alphaTestRef, 0.0f );
	}

	setFragment( fragment );
}

/*
================================================================================

	State, translated.

	The GLS_* bitfield is the one piece of Doom 3's backend that was already
	API-independent, which is why Phase 1 chose it as the seam. Here it stops
	being a set of calls and becomes a pipeline: every field below is compiled
	into the object rather than set on the way to a draw.

================================================================================
*/

/*
====================
R_EacpBlendState

glBlendFunc sets one factor pair for the colour and the alpha channels
together, so both halves of the equation get the same factors. That is what
makes eacp gap 17 (plan.md section 6, step 4b') a real gap and not a
convenience: a material asking for `filter` asks for (DST_COLOR, ZERO) in the
alpha channel too, and no shader can compute that.
====================
*/
static GPU::BlendState R_EacpBlendState( int stateBits ) {
	GPU::BlendState	state;

	GPU::BlendFactor	source = GPU::BlendFactor::One;
	GPU::BlendFactor	destination = GPU::BlendFactor::Zero;

	switch ( stateBits & GLS_SRCBLEND_BITS ) {
	case GLS_SRCBLEND_ZERO:					source = GPU::BlendFactor::Zero; break;
	case GLS_SRCBLEND_ONE:					source = GPU::BlendFactor::One; break;
	case GLS_SRCBLEND_DST_COLOR:			source = GPU::BlendFactor::DestinationColor; break;
	case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:	source = GPU::BlendFactor::OneMinusDestinationColor; break;
	case GLS_SRCBLEND_SRC_ALPHA:			source = GPU::BlendFactor::SourceAlpha; break;
	case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:	source = GPU::BlendFactor::OneMinusSourceAlpha; break;
	case GLS_SRCBLEND_DST_ALPHA:			source = GPU::BlendFactor::DestinationAlpha; break;
	case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:	source = GPU::BlendFactor::OneMinusDestinationAlpha; break;
	case GLS_SRCBLEND_ALPHA_SATURATE:		source = GPU::BlendFactor::SourceAlphaSaturated; break;
	default:
		common->Warning( "eacp: invalid src blend state bits" );
		break;
	}

	switch ( stateBits & GLS_DSTBLEND_BITS ) {
	case GLS_DSTBLEND_ZERO:					destination = GPU::BlendFactor::Zero; break;
	case GLS_DSTBLEND_ONE:					destination = GPU::BlendFactor::One; break;
	case GLS_DSTBLEND_SRC_COLOR:			destination = GPU::BlendFactor::SourceColor; break;
	case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:	destination = GPU::BlendFactor::OneMinusSourceColor; break;
	case GLS_DSTBLEND_SRC_ALPHA:			destination = GPU::BlendFactor::SourceAlpha; break;
	case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:	destination = GPU::BlendFactor::OneMinusSourceAlpha; break;
	case GLS_DSTBLEND_DST_ALPHA:			destination = GPU::BlendFactor::DestinationAlpha; break;
	case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:	destination = GPU::BlendFactor::OneMinusDestinationAlpha; break;
	default:
		common->Warning( "eacp: invalid dst blend state bits" );
		break;
	}

	// (ONE, ZERO) is the source replacing the destination, which is what a
	// disabled blend stage computes - and it is the default state, so most
	// draws take this branch. GL leaves GL_BLEND on for all of them and lets
	// the factors say it; saying it here costs the hardware nothing to know.
	state.enabled = !( source == GPU::BlendFactor::One
					   && destination == GPU::BlendFactor::Zero );

	state.sourceColor = source;
	state.destinationColor = destination;
	state.sourceAlpha = source;
	state.destinationAlpha = destination;

	return state;
}

/*
====================
R_EacpColorWriteMask

The GLS_*MASK bits are the inverse of what they name: set means *do not* write
that channel. Doom 3 uses them for the depth-only and stencil-only passes, which
is why closing eacp gap 12 was a condition of drawing anything at all.
====================
*/
static GPU::ColorWriteMask R_EacpColorWriteMask( int stateBits ) {
	GPU::ColorWriteMask	mask;

	mask.red = !( stateBits & GLS_REDMASK );
	mask.green = !( stateBits & GLS_GREENMASK );
	mask.blue = !( stateBits & GLS_BLUEMASK );
	mask.alpha = !( stateBits & GLS_ALPHAMASK );

	return mask;
}

static GPU::CompareFunction R_EacpDepthCompare( int stateBits ) {
	if ( stateBits & GLS_DEPTHFUNC_EQUAL ) {
		return GPU::CompareFunction::Equal;
	}

	if ( stateBits & GLS_DEPTHFUNC_ALWAYS ) {
		return GPU::CompareFunction::Always;
	}

	return GPU::CompareFunction::LessEqual;
}

/*
====================
R_EacpCullMode

Doom 3's CT_FRONT_SIDED culls GL_FRONT, which reads backwards until you know its
triangles are wound the other way round from GL's default. eacp names the
convention rather than the call - counter-clockwise in clip space is the front
face, on both backends - and GL's default front face is the same convention, so
the two enums line up directly and CT_FRONT_SIDED is CullMode::Front.

The mirror flip is the caller's: GL_Cull folds backEnd.viewDef->isMirror in, and
so does the cullType this is handed.
====================
*/
static GPU::CullMode R_EacpCullMode( int cullType ) {
	switch ( cullType ) {
	case CT_TWO_SIDED:
		return GPU::CullMode::None;
	case CT_BACK_SIDED:
		return GPU::CullMode::Back;
	default:
		return GPU::CullMode::Front;
	}
}

/*
====================
R_EacpSampling

Doom 3's four repeat modes are two at the hardware sampler and its three filters
are two, so its whole sampler need is exactly the four configurations eacp has.
plan.md section 4.3 is the argument, and idImage::SetImageFilterAndRepeat is
where the engine says it: the two _TO_ZERO modes write their border into the
texel data at upload and ask the sampler for plain clamp-to-edge.
====================
*/
static GPU::TextureSampling R_EacpSampling( const idImage *image ) {
	GPU::TextureSampling	sampling;

	sampling.filter = ( image->filter == TF_NEAREST ) ? GPU::TextureFilter::Nearest
													  : GPU::TextureFilter::Linear;

	sampling.addressMode = ( image->repeat == TR_REPEAT ) ? GPU::TextureAddressMode::Repeat
														  : GPU::TextureAddressMode::Clamp;

	return sampling;
}

/*
================================================================================

	idEacpRenderProgs

================================================================================
*/

idEacpRenderProgs::idEacpRenderProgs()
	: vertexStream( GPU::BufferUsage::Vertex )
	, indexStream( GPU::BufferUsage::Index ) {
}

idEacpRenderProgs::~idEacpRenderProgs() {
	// Not Shutdown(): this runs from a static destructor, after the device has
	// gone, and releasing a pipeline then is worse than leaking one. The
	// backend's Shutdown is what empties this while there is still a device to
	// release it to.
}

void idEacpRenderProgs::Shutdown( void ) {
	for ( int i = 0 ; i < GPU::samplingConfigurations ; i++ ) {
		for ( int test = 0 ; test < 2 ; test++ ) {
			programVariant_t &	variant = variants[i][test];

			for ( int j = 0 ; j < variant.pipelines.size() ; j++ ) {
				delete variant.pipelines[j].pipeline;
			}

			variant.pipelines.clear();
			variant.library.reset();
			variant.program.reset();
		}
	}
}

int idEacpRenderProgs::NumPrograms( void ) const {
	int	total = 0;

	for ( int i = 0 ; i < GPU::samplingConfigurations ; i++ ) {
		for ( int test = 0 ; test < 2 ; test++ ) {
			if ( variants[i][test].program.has_value() ) {
				total++;
			}
		}
	}

	return total;
}

int idEacpRenderProgs::NumPipelines( void ) const {
	int	total = 0;

	for ( int i = 0 ; i < GPU::samplingConfigurations ; i++ ) {
		for ( int test = 0 ; test < 2 ; test++ ) {
			total += variants[i][test].pipelines.size();
		}
	}

	return total;
}

/*
====================
idEacpRenderProgs::StageDraw

The program from the image and the pipeline from the state, both compiled the
first time they are asked for. §4.3 sizes the first at four and the demo's own
content sizes the second - which is why NumPipelines exists rather than a
guess in a comment.
====================
*/
idEacpRenderProgs::stageDraw_t idEacpRenderProgs::StageDraw( const idImage *image,
															 int stateBits, int cullType,
															 bool alphaTest ) {
	stageDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	const GPU::TextureSampling	sampling = R_EacpSampling( image );
	programVariant_t &			variant =
		variants[ GPU::samplingIndex( sampling ) ][ alphaTest ? 1 : 0 ];

	if ( !variant.program.has_value() ) {
		variant.program.emplace( sampling, alphaTest );
		variant.library.emplace( GPU::Device::shared(), variant.program->source() );

		if ( !variant.library->isValid() ) {
			common->Warning( "eacp: the material stage shader failed to compile" );
			return draw;
		}
	}

	draw.program = &*variant.program;

	// The bits a pipeline is compiled against. Two of GL_State's are not here:
	// GLS_POLYMODE_LINE, which is r_showTris' wireframe and has no eacp
	// counterpart, and GLS_ATEST_BITS, which has zero call sites in this tree -
	// the alpha test Doom 3 actually uses is shaderStage_t::hasAlphaTest, and
	// it arrives here as the alphaTest argument rather than in the bitfield.
	const int	pipelineBits = stateBits
		& ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS
			| GLS_DEPTHMASK | GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK | GLS_ALPHAMASK
			| GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHFUNC_EQUAL );

	for ( int i = 0 ; i < variant.pipelines.size() ; i++ ) {
		if ( variant.pipelines[i].stateBits == pipelineBits
			 && variant.pipelines[i].cullType == cullType ) {
			draw.pipeline = variant.pipelines[i].pipeline;
			return draw;
		}
	}

	eacp::GPU::GPUView *	view = R_EacpGetView();

	if ( !view ) {
		return draw;
	}

	GPU::RenderPipelineDescriptor	descriptor;

	descriptor.library = &*variant.library;
	descriptor.vertexLayout = variant.program->vertexLayout();
	descriptor.topology = GPU::PrimitiveTopology::Triangles;

	// The drawable's, and the view's own multisampling. Both backends reject a
	// draw whose pipeline disagrees with the pass it is issued into.
	descriptor.colorFormat = GPU::PixelFormat::BGRA8Unorm;
	descriptor.sampleCount = view->sampleCount();

	descriptor.blend = R_EacpBlendState( pipelineBits );
	descriptor.colorWriteMask = R_EacpColorWriteMask( pipelineBits );

	// The attachment carries both planes, so every pipeline drawing into it has
	// to say so - including one that only wants the depth test, and including
	// one that wants neither. What decides whether the test runs is the
	// comparison: a 2D view is Always with no write, which is what
	// RB_BeginDrawingView's glDisable( GL_DEPTH_TEST ) means.
	descriptor.depth = true;
	descriptor.depthCompare = R_EacpDepthCompare( pipelineBits );
	descriptor.depthWrite = !( pipelineBits & GLS_DEPTHMASK );

	// StencilFace's defaults are always-test, never-write, which is exactly
	// glDisable( GL_STENCIL_TEST ). The shadow pass sets them; step 4d.3.
	descriptor.stencil = true;

	descriptor.cullMode = R_EacpCullMode( cullType );

	statePipeline_t	entry;

	entry.stateBits = pipelineBits;
	entry.cullType = cullType;
	entry.pipeline = new GPU::RenderPipeline( GPU::Device::shared(), descriptor );

	if ( !entry.pipeline->isValid() ) {
		common->Warning( "eacp: no pipeline for state 0x%x, cull %i", pipelineBits, cullType );
		delete entry.pipeline;
		entry.pipeline = NULL;
	}

	variant.pipelines.add( entry );

	draw.pipeline = entry.pipeline;

	return draw;
}

/*
====================
	Geometry.

	idVertexCache hands out plain system-memory pointers on this backend -
	ARBVertexBufferObjectAvailable is false, so it never generates a buffer
	object - and a draw has to put those bytes somewhere the GPU can read them.
	StreamingBuffers is that somewhere, and it is idVertexCache's own shape: a
	pool per frame that may still be in flight, recycled only once the frame
	that used it cannot be on the GPU any more.

	Several writes in one frame get several buffers, which is the part that
	matters here: a 2D frame is hundreds of small draws and every one of them is
	still queued when the next is written.
====================
*/
const GPU::Buffer &idEacpRenderProgs::StreamVertices( const void *data, std::size_t bytes ) {
	return vertexStream.write( data, bytes );
}

const GPU::Buffer &idEacpRenderProgs::StreamIndices( const void *data, std::size_t bytes ) {
	return indexStream.write( data, bytes );
}
