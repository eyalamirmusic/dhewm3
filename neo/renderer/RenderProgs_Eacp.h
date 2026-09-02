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

	idDrawVert's tangents are one idVec3[2] and here they are two members,
	because a vertex attribute is pulled out of this struct by a pointer to the
	field it comes from - and a pointer to float[2][3] names both tangents at
	once, which is not an attribute. Same twenty-four bytes at the same two
	offsets, said as the two things the interaction program actually reads.

	RenderProgs_Eacp.cpp static_asserts this against idDrawVert, so a change to
	either is a compile error rather than geometry that comes out scrambled.

================================================================================
*/

struct eacpDrawVert_t {
	float					xyz[3];
	float					st[2];
	float					normal[3];
	float					tangent[3];		// idDrawVert::tangents[0]
	float					bitangent[3];	// idDrawVert::tangents[1]
	eacp::GPU::UNorm8x4		color;
};

/*
================================================================================

	shadowCache_t, said in eacp's terms.

	A shadow volume's vertex is one homogeneous position and nothing else: the
	same xyz twice over, once with w = 1 and once with w = 0, so that a vertex
	program can leave the first where it is and send the second to infinity away
	from the light. That is the whole of the geometry a stencil shadow needs and
	the whole of what R_CreateVertexProgramShadowCache writes.

	Sixteen bytes, checked against idVec4 in RenderProgs_Eacp.cpp, because the
	engine's own buffer is what gets streamed rather than a repacked copy of it.

================================================================================
*/

struct eacpShadowVert_t {
	float					xyzw[4];
};

/*
================================================================================

	The blit's vertex, which is not the engine's at all.

	Six of these are the whole geometry of putting the finished frame on the
	screen: a quad already in clip space, with the texture coordinate that
	samples the render target it came out of. Nothing in Doom 3 produces them,
	because on OpenGL there is nothing to put on the screen - the frame is
	already in the back buffer.

================================================================================
*/

struct eacpBlitVert_t {
	float					xy[2];
	float					st[2];
};

/*
================================================================================

	What a draw does with the stencil buffer.

	On OpenGL this is three calls left in the context - glStencilFunc,
	glStencilOpSeparate twice - and here it is part of a pipeline, so it is a
	second key on the cache beside the GLS_* bits. The reference value is not:
	it is pass state on both of eacp's backends, set once per pass, which is
	right because Doom 3 uses one value for the whole frame.

	Doom 3 reaches five configurations and no more, all of them in the shadow
	half of a view. The two counting ones come in mirrored pairs because a
	mirror reverses which winding faces the viewer, and which face increments is
	the whole of what the count is.

================================================================================
*/

enum eacpStencil_t {
	// Always test, never write: glDisable( GL_STENCIL_TEST ). Everything drawn
	// outside the shadow half of a view.
	ES_IGNORE,

	// The per-light clear, which on OpenGL is glClear( GL_STENCIL_BUFFER_BIT )
	// inside the light's scissor and here is a quad that replaces what it
	// covers with the pass's reference value. A pass cannot be cleared once it
	// has begun on either of eacp's backends - the clear is a property of the
	// attachment being loaded - so the only way to empty a rectangle of the
	// stencil buffer mid-frame is to draw over it.
	ES_CLEAR,

	// Carmack's reverse: the shadow volume's count taken on the fragments the
	// depth test rejected, both facings in one pass over the geometry. This is
	// what glStencilOpSeparate buys and what a one-face-at-a-time API pays for
	// twice.
	ES_COUNT_DEPTH_FAIL,
	ES_COUNT_DEPTH_FAIL_MIRRORED,

	// The same count on the fragments the depth test kept, which is what a
	// volume the view is outside of can use instead - no caps needed, and no
	// near plane to fall through.
	ES_COUNT_DEPTH_PASS,
	ES_COUNT_DEPTH_PASS_MIRRORED,

	// The mask the count is read through: glStencilFunc( GL_GEQUAL, 128, 255 ),
	// which keeps the fragments no shadow volume closed over.
	ES_LIT
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

	idEacpInteractionProgram

	interaction.vfp, which is the whole of Doom 3's lighting: one light against
	one surface, bump-mapped, with a diffuse and a specular term, modulated by
	the light's projection and its falloff.

	The original is two hand-written ARB programs in one file - a vertex program
	that puts the light and the half-angle vector into the surface's tangent
	space and runs six texture matrices, and a fragment program that samples
	five textures plus two lookup tables. This is that expression, once, in a
	language that compiles to both MSL and HLSL.

	Two of the seven textures do not survive the move, and neither is a
	simplification of what is drawn:

