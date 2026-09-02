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

// common->Warning, reached through a function because this header cannot see
// Doom 3's own - see the note above. Its one caller is the variant lookup
// below, which is a template and so has to live here.
void R_EacpShaderCompileFailed( const char *what );

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

	idEacpGammaProgram

	What r_gammaInShader adds to a program, for the programs that get it.

	On OpenGL the correction is a MUL_SAT and three POWs that R_LoadARBProgram
	splices into the source of every ARB fragment program the engine loads
	(draw_arb2.cpp, "dhewm3tmpres") - and *only* those. A fixed-function stage
	has no program to splice into, so the 2D, the ambient passes, the fog and
	the blend lights are drawn on that build with no gamma at all, and this
	build draws them the same way. What had a fragment program there, and so
	derives from this here, is the interaction and the two reflection programs.

	The uniform is program.env[21] with a flag beside it: ( r_brightness,
	1 / r_gamma, apply, 0 ). GammaCorrected says why the flag exists.

================================================================================
*/

class idEacpGammaProgram : public eacp::GPU::ShaderProgram {
public:
	eacp::GPU::Uniform<eacp::GPU::Float4>		gammaBrightness;

protected:
	// The fragment colour through the correction, for a define() to hand to
	// setFragment. A member rather than a free function because the branch it
	// records goes through ShaderProgram's protected var() and ifThen().
	eacp::GPU::Float4		GammaCorrected( const eacp::GPU::Float4 &color );
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

	The mirror clip plane rides on that discard rather than adding a third
	dimension to the cache, and clipPlane below says why.

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

	// The mirror clip plane, in the surface's own coordinates, dotted with the
	// vertex to give a signed distance the discard is taken on. (0, 0, 0, 1) is
	// no plane at all - every vertex a distance of 1 in front of it - and is
	// what every draw outside a subview's depth fill sets.
	//
	// It is a uniform rather than a third variant of this program, unlike the
	// alpha test above it: the discard is already there in this variant, and
	// what a plane changes is the value being compared rather than whether a
	// comparison is compiled at all.
	eacp::GPU::Uniform<eacp::GPU::Float4>		clipPlane;

	eacp::GPU::Uniform<eacp::GPU::Texture2D>	image;

	EACP_SHADER( modelViewProjection, textureMatrixS, textureMatrixT,
				 colorModulate, colorAdd, alphaTestRef, clipPlane, image )

private:
	bool					discards;
};

/*
================================================================================

	What a stage that is not TG_EXPLICIT samples its cube map with.

	Doom 3 has four cube texgens and this has three, because two of them are one
	program: TG_SKYBOX_CUBE is TG_WOBBLESKY_CUBE whose matrix is the identity,
	and R_LocalPointToGlobal through an identity 3x3 returns the vector it was
	given, bit for bit. So the wobble costs the sky three dot products per vertex
	and saves a program.

	The fourth, TG_REFLECT_CUBE, is two programs rather than one - the material
	either has a bump stage or it does not, and RB_PrepareStageTexturing picks
	`bumpyEnvironment.vfp` or `environment.vfp` on exactly that test. Only the
	unbumped one is here; the other is idEacpBumpyReflectProgram below, because
	it samples a second texture and works in a different space.

================================================================================
*/

enum eacpCubeTexgen_t {
	// TG_SKYBOX_CUBE and TG_WOBBLESKY_CUBE: the vector from the eye to the
	// vertex, in model space, turned by a 3x3 the frontend rebuilds every frame
	// for the wobble and left as the identity for the plain sky.
	ECT_SKY,

	// TG_DIFFUSE_CUBE: the vertex normal, straight through.
	ECT_DIFFUSE,

	// TG_REFLECT_CUBE on a material with no bump stage: environment.vfp.
	ECT_REFLECT
};

