/*
===========================================================================

dhewm 3 on eacp - the programs the backend draws with, and what a draw is
compiled against.

Doom 3's backend expresses a draw as fixed-function state: a matrix stack, a
texture-env combiner, a colour, a blend function. A modern API has none of those
- it has a *pipeline*, compiled from all of that state at once, and a shader
that does whatever the combiner used to. So the port needs two caches rather
than one:

  - a program per way a texture is sampled, because eacp bakes the sampler into
    the shader (GPU/SAMPLERS.md, and plan.md section 4.3), and

  - a pipeline per piece of Doom 3 state, because the blend equation, the depth
    test and the cull mode are all compiled into one object.

Both are built the first time they are asked for, so the port pays for the
combinations the content actually contains rather than for the ones it could.

This header is deliberately free of Doom 3's own headers - one forward
declaration and nothing else - so it can be included *before* them. idlib/Str.h
turns strcmp and eight of its neighbours into macros, which breaks any standard
header pulled in after it, and eacp's headers are full of standard ones.

===========================================================================
*/

#ifndef __RENDERPROGS_EACP_H__
#define __RENDERPROGS_EACP_H__

#include <eacp/GPU/GPU.h>

#include <optional>

class idImage;

/*
================================================================================

	The CPU side of a uniform, named.

	eacp's Uniform<Float4> is assigned a packed Array<float, 4> and its
	Uniform<Float4x4> an Array<float, 16> - the same bytes the shader reads, in
	the same order. The types are worth names for the same reason the vertex
	struct below is: an eacp::Array<float, 16> at a call site says how big it is
	and not what it is, and this is the difference between a matrix and four
	colours in a row.

	The two makers are what keep a matrix upload from being sixteen
	comma-separated subscripts, which is what it was before they existed.

================================================================================
*/

using Float4 = eacp::Array<float, 4>;
using Float4x4 = eacp::Array<float, 16>;

inline Float4 asFloat4( float x, float y, float z, float w ) {
	return Float4 { x, y, z, w };
}

// Doom 3 keeps its matrices as float[16] in OpenGL's column-major order, which
// is also MSL's and HLSL's as eacp generates them - so this is a copy rather
// than a transpose.
inline Float4x4 asFloat4x4( const float m[16] ) {
	return Float4x4 { m[0],  m[1],  m[2],  m[3],
					  m[4],  m[5],  m[6],  m[7],
					  m[8],  m[9],  m[10], m[11],
					  m[12], m[13], m[14], m[15] };
}

/*
================================================================================

	idDrawVert, said in eacp's terms.

	The same sixty bytes in the same order, so the vertex the engine already has
	is uploaded as it stands rather than repacked per draw. What this adds is
	the wire format of the one field whose C++ type does not imply it: colour is
	four bytes read as 0..1, which is what UNorm8x4 declares and what a plain
	byte[4] would have been taken for four separate floats.

	RenderProgs_Eacp.cpp static_asserts this against idDrawVert, so a change to
	either is a compile error rather than geometry that comes out scrambled.

================================================================================
*/

struct eacpDrawVert_t {
	float					xyz[3];
	float					st[2];
	float					normal[3];
	float					tangents[2][3];
	eacp::GPU::UNorm8x4		color;
};

/*
================================================================================

	idEacpStageProgram

	The generic material stage: a texture sampled at a transformed coordinate,
	multiplied by a colour that is part constant and part per-vertex.

	That one expression is the whole of what Doom 3 draws without a light, and
	the fixed-function pipeline it replaces needed two texture units and six
	glTexEnvi calls to say it. The three stageVertexColor_t modes are not three
	programs either - they are three (modulate, add) pairs through

	    colour = vertexColor * colorModulate + colorAdd

	which is SVC_IGNORE at (0, c), SVC_MODULATE at (c, 0) and
	SVC_INVERSE_MODULATE at (-c, c).

	It is also what fills the depth buffer, which is not a second program
	because it is not a second expression: RB_T_FillDepthBuffer draws a
	material's alpha-tested stages exactly as this draws its ambient ones, with
	the constant colour set to black and the alpha test on. So the only thing
	the depth fill adds is the discard - which cannot be a uniform, because a
	discard is a branch the generated source either has or does not, so it is
	the second dimension of the program cache rather than a bit of state.

================================================================================
*/