	  - the normalization cube map, which exists because an ARB fragment program
	    cannot normalize a vector. normalize() can, and the .vfp says so itself:
	    the half-angle half of the shader already takes that road, with the cube
	    map sitting commented out above it.
	  - the specular lookup table, which is a 256x1 ramp of
	    max( 0, (d - 0.75) * 4 )^2 - a curve tabulated because the ARB program
	    "can't really do a power function", in the words of the comment over the
	    generator. Two multiplies here, and exact rather than quantized to the
	    table's eight bits.

	What does not go away is the fifth texture, and it is the reason this
	program is macOS-only for now: eacp's D3D12 root signature has four texture
	slots (plan.md section 5, gap 19), and bump, falloff, projection, diffuse
	and specular are five.

================================================================================
*/

class idEacpInteractionProgram : public eacp::GPU::ShaderProgram {
public:
	// The five samplings, in the order the textures are declared below. One
	// program per distinct tuple - see idEacpRenderProgs::InteractionDraw.
	struct sampling_t {
		eacp::GPU::TextureSampling	bump;
		eacp::GPU::TextureSampling	falloff;
		eacp::GPU::TextureSampling	projection;
		eacp::GPU::TextureSampling	diffuse;
		eacp::GPU::TextureSampling	specular;
	};

							idEacpInteractionProgram( const sampling_t &sampling );

	virtual void			define( void ) override;

	// Clip from model, as everywhere else. The ARB vertex program got this for
	// free from OPTION ARB_position_invariant, which is to say from the
	// fixed-function matrix stack it shared with the rest of the frame.
	eacp::GPU::Uniform<eacp::GPU::Float4x4>		modelViewProjection;

	// The light and the eye in the surface's own coordinates, which is what
	// lets the whole shader work in model space and never build a matrix.
	eacp::GPU::Uniform<eacp::GPU::Float4>		localLightOrigin;
	eacp::GPU::Uniform<eacp::GPU::Float4>		localViewOrigin;

	// The light's projection, as four planes dotted with the model-space
	// position: three for the projected image's (s, t, q) and one for the
	// falloff's single coordinate.
	eacp::GPU::Uniform<eacp::GPU::Float4>		lightProjectionS;
	eacp::GPU::Uniform<eacp::GPU::Float4>		lightProjectionT;
	eacp::GPU::Uniform<eacp::GPU::Float4>		lightProjectionQ;
	eacp::GPU::Uniform<eacp::GPU::Float4>		lightFalloffS;

	// Each map's own texture matrix, as the two rows R_SetDrawInteraction
	// fills: (a, b, 0, c) dotted with (s, t, 0, 1).
	eacp::GPU::Uniform<eacp::GPU::Float4>		bumpMatrixS;
	eacp::GPU::Uniform<eacp::GPU::Float4>		bumpMatrixT;
	eacp::GPU::Uniform<eacp::GPU::Float4>		diffuseMatrixS;
	eacp::GPU::Uniform<eacp::GPU::Float4>		diffuseMatrixT;
	eacp::GPU::Uniform<eacp::GPU::Float4>		specularMatrixS;
	eacp::GPU::Uniform<eacp::GPU::Float4>		specularMatrixT;

	// stageVertexColor_t as (modulate, add), exactly as the generic material
	// stage says it - and here it is the ARB program's own spelling too, which
	// is where that trick comes from.
	eacp::GPU::Uniform<eacp::GPU::Float4>		colorModulate;
	eacp::GPU::Uniform<eacp::GPU::Float4>		colorAdd;

	// The light's colour times the stage's, one for each term.
	eacp::GPU::Uniform<eacp::GPU::Float4>		diffuseColor;
	eacp::GPU::Uniform<eacp::GPU::Float4>		specularColor;

	// An ambient light has no direction: the ARB path swaps the normalization
	// cube map for _ambient, whose every texel is the same vector, so the
	// tangent-space light direction becomes a constant. xyz is that constant
	// and w selects it - 1 for an ambient light, 0 for every other kind.
	eacp::GPU::Uniform<eacp::GPU::Float4>		ambientLightVector;

	eacp::GPU::Uniform<eacp::GPU::Texture2D>	bumpImage;
	eacp::GPU::Uniform<eacp::GPU::Texture2D>	lightFalloffImage;
	eacp::GPU::Uniform<eacp::GPU::Texture2D>	lightImage;
	eacp::GPU::Uniform<eacp::GPU::Texture2D>	diffuseImage;
	eacp::GPU::Uniform<eacp::GPU::Texture2D>	specularImage;