/*
================================================================================

	idEacpCubeProgram

	The generic material stage again, with the texture coordinate computed
	rather than read - a sky, a reflection or a diffuse cube instead of a
	surface's own (s, t).

	It is a program beside idEacpStageProgram rather than a variant of it, and
	the reason is eacp's rather than a preference. A shader's textures are
	declared by the uniform members it lists, all of them, whether define()
	samples them or not - so a stage program that grew a TextureCube would
	declare one on every TG_EXPLICIT variant too, and Metal's validation layer
	rejects a draw with a declared texture left unbound. The four samplings times
	the alpha test that step 4c compiled therefore stay exactly what they were,
	which is also what keeps every gate frame without cube content byte-identical.

	Three of Doom 3's four cube texgens go through the fixed-function pipeline
	rather than an ARB program, so their colour is the ordinary
	(modulate, add) pair. The fourth, ECT_REFLECT, is environment.vfp, where the
	fragment program replaces the texture-env combiner outright - so the second
	texture unit the fixed-function path binds the white image on has no effect
	and the colour is `vertex.color` alone. The caller is what knows the
	difference; this end of it is the same two uniforms either way.

================================================================================
*/

class idEacpCubeProgram : public idEacpGammaProgram {
public:
							idEacpCubeProgram( eacpCubeTexgen_t texgen,
											   eacp::GPU::TextureSampling sampling );

	virtual void			define( void ) override;

	eacp::GPU::Uniform<eacp::GPU::Float4x4>		modelViewProjection;

	// The eye in the surface's own coordinates, which is R_SkyboxTexGen's
	// localViewOrigin and environment.vfp's program.env[5]. Unread by
	// ECT_DIFFUSE, whose coordinate is the normal and depends on nothing else.
	eacp::GPU::Uniform<eacp::GPU::Float4>		localViewOrigin;

	// R_WobbleskyTexGen's 3x3, in the 4x4 Doom 3 writes it as - and the identity
	// for a plain skybox, which is what makes the two one program. Read by
	// ECT_SKY alone.
	eacp::GPU::Uniform<eacp::GPU::Float4x4>		texgenMatrix;

	eacp::GPU::Uniform<eacp::GPU::Float4>		colorModulate;
	eacp::GPU::Uniform<eacp::GPU::Float4>		colorAdd;

	eacp::GPU::Uniform<eacp::GPU::TextureCube>	cubeImage;

	// idEacpGammaProgram's gammaBrightness is read by ECT_REFLECT alone:
	// environment.vfp is a fragment program on the ARB path and gets the
	// injected correction, where a skybox and a diffuse cube are fixed-function
	// texgens there and get none.
	EACP_SHADER( modelViewProjection, localViewOrigin, texgenMatrix,
				 colorModulate, colorAdd, cubeImage, gammaBrightness )

private:
	eacpCubeTexgen_t		texgen;
};

/*
================================================================================

	idEacpBumpyReflectProgram

	bumpyEnvironment.vfp: TG_REFLECT_CUBE on a material that also has a bump
	stage, which RB_PrepareStageTexturing decides on `GetBumpStage() != NULL`
	and nothing else.

	It is not environment.vfp with a normal map bolted on - it works in a
	different space. environment.vfp reflects a model-space eye vector about the
	model-space vertex normal and samples the cube with the result, so its
	reflection turns with the object; this one carries both into *global* space
	first, through the three rows of the model matrix the vertex program is
	handed as program.env[6], [7] and [8], and reflects there. That is what lets
	a bumped surface reflect a cube map that is fixed to the world.

	The normal map is read the way every normal map in this engine is read: x out
	of the alpha channel, because idImage::GenerateImage swaps red and alpha on
	upload so that one fragment program serves both the compressed and the
	uncompressed form. `MOV localNormal.x, localNormal.a` in the original.

	And it writes rgb only - `MOV result.color.xyz, R0` - taking neither the
	vertex colour nor the stage's constant, which is a difference from
	environment.vfp beside it and reads like an oversight in the original rather
	than a decision. It is reproduced rather than corrected.

================================================================================
*/

class idEacpBumpyReflectProgram : public idEacpGammaProgram {
public:
							idEacpBumpyReflectProgram( eacp::GPU::TextureSampling cube,
													   eacp::GPU::TextureSampling bump );

	virtual void			define( void ) override;

	eacp::GPU::Uniform<eacp::GPU::Float4x4>		modelViewProjection;

	// program.env[5]: the eye in the surface's own coordinates.
	eacp::GPU::Uniform<eacp::GPU::Float4>		localViewOrigin;