class idEacpStageProgram : public eacp::GPU::ShaderProgram {
public:
							idEacpStageProgram( eacp::GPU::TextureSampling sampling,
												bool alphaTest );

	virtual void			define( void ) override;

	// Clip from model, with the projection's z already mapped out of OpenGL's
	// [-1, 1] and into the [0, 1] both of eacp's backends clip against.
	eacp::GPU::Uniform<eacp::GPU::Float4x4>		modelViewProjection;

	// The two rows of Doom 3's texture matrix that do anything, each dotted
	// with (s, t, 1, 0). The third and fourth rows of the 4x4 it is written as
	// are the identity in every material the parser can produce.
	eacp::GPU::Uniform<eacp::GPU::Float4>		textureMatrixS;
	eacp::GPU::Uniform<eacp::GPU::Float4>		textureMatrixT;

	eacp::GPU::Uniform<eacp::GPU::Float4>		colorModulate;
	eacp::GPU::Uniform<eacp::GPU::Float4>		colorAdd;

	// What glAlphaFunc( GL_GREATER, ref ) compares against, on the alpha-test
	// variant. A register value, so it changes per stage and per frame and
	// cannot be the compile-time threshold setDiscardBelow takes - see define().
	eacp::GPU::Uniform<eacp::GPU::Float>		alphaTestRef;

	eacp::GPU::Uniform<eacp::GPU::Texture2D>	image;

	EACP_SHADER( modelViewProjection, textureMatrixS, textureMatrixT,
				 colorModulate, colorAdd, alphaTestRef, image )

private:
	bool					discards;
};

/*
================================================================================

	idEacpRenderProgs

	The two caches and the streaming pools, in one place because they are asked
	for together: a draw wants the program its texture is sampled through, the
	pipeline its state compiles to, and somewhere to put its geometry.

================================================================================
*/

class idEacpRenderProgs {
public:
							idEacpRenderProgs();
							~idEacpRenderProgs();

	// Everything the device owns, released before the device goes away.
	void					Shutdown( void );

	// What one stage draws with. Both halves are compiled on first use, so a
	// null pipeline means the compile failed and the caller should skip the
	// draw rather than issue one against nothing.
	struct stageDraw_t {
		idEacpStageProgram *					program;
		const eacp::GPU::RenderPipeline *		pipeline;
	};

	stageDraw_t				StageDraw( const idImage *image, int stateBits, int cullType,
									   bool alphaTest );

	// Geometry for one draw, in a buffer no frame still in flight is reading.
	// The reference is good until this frame's pool comes round again, which is
	// several frames after the draw that bound it was submitted.
	const eacp::GPU::Buffer &	StreamVertices( const void *data, std::size_t bytes );
	const eacp::GPU::Buffer &	StreamIndices( const void *data, std::size_t bytes );

	// How many programs and pipelines have been compiled, for the log line that
	// says what a level's content actually cost. Both numbers are the content's
	// answer to a question plan.md only sized: the first is how many of the
	// eight (sampling, alpha test) combinations a level reaches, and the second
	// how many pieces of Doom 3 state it draws them in.
	int						NumPrograms( void ) const;
	int						NumPipelines( void ) const;

private:
	// One pipeline, and the state it was compiled for. Held by pointer because
	// GPU::RenderPipeline is pinned in place - it owns native objects behind a
	// Pimpl and has no copy.
	struct statePipeline_t {
		int									stateBits;
		int									cullType;
		eacp::GPU::RenderPipeline *			pipeline;
	};

	// One compiled program, and everything built on it. The program holds the
	// generated source and the packed uniform block; the library is that source
	// compiled once; the pipelines are the states it has been asked to draw in.
	struct programVariant_t {
		std::optional<idEacpStageProgram>		program;
		std::optional<eacp::GPU::ShaderLibrary>	library;
		eacp::Vector<statePipeline_t>			pipelines;
	};

	// Two dimensions, because two things are baked into a shader's source
	// rather than set beside it: how its texture is sampled (GPU/SAMPLERS.md)
	// and whether it discards.
	programVariant_t		variants[eacp::GPU::samplingConfigurations][2];

	eacp::GPU::StreamingBuffers	vertexStream;
	eacp::GPU::StreamingBuffers	indexStream;
};

extern idEacpRenderProgs	eacpRenderProgs;

#endif /* !__RENDERPROGS_EACP_H__ */