	EACP_SHADER( modelViewProjection, localLightOrigin, localViewOrigin,
				 lightProjectionS, lightProjectionT, lightProjectionQ, lightFalloffS,
				 bumpMatrixS, bumpMatrixT, diffuseMatrixS, diffuseMatrixT,
				 specularMatrixS, specularMatrixT,
				 colorModulate, colorAdd, diffuseColor, specularColor,
				 ambientLightVector,
				 bumpImage, lightFalloffImage, lightImage, diffuseImage, specularImage )
};

/*
================================================================================

	idEacpShadowProgram

	shadow.vp, which is two instructions and the only vertex program Doom 3
	ships that has no fragment program beside it - because a shadow volume is
	rasterized so that the stencil ops fire and for no other reason. Nothing it
	computes is ever written.

	What the two instructions do is the extrusion: a shadow volume's vertices
	come in pairs, the same position with w = 1 and with w = 0, and the second
	of each pair is sent to infinity in the direction away from the light. That
	is why the volume can be built once per surface and reused for every frame
	the surface and the light are both still there - the projection is the
	shader's, not the geometry's.

	It is also the reason the *frontend* has to know which backend is running:
	tr.backEndRendererHasVertexPrograms is what makes R_CreateShadowVolume build
	the doubled cache this reads rather than a volume already projected on the
	CPU, and it is true for BE_EACP.

	This program has no variants at all. It samples nothing, so there is no
	sampling to bake in; it discards nothing, so there is no branch to compile
	twice. One program, and a pipeline per way the stencil is counted.

================================================================================
*/

class idEacpShadowProgram : public eacp::GPU::ShaderProgram {
public:
							idEacpShadowProgram();

	virtual void			define( void ) override;

	eacp::GPU::Uniform<eacp::GPU::Float4x4>		modelViewProjection;

	// The light in the surface's own coordinates, with w = 0 - which the
	// extrusion relies on, and which R_GlobalPointToLocal does not set, so the
	// caller does.
	eacp::GPU::Uniform<eacp::GPU::Float4>		localLightOrigin;

	EACP_SHADER( modelViewProjection, localLightOrigin )
};

/*
================================================================================

	idEacpBlitProgram

	The finished frame onto the screen: one texture, sampled at the coordinate
	the vertex carries, written out unchanged.

	It exists because the frame is no longer drawn into the thing that gets
	presented. Everything above composes into an app-owned render target, and
	this is what puts that target on the drawable - which is a draw here and is
	nothing at all on OpenGL, where the frame was in the back buffer the moment
	it was drawn.

	There is no transform because there is nothing to transform: the six
	vertices are already in clip space, and what the viewport does with them is
	the whole of the mapping.

================================================================================
*/

class idEacpBlitProgram : public eacp::GPU::ShaderProgram {
public:
							idEacpBlitProgram();

	virtual void			define( void ) override;

	eacp::GPU::Uniform<eacp::GPU::Texture2D>	image;