	// program.env[6], [7] and [8]: the three rows of the model matrix, which
	// turn a model-space direction into a global one. Rows rather than a matrix
	// because the original is three DP3s and because the w each of them carries
	// is the model's translation, which a direction must not pick up.
	eacp::GPU::Uniform<eacp::GPU::Float4>		modelRowX;
	eacp::GPU::Uniform<eacp::GPU::Float4>		modelRowY;
	eacp::GPU::Uniform<eacp::GPU::Float4>		modelRowZ;

	eacp::GPU::Uniform<eacp::GPU::TextureCube>	cubeImage;
	eacp::GPU::Uniform<eacp::GPU::Texture2D>	bumpImage;

	// idEacpGammaProgram's gammaBrightness, as on the interaction program:
	// bumpyEnvironment.vfp is a fragment program on the ARB path and gets the
	// injected correction.
	EACP_SHADER( modelViewProjection, localViewOrigin,
				 modelRowX, modelRowY, modelRowZ,
				 cubeImage, bumpImage, gammaBrightness )
};

/*
================================================================================

	idEacpScreenProgram

	TG_SCREEN and TG_SCREEN2, which are the same texgen written out twice in
	RB_PrepareStageTexturing - the two branches are identical line for line, so
	they are one program here and the plan's two entries are one.

	It is the only texgen that needs no cube map. What it computes is where the
	vertex lands *on the screen*: rows 0, 1 and 3 of modelView x projection,
	dotted with the vertex, which is the x, y and w of the clip-space position
	the same vertex is drawn at. So a surface sampling `_currentRender` through
	it reads the pixel it is about to cover, which is what a refraction or a
	distortion wants.

	OpenGL says that with three object-linear texgens and a texture matrix over
	the homogeneous result; here the planes are three uniforms, the matrix is the
	same two rows every other stage's is, and the divide is the fragment's -
	which is what a projective texture read is.

	The image is whatever the stage names and is usually `_currentRender`, which
	step 4e.3 fills in.

================================================================================
*/

class idEacpScreenProgram : public eacp::GPU::ShaderProgram {
public:
							idEacpScreenProgram( eacp::GPU::TextureSampling sampling );

	virtual void			define( void ) override;

	eacp::GPU::Uniform<eacp::GPU::Float4x4>		modelViewProjection;

	// GL_OBJECT_PLANE for S, T and Q: rows 0, 1 and 3 of the same product the
	// position is transformed by, each dotted with (x, y, z, 1).
	eacp::GPU::Uniform<eacp::GPU::Float4>		screenPlaneS;
	eacp::GPU::Uniform<eacp::GPU::Float4>		screenPlaneT;
	eacp::GPU::Uniform<eacp::GPU::Float4>		screenPlaneQ;

	// The stage's texture matrix, applied to the homogeneous coordinate as GL's
	// texture matrix is - so these two rows are dotted with (s, t, q, 0) rather
	// than with (s, t, 1, 0) the way an explicit stage's are.
	eacp::GPU::Uniform<eacp::GPU::Float4>		textureMatrixS;
	eacp::GPU::Uniform<eacp::GPU::Float4>		textureMatrixT;

	eacp::GPU::Uniform<eacp::GPU::Float4>		colorModulate;
	eacp::GPU::Uniform<eacp::GPU::Float4>		colorAdd;

	eacp::GPU::Uniform<eacp::GPU::Texture2D>	image;

	EACP_SHADER( modelViewProjection,
				 screenPlaneS, screenPlaneT, screenPlaneQ,
				 textureMatrixS, textureMatrixT,
				 colorModulate, colorAdd, image )
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

class idEacpInteractionProgram : public idEacpGammaProgram {
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

	// gammaBrightness is idEacpGammaProgram's, and it is in the list below
	// because RB_ARB2_DrawInteraction sets env[21] on every interaction.

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
				 ambientLightVector, gammaBrightness,
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

	idEacpFogProgram

	RB_FogPass, which is the one thing Doom 3 draws that is a *volume* rather
	than a surface: a fog light adds a colour to everything inside it, in
	proportion to how far the viewer is looking through it.

	The fixed-function original is two texture units with the default modulate
	combiner between them, driven entirely by object-linear texgens, and each of
	its two textures answers a different question:

	  - **_fog**, a 128x128 radial ramp of 1 - 0.982^d, read at
	    ( 0.5 - distance/fogDistance, 0.5 ) - so a fragment's alpha is how much
	    fog is between it and the eye. The image is two-dimensional and only its
	    middle row is ever sampled, because the s coordinate is the whole of the
	    answer and the comment over R_FogImage says why: "we calculate distance
	    correctly in two planes, but the third will still be projection based".
	  - **_fogEnter**, a 64x64 correction read at the distances of the *viewer*
	    and of the fragment from the fog volume's top plane. What it fixes is the
	    terminator: a viewer half in and half out of a fog volume should see fog
	    only over the part of each ray that is inside it, and R_FogEnterImage's
	    FogFraction tabulates exactly that fraction for every pair of heights.

	**Both stay textures rather than becoming arithmetic**, which is the opposite
	of the decision step 4d.2 made about the specular ramp - and for the opposite
	reason. That ramp was a curve the ARB program could not evaluate, so the
	table was the workaround and the curve was the intent. These two are not:
	one is an exponential accumulated by repeated multiplication over 256 steps
	and the other is a piecewise function of two variables with four cases and a
	deep-water blend. Reproducing either in the shader would be reproducing a
	generator, and keeping them keeps the picture bit-for-bit closest to the
	OpenGL build's - which is what every step of this port is measured against.

	One program and no variants. Both images are generated rather than loaded -
	R_FogImage and R_FogEnterImage, Image_init.cpp - and both are generated
	TF_LINEAR with TR_CLAMP, so there is exactly one sampling tuple a fog light
	can ask for. If either generator ever changed its mind, this would need the
	variant list idEacpBlendLightProgram below has.

================================================================================
*/

class idEacpFogProgram : public eacp::GPU::ShaderProgram {
public:
							idEacpFogProgram();

	virtual void			define( void ) override;

	eacp::GPU::Uniform<eacp::GPU::Float4x4>		modelViewProjection;

	// The fog's density plane in the surface's own coordinates: dotted with the
	// vertex it gives the s the _fog image is read at. It is the view's forward
	// axis scaled by -0.5/fogDistance with 0.5 added to the constant term, so a
	// vertex at the eye reads the middle of the image and one a fog distance
	// away reads its edge.
	eacp::GPU::Uniform<eacp::GPU::Float4>		fogPlane;

	// The two coordinates _fogEnter is read at, also as planes in the surface's
	// own space. t is the fragment's height above the fog volume's top plane
	// and s is the viewer's - which is why s is a plane with no normal at all,
	// its constant term being the whole of the answer.
	eacp::GPU::Uniform<eacp::GPU::Float4>		fogEnterPlaneS;
	eacp::GPU::Uniform<eacp::GPU::Float4>		fogEnterPlaneT;

	// The fog's colour, from the light material's single stage. Its alpha is 1
	// rather than the register's, because RB_FogPass sends it with qglColor3fv
	// and GL fills the fourth channel in - the register's own alpha is the fog
	// *distance* and has already been spent on fogPlane above.
	eacp::GPU::Uniform<eacp::GPU::Float4>		fogColor;

	eacp::GPU::Uniform<eacp::GPU::Texture2D>	fogImage;
	eacp::GPU::Uniform<eacp::GPU::Texture2D>	fogEnterImage;

	EACP_SHADER( modelViewProjection, fogPlane, fogEnterPlaneS, fogEnterPlaneT,
				 fogColor, fogImage, fogEnterImage )
};

/*
================================================================================

	idEacpBlendLightProgram

	RB_BlendLight, which is the other half of RB_STD_FogAllLights and is a
	simpler idea than the fog beside it: a blend light projects its image onto
	everything inside it and blends the result in with whatever the light
	material's stage asked for, instead of interacting with the surface the way
	a real light does. `fogs/glare` is a light that adds a glow to a volume;
	`fogs/pitFog` is one that fades a pit to black with distance.

	Two textures, and they are the light's own two - the projected image from
	the stage and the falloff the light material declares - sampled the way the
	interaction program samples the same pair: the projection projectively, at
	( s/q, t/q ), and the falloff at one coordinate. So this is the light half
	of interaction.vfp with the surface half deleted, which is exactly what the
	fixed-function original is: two texture units, both driven by object-linear
	texgens off the light's projection planes, modulated together and by the
	stage's colour.

	**The variants are a list keyed on two samplings**, the way
	idEacpRenderProgs::InteractionDraw's are keyed on five. Both images are
	declared by the light material, so a light that repeats where its neighbour
	clamps is a second program - and a projection sampled with the wrong address
	mode tiles a light across a level rather than confining it to its volume,
	which is the same reason the interaction program's key counts them.

================================================================================
*/

class idEacpBlendLightProgram : public eacp::GPU::ShaderProgram {
public:
	struct sampling_t {
		eacp::GPU::TextureSampling	projection;
		eacp::GPU::TextureSampling	falloff;
	};