	EACP_SHADER( image )
};

/*
================================================================================

	idEacpRenderProgs

	The three caches and the streaming pools, in one place because they are
	asked for together: a draw wants the program its texture is sampled through,
	the pipeline its state compiles to, and somewhere to put its geometry.

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

	// The same for one light against one surface. Its five images arrive
	// together because the program samples all five and its sampling variant is
	// the tuple of what they ask for, not any one of them.
	//
	// The stencil is here and not in the state bits because that is where a
	// modern API keeps it: a light whose shadows have been counted is drawn
	// through a pipeline that tests the count, and one with no shadow-casting
	// surface through a pipeline that does not.
	struct interactionDraw_t {
		idEacpInteractionProgram *				program;
		const eacp::GPU::RenderPipeline *		pipeline;
	};

	interactionDraw_t		InteractionDraw( const idImage *bump, const idImage *falloff,
											 const idImage *projection, const idImage *diffuse,
											 const idImage *specular,
											 int stateBits, int cullType,
											 eacpStencil_t stencil );

	// One shadow volume, or the quad that clears the count before them. Both go
	// through the one program - the clear being that program with an identity
	// transform and the light at the origin, which makes its extrusion the
	// identity too - so what tells them apart is entirely the pipeline.
	struct shadowDraw_t {
		idEacpShadowProgram *					program;
		const eacp::GPU::RenderPipeline *		pipeline;
	};

	shadowDraw_t			ShadowDraw( int stateBits, int cullType, eacpStencil_t stencil );

	// The frame onto the screen, and the frame into an idImage's texture. Their
	// pipelines are the two things here that are not compiled against the
	// render target - one draws into the drawable and takes its sample count,
	// the other into an image and takes its format - so the one program is
	// asked for through whichever of the two the destination calls for.
	struct blitDraw_t {
		idEacpBlitProgram *						program;
		const eacp::GPU::RenderPipeline *		pipeline;
	};

	blitDraw_t				BlitDraw( void );
	blitDraw_t				CaptureDraw( void );

	// Geometry for one draw, in a buffer no frame still in flight is reading.
	// The reference is good until this frame's pool comes round again, which is
	// several frames after the draw that bound it was submitted.
	const eacp::GPU::Buffer &	StreamVertices( const void *data, std::size_t bytes );
	const eacp::GPU::Buffer &	StreamIndices( const void *data, std::size_t bytes );

	// How many programs and pipelines have been compiled, over all three
	// caches, for the log line that says what a level's content actually cost.
	// Both numbers are the content's answer to a question plan.md only sized:
	// how many of the sampling combinations a level reaches, and how many
	// pieces of Doom 3 state it draws them in.
	int						NumPrograms( void ) const;
	int						NumPipelines( void ) const;

private:
	// One pipeline, and the state it was compiled for. Held by pointer because
	// GPU::RenderPipeline is pinned in place - it owns native objects behind a
	// Pimpl and has no copy - and by an *owning* pointer because that is what
	// says who releases it. eacp::OwningPointer is a unique_ptr that still
	// converts to the raw pointer a draw is issued with, so the ownership costs
	// the call sites nothing.
	struct statePipeline_t {
		int											stateBits;
		int											cullType;
		eacpStencil_t								stencil;
		eacp::OwningPointer<eacp::GPU::RenderPipeline>	pipeline;
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

	// The interaction program's variants, and they are a list rather than an
	// array because there are five textures: four ways of sampling each is
	// 1024 combinations, of which a level reaches a handful. The key is those
	// five sampling indices packed two bits apiece, which is what makes a
	// lookup a comparison of one int.
	struct interactionVariant_t {
		int											key;
		std::optional<idEacpInteractionProgram>		program;
		std::optional<eacp::GPU::ShaderLibrary>		library;
		eacp::Vector<statePipeline_t>				pipelines;
	};

	// By pointer, because a ShaderProgram is pinned in place - its uniform
	// members hold nodes in the graph it owns - so it can neither be copied nor
	// moved, and a vector that grows does one or the other. An OwnedVector is
	// that vector of pointers with the ownership said out loud: it is a
	// Vector<OwningPointer<T>>, so the elements go when it does.
	eacp::OwnedVector<interactionVariant_t>	interactions;

	// The shadow program, which has no variants - see the class. What it does
	// have is more pipelines than either of the others per program: the count
	// in its two forms, the clear, and the mirrored pair of the count.
	std::optional<idEacpShadowProgram>		shadowProgram;
	std::optional<eacp::GPU::ShaderLibrary>	shadowLibrary;
	eacp::Vector<statePipeline_t>			shadowPipelines;

	// The blit, which has one program and no state to key it on - and two
	// pipelines, because it has two destinations: the drawable, which the frame
	// is presented on, and an idImage's texture, which is where _currentRender
	// and _scratch are kept. Neither is built by PipelineFor, that one being
	// written against the render target's attachments and these two being
	// written against something else.
	std::optional<idEacpBlitProgram>		blitProgram;
	std::optional<eacp::GPU::ShaderLibrary>	blitLibrary;
	eacp::OwningPointer<eacp::GPU::RenderPipeline>	blitPipeline;
	eacp::OwningPointer<eacp::GPU::RenderPipeline>	capturePipeline;

	// The program above, compiled on the first of the two draws that wants it.
	idEacpBlitProgram *		BlitProgram( void );

	// The pipeline all three caches build, from the state Doom 3 asks for and
	// the program that is going to be drawn with it. Empty if it would not
	// compile, which the caller answers by skipping the draw - and returned by
	// value because that is the handover: whoever takes it owns it.
	eacp::OwningPointer<eacp::GPU::RenderPipeline>
							BuildPipeline( const eacp::GPU::ShaderLibrary &library,
										   const eacp::GPU::VertexLayout &layout,
										   int stateBits, int cullType,
										   eacpStencil_t stencil );

	// The one lookup all three caches do: a linear search of the pipelines
	// already compiled for a program, and a compile when it is not there. The
	// list never grows past what the content contains, which is 26 pipelines
	// over every program in the demo's first level.
	const eacp::GPU::RenderPipeline *
							PipelineFor( eacp::Vector<statePipeline_t> &pipelines,
										 const eacp::GPU::ShaderLibrary &library,
										 const eacp::GPU::VertexLayout &layout,
										 int stateBits, int cullType,
										 eacpStencil_t stencil );

	eacp::GPU::StreamingBuffers	vertexStream;
	eacp::GPU::StreamingBuffers	indexStream;
};

extern idEacpRenderProgs	eacpRenderProgs;

#endif /* !__RENDERPROGS_EACP_H__ */