							idEacpBlendLightProgram( const sampling_t &sampling );

	virtual void			define( void ) override;

	eacp::GPU::Uniform<eacp::GPU::Float4x4>		modelViewProjection;

	// The light's projection in the surface's own coordinates, with the light
	// stage's texture matrix already folded into the first two planes by
	// RB_BakeTextureMatrixIntoTexgen - which is what the fixed-function path
	// gets from a matrix on the texture unit and what R_SetDrawInteraction does
	// with the same four planes for a real light.
	eacp::GPU::Uniform<eacp::GPU::Float4>		lightProjectionS;
	eacp::GPU::Uniform<eacp::GPU::Float4>		lightProjectionT;
	eacp::GPU::Uniform<eacp::GPU::Float4>		lightProjectionQ;
	eacp::GPU::Uniform<eacp::GPU::Float4>		lightFalloffS;

	// The light stage's colour, alpha included - which is the difference
	// RB_BlendLight's own comment draws against a normal light, whose alpha
	// never reaches the blend.
	eacp::GPU::Uniform<eacp::GPU::Float4>		color;

	eacp::GPU::Uniform<eacp::GPU::Texture2D>	lightImage;
	eacp::GPU::Uniform<eacp::GPU::Texture2D>	lightFalloffImage;

	EACP_SHADER( modelViewProjection,
				 lightProjectionS, lightProjectionT, lightProjectionQ, lightFalloffS,
				 color, lightImage, lightFalloffImage )
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

	// The same, for a stage whose coordinate is generated rather than read.
	// Three entry points because they are three programs (see the classes), and
	// each returns the same pair for the same reason: a null pipeline is a
	// compile that failed and a draw the caller should skip.
	struct cubeDraw_t {
		idEacpCubeProgram *						program;
		const eacp::GPU::RenderPipeline *		pipeline;
	};

	cubeDraw_t				CubeDraw( eacpCubeTexgen_t texgen, const idImage *cube,
									  int stateBits, int cullType );

	struct bumpyReflectDraw_t {
		idEacpBumpyReflectProgram *				program;
		const eacp::GPU::RenderPipeline *		pipeline;
	};

	bumpyReflectDraw_t		BumpyReflectDraw( const idImage *cube, const idImage *bump,
											  int stateBits, int cullType );

	struct screenDraw_t {
		idEacpScreenProgram *					program;
		const eacp::GPU::RenderPipeline *		pipeline;
	};

	screenDraw_t			ScreenDraw( const idImage *image, int stateBits, int cullType );

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

	// One fog light's volume, and one stage of one blend light. Neither takes a
	// stencil, because RB_STD_FogAllLights brackets both with
	// glDisable( GL_STENCIL_TEST ) - the pass that would have read a count is
	// over by the time either of these draws.
	struct fogDraw_t {
		idEacpFogProgram *						program;
		const eacp::GPU::RenderPipeline *		pipeline;
	};

	fogDraw_t				FogDraw( int stateBits, int cullType );

	struct blendLightDraw_t {
		idEacpBlendLightProgram *				program;
		const eacp::GPU::RenderPipeline *		pipeline;
	};

	blendLightDraw_t		BlendLightDraw( const idImage *projection, const idImage *falloff,
											int stateBits, int cullType );

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

	// The same four fields for a texgen program, said once rather than three
	// times. A template because that is the whole of what differs between the
	// three: the key is an int either way, and a library and a list of pipelines
	// are what any compiled program has.
	//
	// interactionVariant_t above is deliberately left as it is. It has the same
	// shape, and folding it in would be a change to the one program whose output
	// this step is not allowed to move.
	template <class Program>
	struct texgenVariant_t {
		int											key;
		std::optional<Program>						program;
		std::optional<eacp::GPU::ShaderLibrary>		library;
		eacp::Vector<statePipeline_t>				pipelines;
	};

	// The lookup those three share, and PipelineFor's sibling: the variant for a
	// key, compiled the first time it is asked for. `make` is what the program's
	// constructor wants, which is the one thing the three do not have in common,
	// so it arrives as a callable rather than as an argument list this would
	// have to know the shape of.
	//
	// NULL if the shader would not compile, and the failed program is dropped so
	// the next draw asks again rather than dereferencing an empty optional. The
	// entry stays in the list with no program in it, which is what keeps a broken
	// shader from being recompiled once per draw for the rest of the run.
	template <class Program, class Make>
	texgenVariant_t<Program> *
							TexgenVariant( eacp::OwnedVector<texgenVariant_t<Program>> &list,
										   int key, const char *what, Make make ) {
		texgenVariant_t<Program> *	variant = NULL;

		for ( int i = 0 ; i < list.size() ; i++ ) {
			if ( list[i]->key == key ) {
				variant = list[i];
				break;
			}
		}

		if ( !variant ) {
			variant = &list.createNew();

			variant->key = key;
			make( variant->program );
			variant->library.emplace( eacp::GPU::Device::shared(),
									  variant->program->source() );

			if ( !variant->library->isValid() ) {
				R_EacpShaderCompileFailed( what );
				variant->program.reset();
			}
		}

		return variant->program.has_value() ? variant : NULL;
	}

	// The two counters over any of those caches - the loops NumPrograms and
	// NumPipelines run over the interaction one, said once for the three that
	// came after it.
	template <class Program>
	int						CountPrograms( const eacp::OwnedVector<texgenVariant_t<Program>> &list ) const {
		int	total = 0;

		for ( int i = 0 ; i < list.size() ; i++ ) {
			if ( list[i]->program.has_value() ) {
				total++;
			}
		}

		return total;
	}

	template <class Program>
	int						CountPipelines( const eacp::OwnedVector<texgenVariant_t<Program>> &list ) const {
		int	total = 0;

		for ( int i = 0 ; i < list.size() ; i++ ) {
			total += list[i]->pipelines.size();
		}

		return total;
	}

	// The three texgen caches. Each key packs what the program is compiled
	// against and nothing else: the cube's is its texgen and its sampling, the
	// bumpy reflect's is its two samplings, and the screen's is its one - so the
	// key spaces are 12, 16 and 4, of which the demo's first level reaches three.
	eacp::OwnedVector<texgenVariant_t<idEacpCubeProgram>>			cubes;
	eacp::OwnedVector<texgenVariant_t<idEacpBumpyReflectProgram>>	bumpyReflects;
	eacp::OwnedVector<texgenVariant_t<idEacpScreenProgram>>			screens;

	// The shadow program, which has no variants - see the class. What it does
	// have is more pipelines than either of the others per program: the count
	// in its two forms, the clear, and the mirrored pair of the count.
	std::optional<idEacpShadowProgram>		shadowProgram;
	std::optional<eacp::GPU::ShaderLibrary>	shadowLibrary;
	eacp::Vector<statePipeline_t>			shadowPipelines;

	// The fog program, which has no variants either and for a different reason
	// than the shadow one: it samples two textures rather than none, but both
	// are generated by the engine at a fixed filter and repeat, so there is one
	// tuple to compile for. Its pipelines are the two RB_FogPass draws in - the
	// surfaces at GLS_DEPTHFUNC_EQUAL and the light's own frustum at
	// GLS_DEPTHFUNC_LESS with the cull reversed.
	std::optional<idEacpFogProgram>			fogProgram;
	std::optional<eacp::GPU::ShaderLibrary>	fogLibrary;
	eacp::Vector<statePipeline_t>			fogPipelines;

	// The blend light's variants, keyed the way the interaction program's are
	// and for the same reason - the two images are the light material's, so
	// their sampling is content rather than a constant. Two two-bit indices is
	// a key space of sixteen and a level reaches one or two of them, so a
	// linear search over an OwnedVector is again the whole of the lookup.
	struct blendLightVariant_t {
		int											key;
		std::optional<idEacpBlendLightProgram>		program;
		std::optional<eacp::GPU::ShaderLibrary>		library;
		eacp::Vector<statePipeline_t>				pipelines;
	};

	eacp::OwnedVector<blendLightVariant_t>	blendLights;

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
