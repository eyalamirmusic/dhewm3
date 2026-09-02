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
static_assert( offsetof( eacpDrawVert_t, normal ) == offsetof( idDrawVert, normal ),
			   "eacpDrawVert_t::normal must sit where idDrawVert::normal does" );
static_assert( offsetof( eacpDrawVert_t, tangent ) == offsetof( idDrawVert, tangents ),
			   "eacpDrawVert_t::tangent must sit where idDrawVert::tangents[0] does" );
static_assert( offsetof( eacpDrawVert_t, bitangent )
			   == offsetof( idDrawVert, tangents ) + sizeof( idVec3 ),
			   "eacpDrawVert_t::bitangent must sit where idDrawVert::tangents[1] does" );
static_assert( offsetof( eacpDrawVert_t, color ) == offsetof( idDrawVert, color ),
			   "eacpDrawVert_t::color must sit where idDrawVert::color does" );

// And the same for the shadow volume's, which is one homogeneous position: the
// vertex cache's own bytes are what a draw streams.
static_assert( sizeof( eacpShadowVert_t ) == sizeof( shadowCache_t ),
			   "eacpShadowVert_t must be shadowCache_t's layout exactly" );

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
	//
	// The mirror clip plane is the second thing that kills a fragment here, and
	// it is folded into the same value because a program has one discard: the
	// smaller of the two is below zero exactly when either of them is.
	//
	// What it replaces is Doom 3's own trick, which is worth writing down
	// because it is not obvious what the code is doing. RB_STD_FillDepthBuffer
	// binds _alphaNotch - two texels, alpha 0 then alpha 255, nearest and
	// clamped - on a second texture unit, and drives its s coordinate from an
	// object-linear texgen whose plane is this one with 0.5 added to the
	// distance. So a vertex in front of the plane samples the second texel and
	// one behind it the first, the modulate combiner multiplies the fragment's
	// alpha by that 0 or 1, and the alpha test does the rest. The comparison it
	// amounts to is dot( plane, vertex ) < 0, which is what is written here.
	if ( discards ) {
		auto	clipDistance = GPU::dot( GPU::float4( position, 1.0f ), clipPlane );

		setDiscardBelow( GPU::min( fragment.w() - alphaTestRef, varying( clipDistance ) ),
						 0.0f );
	}

	setFragment( fragment );
}

void R_EacpShaderCompileFailed( const char *what ) {
	common->Warning( "eacp: the %s shader failed to compile", what );
}

/*
================================================================================

	idEacpGammaProgram

================================================================================
*/

/*
====================
idEacpGammaProgram::GammaCorrected

The correction R_LoadARBProgram splices into an ARB fragment program, in the
EDSL:

    rgb = pow( saturate( rgb * brightness ), 1 / gamma )

It sits behind a branch on the uniform's flag rather than being computed and
multiplied out, and that is not an optimisation. At the default settings the
expression is pow( x, 1.0 ), which neither shading language promises to return
as exactly x, and a frame that differs by an ulp from the one before this
uniform existed is a frame the gate cannot tell from a regression. The branch is
on a uniform, so every fragment of a draw takes the same side of it, which is
the cheap kind.

The saturate is written out although an 8-bit attachment would clamp anyway,
because the pow reads the value before the attachment does, and a negative base
is what the original's MUL_SAT is there to keep away from POW. Alpha is left
alone, as the injected code leaves result.color.w.
====================
*/
GPU::Float4 idEacpGammaProgram::GammaCorrected( const GPU::Float4 &color ) {
	auto	rgb = var( color.xyz() );

	ifThen( gammaBrightness.z() > 0.5f, [&]() {
		auto	scaled = GPU::min( GPU::max( rgb.get() * gammaBrightness.x(), 0.0f ), 1.0f );

		rgb = GPU::pow( scaled, gammaBrightness.y() );
	} );

	return GPU::float4( rgb.get(), color.w() );
}

/*
================================================================================

	idEacpCubeProgram

================================================================================
*/

idEacpCubeProgram::idEacpCubeProgram( eacpCubeTexgen_t texgenKind,
									  GPU::TextureSampling sampling ) {
	// Before compile(), as everywhere: the build walk reads the sampling to
	// place the static sampler the generated source points at, and it reads the
	// texgen because define() branches on it.
	cubeImage.sampling = sampling;

	texgen = texgenKind;

	compile();
}

/*
====================
idEacpCubeProgram::define

Three coordinate generators and one expression over them.

What OpenGL does for the first two is not a shader at all - the frontend writes
a three-component texture coordinate into the vertex cache every frame
(R_SkyboxTexGen, R_WobbleskyTexGen) and glTexCoordPointer reads it. That buffer
is still written on this backend, by frontend code this port does not touch, and
it is ignored: the same arithmetic is four instructions in a vertex shader and
does not cost a per-frame allocation and an upload of three floats per vertex.
So `surf->dynamicTexCoords` is filled in and never bound, which is worth knowing
before someone goes looking for the bind.

The third is environment.vfp, and its two halves land where the original puts
them: the vertex program interpolates the normal and the vector to the eye, and
the fragment program normalizes both - because it is the *interpolated* pair
that has to be unit length, and normalizing per vertex would not make it so.
====================
*/
void idEacpCubeProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );
	auto	normal = vertexInput( &eacpDrawVert_t::normal );
	auto	vertexColor = vertexInput( &eacpDrawVert_t::color );

	setPosition( modelViewProjection * GPU::float4( position, 1.0f ) );

	auto	surfaceColor = varying( vertexColor * colorModulate + colorAdd );

	if ( texgen == ECT_REFLECT ) {
		// environment.vfp's two texture coordinates: the surface normal and the
		// vector to the eye, both in model space and both left as they are for
		// the fragment stage to normalize.
		auto	toEye = varying( localViewOrigin.xyz() - position );
		auto	surfaceNormal = varying( normal );

		auto	eye = GPU::normalize( toEye );
		auto	unitNormal = GPU::normalize( surfaceNormal );

		// MAD R0, R0, scaleTwo, -toEye over R0 = dot( toEye, normal ) * normal,
		// which is the reflection of the eye vector about the normal.
		auto	reflection = unitNormal * ( GPU::dot( eye, unitNormal ) * 2.0f ) - eye;

		// Through the gamma correction, which the two texgens below are not:
		// this one is a fragment program on the ARB path and they are
		// fixed-function.
		setFragment( GammaCorrected( GPU::sample( cubeImage, reflection ) * surfaceColor ) );
		return;
	}

	// The other two are a coordinate the vertex knows and the fragment
	// interpolates, exactly as the three-component texcoord OpenGL streams is.
	//
	// R_WobbleskyTexGen's transform is a rotation with no translation, and it is
	// applied by R_LocalPointToGlobal - which adds the fourth column. Multiplying
	// the homogeneous vector with w = 0 is that same product with the column it
	// must not pick up multiplied by zero, so this is the generator's arithmetic
	// rather than an approximation of it. A plain skybox sends the identity and
	// gets its own vector back unchanged, to the bit.
	auto	coordinate = ( texgen == ECT_DIFFUSE )
		? normal
		: ( texgenMatrix * GPU::float4( position - localViewOrigin.xyz(), 0.0f ) ).xyz();

	setFragment( GPU::sample( cubeImage, varying( coordinate ) ) * surfaceColor );
}

/*
================================================================================

	idEacpBumpyReflectProgram

================================================================================
*/

idEacpBumpyReflectProgram::idEacpBumpyReflectProgram( GPU::TextureSampling cube,
													  GPU::TextureSampling bump ) {
	cubeImage.sampling = cube;
	bumpImage.sampling = bump;

	compile();
}

/*
====================
idEacpBumpyReflectProgram::define

bumpyEnvironment.vfp, both halves.

The vertex half carries three things into global space through the model
matrix's rows: the vector to the eye, and the surface's tangent frame - which
arrives as three varyings holding one *component* each of the three basis
vectors, because that is the shape nine DP3s produce and because it is what
makes the fragment half's three DP3s the two changes of basis composed.

The fragment half is the same reflection environment.vfp computes, over a normal
the bump map perturbed rather than the vertex's own.

Two things the original leaves out are left out here. The bump map is sampled at
the surface's raw (s, t) and *not* through the stage's texture matrix - a vertex
program bypasses GL's texture matrix entirely, so a `scroll` on a reflect stage
has never done anything on the ARB2 path. And the colour: this writes the cube
sample and nothing else, where environment.vfp beside it multiplies by the
vertex colour.
====================
*/
void idEacpBumpyReflectProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );
	auto	texcoord = vertexInput( &eacpDrawVert_t::st );
	auto	normal = vertexInput( &eacpDrawVert_t::normal );
	auto	tangent = vertexInput( &eacpDrawVert_t::tangent );
	auto	bitangent = vertexInput( &eacpDrawVert_t::bitangent );

	setPosition( modelViewProjection * GPU::float4( position, 1.0f ) );

	// The model matrix's rows, as directions: the w each carries is the model's
	// translation and a DP3 never reads it.
	auto	rowX = modelRowX.xyz();
	auto	rowY = modelRowY.xyz();
	auto	rowZ = modelRowZ.xyz();

	auto	toGlobal = [&]( const GPU::Float3 &v ) {
		return GPU::float3( GPU::dot( v, rowX ), GPU::dot( v, rowY ), GPU::dot( v, rowZ ) );
	};

	// One component of each of the three basis vectors per varying, which is
	// what the nine DP3s in the original write and what the three in its
	// fragment program read back.
	auto	basisX = varying( GPU::float3( GPU::dot( tangent, rowX ),
										   GPU::dot( bitangent, rowX ),
										   GPU::dot( normal, rowX ) ) );
	auto	basisY = varying( GPU::float3( GPU::dot( tangent, rowY ),
										   GPU::dot( bitangent, rowY ),
										   GPU::dot( normal, rowY ) ) );
	auto	basisZ = varying( GPU::float3( GPU::dot( tangent, rowZ ),
										   GPU::dot( bitangent, rowZ ),
										   GPU::dot( normal, rowZ ) ) );

	auto	toEye = varying( toGlobal( localViewOrigin.xyz() - position ) );

	auto	bumpCoord = varying( texcoord );

	// x out of alpha, then out of [0, 1] and into [-1, 1], then normalized -
	// the same three steps the interaction program takes, and for the same
	// reason: idImage::GenerateImage swaps red and alpha on every normal map it
	// uploads.
	auto	bumpTexel = GPU::sample( bumpImage, bumpCoord );
	auto	localNormal = GPU::normalize( bumpTexel.wyz() * 2.0f - 1.0f );

	auto	globalNormal = GPU::float3( GPU::dot( localNormal, basisX ),
										GPU::dot( localNormal, basisY ),
										GPU::dot( localNormal, basisZ ) );

	auto	eye = GPU::normalize( toEye );

	auto	reflection = globalNormal * ( GPU::dot( eye, globalNormal ) * 2.0f ) - eye;

	// **Alpha is zero because the original writes nothing there at all, and zero
	// is the value that cannot change what is already in the buffer.**
	// `MOV result.color.xyz, R0` leaves w undefined by the ARB specification, so
	// there is no number here to copy - what there is instead is what the
	// materials the demo's maps place that reach this program do with it.
	// `textures/sfx/chiglass1blue` and `textures/decals/p_oppressive` are
	// `blend add`, where the destination alpha becomes src + dst;
	// `textures/outside/outfactory_new2` is `blend gl_dst_alpha, gl_one`, where
	// it becomes src * dst + dst. A source alpha of zero leaves the
	// destination's exactly as it was under both, so zero is the one value that
	// is invisible on every path the content actually takes, which is the best
	// a fragment program's undefined output can be reproduced as. (The three
	// `maskalpha` glasses - glass1, glass2, mc_medglass - never reach this
	// program: they have no lighting stage, so no implicit bump map, and take
	// idEacpCubeProgram's ECT_REFLECT instead.)
	setFragment( GammaCorrected( GPU::float4( GPU::sample( cubeImage, reflection ).xyz(),
											  0.0f ) ) );
}

/*
================================================================================

	idEacpScreenProgram

================================================================================
*/

idEacpScreenProgram::idEacpScreenProgram( GPU::TextureSampling sampling ) {
	image.sampling = sampling;

	compile();
}

/*
====================
idEacpScreenProgram::define

Three object-linear texgens and a projective read.

The planes are rows 0, 1 and 3 of modelView x projection, so (s, t, q) is the
(x, y, w) of the clip-space position this same vertex is drawn at - and s/q,
t/q is therefore where on the screen it lands, in the [0, 1] a texture is
sampled in only because the projection's own [-1, 1] has already been halved and
shifted by the plane rows themselves. Whatever the material names is read at
that point, which for every user of this in the game is `_currentRender`.

The divide is in the fragment stage rather than the vertex stage, and that is
not an optimisation to undo: interpolating s/q would give the wrong answer
everywhere except the vertices, which is the whole reason GL_TEXTURE_GEN_Q
exists.
====================
*/
void idEacpScreenProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );
	auto	vertexColor = vertexInput( &eacpDrawVert_t::color );

	auto	model = GPU::float4( position, 1.0f );

	setPosition( modelViewProjection * model );

	auto	s = GPU::dot( model, screenPlaneS );
	auto	t = GPU::dot( model, screenPlaneT );
	auto	q = GPU::dot( model, screenPlaneQ );

	// GL's texture matrix over the homogeneous coordinate, which is why these
	// two rows are dotted with (s, t, q, 0) where an explicit stage's are dotted
	// with (s, t, 1, 0): the third input is the generated q rather than the
	// constant 1. The matrix's own fourth row is the identity in everything
	// RB_GetShaderTextureMatrix can produce, so q comes through unchanged and
	// there is no third row to carry.
	auto	generated = GPU::float4( s, t, q, 0.0f );

	auto	projected = varying( GPU::float3( GPU::dot( generated, textureMatrixS ),
											  GPU::dot( generated, textureMatrixT ),
											  q ) );

	auto	fragment = GPU::sample( image, projected.xy() / projected.z() )
		* varying( vertexColor * colorModulate + colorAdd );

	setFragment( fragment );
}

/*
================================================================================

	idEacpHeatHazeProgram

================================================================================
*/

/*
====================
idEacpHeatHazeProgram::idEacpHeatHazeProgram

**The mask's sampling is baked in even where nothing samples a mask**, and that
is not an oversight. eacp declares every texture a program lists, whether
define() reaches it or not, and Metal's validation layer refuses a draw with a
declared texture left unbound - the same rule that made step 4e.5 write three
texgen programs rather than one. Splitting the heat haze on the same rule would
have made two programs out of what is plainly one shader with a switch, so the
third slot stays declared, the unmasked form binds its own normal map there, and
the key it is asked for under carries that normal map's sampling. What it costs
is nothing at all: the slot is bound to a texture that already had to be, and
the key space does not grow.
====================
*/
idEacpHeatHazeProgram::idEacpHeatHazeProgram( bool mask, bool vertexColor,
											  const sampling_t &sampling ) {
	// Before compile(), as everywhere here: the build walk reads each texture's
	// sampling to place the static sampler the generated source points at, and
	// it reads the two switches because define() branches on them.
	currentRenderImage.sampling = sampling.currentRender;
	normalImage.sampling = sampling.normal;
	maskImage.sampling = sampling.mask;

	masked = mask;
	vertexColored = vertexColor;

	compile();
}

/*
====================
idEacpHeatHazeProgram::define

heatHaze.vfp and its two masked forms, both halves of each.

The vertex half computes three things and one of them is interesting. The
scrolled coordinate and the raw one are what they look like; the third is the
deform magnitude scaled by the projected width of a unit at this vertex's depth,
which is what keeps the ripple a constant size on the screen as the surface
recedes. `(1, 0, z, 1)` with z the view-space depth, dotted with rows 0 and 3 of
the projection, is the numerator and the denominator of that width - and the
`max` under the divide is the original's guard for a polygon crossing the view
plane, where the denominator goes through zero.

The fragment half offsets the screen position by the normal map and reads
`_currentRender` there. Two details are the original's rather than this port's:
the normal map's x comes out of the alpha channel, because idImage::GenerateImage
swaps red and alpha on every normal map it uploads; and the offset is saturated
into [0, 1] *before* the power-of-two scale, so a distortion that would have read
off the edge of the frame reads the edge instead of the padding beside it.
====================
*/
void idEacpHeatHazeProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );
	auto	texcoord = vertexInput( &eacpDrawVert_t::st );

	// `MOV result.color, vertex.color`, and only heatHazeWithMaskAndVertex.vfp
	// has that line. Pulled behind the switch rather than always, because an
	// attribute pulled here is an attribute in this variant's vertex layout.
	std::optional<GPU::Float4>	vertexColor;

	if ( masked && vertexColored ) {
		vertexColor = varying( vertexInput( &eacpDrawVert_t::color ) );
	}

	auto	model = GPU::float4( position, 1.0f );
	auto	clip = modelViewProjection * model;

	setPosition( clip );

	// fragment.position, rebuilt. The clip position's (x, y, w) as a varying is
	// interpolated with w divided out, so dividing it back in the fragment
	// stage gives the normalized device coordinate at that fragment exactly -
	// which is the window coordinate the ARB program reads, over the viewport
	// size that program.env[1] divides it by. The half-and-shift is the
	// remaining difference between [-1, 1] and [0, 1].
	auto	screen = varying( GPU::float3( clip.x(), clip.y(), clip.w() ) );

	// result.texcoord[1]: the normal map's coordinate, scrolled.
	auto	scrolled = varying( texcoord + scroll.xy() );

	// result.texcoord[2]: the deform magnitude, scaled by depth. Written as the
	// original's dot products rather than folded into fewer uniforms, because
	// the two projection rows are (2n/w, 0, (r+l)/w, 0) and (0, 0, -1, 0) only
	// for the frustum R_SetupProjection happens to build.
	auto	viewZ = GPU::dot( model, modelViewRowZ );
	auto	depth = GPU::float4( 1.0f, 0.0f, viewZ, 1.0f );

	auto	across = GPU::dot( depth, projectionRowX );
	auto	forward = GPU::max( GPU::dot( depth, projectionRowW ), 1.0f );

	auto	deform = varying( deformMagnitude.xy()
							  * GPU::min( across / forward, 0.02f ) );

	// The distortion itself: x out of alpha, then out of [0, 1] and into
	// [-1, 1]. A mutable local because the mask multiplies into it, exactly as
	// the original's localNormal register is written twice.
	auto	normalTexel = GPU::sample( normalImage, scrolled );
	auto	localNormal = var( normalTexel.wy() * 2.0f - 1.0f );

	if ( masked ) {
		auto	maskTexel = GPU::sample( maskImage, varying( texcoord ) );

		// heatHazeWithMaskAndVertex.vfp's one extra line. The vertex colour is
		// what fades a particle out, so a mask times a faded colour drops below
		// the threshold and the fragment goes - which is how a heat plume
		// disappears rather than shrinking.
		auto	weighted = vertexColor.has_value()
			? maskTexel.xy() * vertexColor->xy()
			: maskTexel.xy();

		auto	mask = weighted - 0.01f;

		// KIL, which kills on any negative component - and only x and y were
		// written by the SUB above, the other two being the mask texel's own
		// and so never negative. So the smaller of the two is the whole test.
		setDiscardBelow( GPU::min( mask.x(), mask.y() ), 0.0f );

		localNormal = localNormal.get() * mask;
	}

	auto	screenCoord = screen.xy() / screen.z() * 0.5f + 0.5f;

	auto	offset = GPU::clamp( localNormal.get() * deform + screenCoord, 0.0f, 1.0f );

	setFragment( GammaCorrected( GPU::sample( currentRenderImage,
											  offset * currentRenderScale.xy() ) ) );
}

/*
================================================================================

	idEacpColorProcessProgram

================================================================================
*/

idEacpColorProcessProgram::idEacpColorProcessProgram( GPU::TextureSampling sampling ) {
	currentRenderImage.sampling = sampling;

	compile();
}

/*
====================
idEacpColorProcessProgram::define

colorProcess.vfp. The frame, desaturated, mixed towards a hue.

Its vertex program is four lines of which two are arithmetic on constants, and
those two are here in the fragment stage instead: `1 - fraction` and
`target * fraction` are the same value at every vertex, so interpolating them
would be paying for a varying to carry a uniform.

The screen coordinate is the heat haze's, and there is no distortion to add to
it and no saturate over it - the original has neither.
====================
*/
void idEacpColorProcessProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );

	auto	model = GPU::float4( position, 1.0f );
	auto	clip = modelViewProjection * model;

	setPosition( clip );

	auto	screen = varying( GPU::float3( clip.x(), clip.y(), clip.w() ) );

	auto	screenCoord = screen.xy() / screen.z() * 0.5f + 0.5f;

	auto	source = GPU::sample( currentRenderImage,
								  screenCoord * currentRenderScale.xy() );

	// The original's grey: the three channels added and scaled by 0.33. Not a
	// third, and not weighted for the eye - written out as it stands, like
	// every other constant in these programs.
	auto	grey = ( source.x() + source.y() + source.z() ) * 0.33f;

	auto	blended = source.xyz() * ( 1.0f - fraction.xyz() )
		+ targetColor.xyz() * fraction.xyz() * grey;

	// Alpha is the frame's own, for the reason the heat haze's is:
	// `MAD result.color.xyz` leaves w undefined and what this stage draws is
	// the pixel that was already there.
	setFragment( GammaCorrected( GPU::float4( blended, source.w() ) ) );
}

/*
================================================================================

	idEacpInteractionProgram

================================================================================
*/

idEacpInteractionProgram::idEacpInteractionProgram( const sampling_t &sampling ) {
	// Before compile(), for the same reason the stage program does it: the
	// build walk reads each texture's sampling to place the static sampler the
	// generated source points at.
	bumpImage.sampling = sampling.bump;
	lightFalloffImage.sampling = sampling.falloff;
	lightImage.sampling = sampling.projection;
	diffuseImage.sampling = sampling.diffuse;
	specularImage.sampling = sampling.specular;

	compile();
}

/*
====================
idEacpInteractionProgram::define

interaction.vfp, both halves of it, as one expression.

The vertex half is a change of coordinates: the vectors to the light and to the
eye, rotated into the surface's tangent space by the three basis vectors the
vertex carries, plus six texture coordinates that are each two dot products.
Nothing there is a matrix multiply, which is why the uniforms are rows rather
than matrices - the original is written in DP4s for the same reason.

The fragment half is the lighting: a bump-mapped N.L, modulated by the light's
projected image and its falloff, times a diffuse term and a specular one.
====================
*/
void idEacpInteractionProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );
	auto	texcoord = vertexInput( &eacpDrawVert_t::st );
	auto	normal = vertexInput( &eacpDrawVert_t::normal );
	auto	tangent = vertexInput( &eacpDrawVert_t::tangent );
	auto	bitangent = vertexInput( &eacpDrawVert_t::bitangent );
	auto	vertexColor = vertexInput( &eacpDrawVert_t::color );

	auto	model = GPU::float4( position, 1.0f );

	setPosition( modelViewProjection * model );

	// The two vectors the lighting is made of, in model space. localLightOrigin
	// carries w = 0 and localViewOrigin w = 1, which the ARB program relies on
	// and this does not: the subtraction is over xyz here and says so.
	auto	toLight = localLightOrigin.xyz() - position;
	auto	toView = localViewOrigin.xyz() - position;

	// Tangent space is (tangent, bitangent, normal), which is idDrawVert's
	// tangents[0], tangents[1] and normal - and *not* the order the comment at
	// the top of interaction.vfp gives for its vertex attributes. The comment
	// is stale; RB_ARB2_CreateDrawInteractions is where the attributes are
	// really bound, and this follows that.
	auto	inTangentSpace = [&]( const GPU::Float3 &v ) {
		return GPU::float3( GPU::dot( tangent, v ),
							GPU::dot( bitangent, v ),
							GPU::dot( normal, v ) );
	};

	// The half-angle vector, which is the sum of the two unit vectors and is
	// deliberately left unnormalized here: the original normalizes it in the
	// fragment shader, where the interpolated sum is what needs the length
	// taken out of it.
	auto	halfAngle = GPU::normalize( toLight ) + GPU::normalize( toView );

	// (s, t, 0, 1) - a two-component vertex attribute read by a DP4, which is
	// what fills in the z and the w the material's matrix rows are written
	// against.
	auto	surfaceCoord = GPU::float4( texcoord, 0.0f, 1.0f );

	auto	textureCoord = [&]( const GPU::Float4 &s, const GPU::Float4 &t ) {
		return GPU::float2( GPU::dot( surfaceCoord, s ), GPU::dot( surfaceCoord, t ) );
	};

	auto	tangentLight = varying( inTangentSpace( toLight ) );
	auto	tangentHalf = varying( inTangentSpace( halfAngle ) );

	auto	bumpCoord = varying( textureCoord( bumpMatrixS, bumpMatrixT ) );
	auto	diffuseCoord = varying( textureCoord( diffuseMatrixS, diffuseMatrixT ) );
	auto	specularCoord = varying( textureCoord( specularMatrixS, specularMatrixT ) );

	// The falloff is a one-dimensional ramp read down the middle of a 2D image,
	// which is where the 0.5 comes from: it is the t of interaction.vfp's
	// defaultTexCoord, the constant the vertex program fills every unset
	// coordinate with.
	auto	falloffCoord = GPU::float2( varying( GPU::dot( model, lightFalloffS ) ), 0.5f );

	// The projected image, still homogeneous - the divide is the fragment's,
	// because that is what a projective texture read is.
	auto	projected = varying( GPU::float3( GPU::dot( model, lightProjectionS ),
											  GPU::dot( model, lightProjectionT ),
											  GPU::dot( model, lightProjectionQ ) ) );

	auto	surfaceColor = varying( vertexColor * colorModulate + colorAdd );

	// The bump map, with x read out of the alpha channel. That is not a format
	// quirk this port is working around: idImage::GenerateImage swaps red and
	// alpha for every normal map it uploads, compressed or not, "so we only
	// have to use one fragment program" - so alpha is where x lives on every
	// bump map the engine has, including the generated flat one.
	auto	bumpTexel = GPU::sample( bumpImage, bumpCoord );
	auto	localNormal = bumpTexel.wyz() * 2.0f - 1.0f;

	// The direction to the light in tangent space, per fragment. The original
	// gets this by sampling a cube map whose texels are a normalize(), which is
	// the one thing an ARB fragment program cannot compute; the .vfp keeps the
	// arithmetic version beside it, commented out, and this is that version.
	//
	// An ambient light has no direction at all - the ARB path swaps that cube
	// map for one whose every texel is the same vector - so w selects a
	// constant instead. It is 0 or 1, which makes this a choice rather than a
	// blend.
	auto	lightVector = GPU::normalize( tangentLight ) * ( 1.0f - ambientLightVector.w() )
		+ ambientLightVector.xyz() * ambientLightVector.w();

	auto	lightAmount = GPU::dot( lightVector, localNormal );

	auto	projection = GPU::sample( lightImage, projected.xy() / projected.z() );
	auto	falloff = GPU::sample( lightFalloffImage, falloffCoord );

	auto	light = projection * falloff * lightAmount;

	auto	diffuse = GPU::sample( diffuseImage, diffuseCoord ) * diffuseColor;

	// The specular ramp, which is a texture in the original and two multiplies
	// here. R_SpecularTableImage tabulates max( 0, (d - 0.75) * 4 )^2 into a
	// 256x1 clamped image, and says in its own comment that it exists because
	// the fragment program "can't really do a power function" - so the curve is
	// the intent and the table is the workaround.
	//
	// The min is the clamp the sampler was doing: a table read past its end
	// repeats the last texel, and the curve does not stop climbing.
	auto	specularDot = GPU::dot( GPU::normalize( tangentHalf ), localNormal );
	auto	ramp = GPU::max( ( GPU::min( specularDot, 1.0f ) - 0.75f ) * 4.0f, 0.0f );

	auto	specularTexel = GPU::sample( specularImage, specularCoord );

	// Twice the specular map, which is the ADD in the original: the maps are
	// authored half-bright so that a highlight has somewhere to go.
	auto	specular = ramp * ramp * specularColor * ( specularTexel + specularTexel );

	setFragment( GammaCorrected( light * ( diffuse + specular ) * surfaceColor ) );
}

/*
================================================================================

	idEacpShadowProgram

================================================================================
*/

idEacpShadowProgram::idEacpShadowProgram() {
	compile();
}

/*
====================
idEacpShadowProgram::define

shadow.vp, whole:

	SUB	R0, vertex.position, program.env[4];
	MAD	R0, R0.wwww, program.env[4], R0;

	DP4	result.position.x, R0, state.matrix.mvp.row[0];
	... and the other three rows

The light's w is zero, which the first line relies on twice: it leaves the
vertex's own w alone through the subtraction, and it makes the second line add
the light back for a vertex whose w is 1 while leaving it subtracted for one
whose w is 0. That is the extrusion - the near copy of a vertex stays on the
surface, and the far copy becomes the direction away from the light with w = 0,
which is a point at infinity.

The DP4s are the fixed-function matrix stack, which is a uniform here.
====================
*/
void idEacpShadowProgram::define( void ) {
	auto	position = vertexInput( &eacpShadowVert_t::xyzw );

	auto	light = localLightOrigin.xyz();

	auto	extruded = GPU::float4( position.xyz() - light + light * position.w(),
									position.w() );

	setPosition( modelViewProjection * extruded );

	// Never read. Every pipeline this program is drawn through masks off all
	// four colour channels - a shadow volume exists to be rasterized so that
	// the stencil ops fire, and the original has no fragment program at all.
	// Something has to be written here because a fragment stage without an
	// output is not a shader, so this is the smallest thing that is one.
	setFragment( GPU::float4( constant( 0.0f ), 0.0f, 0.0f, 0.0f ) );
}

/*
================================================================================

	idEacpFogProgram

================================================================================
*/

idEacpFogProgram::idEacpFogProgram() {
	// Named rather than read off the images, because there is nothing to read
	// off: R_FogImage and R_FogEnterImage generate both at TF_LINEAR with
	// TR_CLAMP and no cvar or material can say otherwise, so this program has
	// one sampling tuple by construction rather than by cache. The linear
	// filter is load-bearing on the second of them - "picky to get the bilerp
	// correct at terminator", says the comment over FOG_ENTER in tr_local.h,
	// and the half-texel FOG_ENTER carries is what that pickiness amounts to.
	fogImage.sampling.filter = GPU::TextureFilter::Linear;
	fogImage.sampling.addressMode = GPU::TextureAddressMode::Clamp;

	fogEnterImage.sampling.filter = GPU::TextureFilter::Linear;
	fogEnterImage.sampling.addressMode = GPU::TextureAddressMode::Clamp;

	compile();
}

/*
====================
idEacpFogProgram::define

RB_T_BasicFog's two texture units, and the default modulate combiner between
them.

Every coordinate is a plane dotted with the vertex in its own model space, which
is what GL_OBJECT_LINEAR means and what RB_T_BasicFog re-sends whenever the
space changes. Three of the four are computed that way here; the fourth is the
literal 0.5 below.
====================
*/
void idEacpFogProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );

	auto	model = GPU::float4( position, 1.0f );

	setPosition( modelViewProjection * model );

	// **The t of the _fog lookup is a constant, and the original says so by
	// overwriting its own texgen.** RB_T_BasicFog computes fogPlanes[1] beside
	// fogPlanes[0] - the view's *right* axis, which would have made this a
	// genuine two-dimensional distance - and then sets the plane it hands
	// GL_T to ( 0, 0, 0, 0.5 ) on the line after, with the two lines that would
	// have used the plane commented out above it. So the second axis was tried
	// and abandoned, the middle row of the image is the whole of what is
	// sampled, and 0.5 here is the honest translation of that.
	auto	density = GPU::float2( varying( GPU::dot( model, fogPlane ) ), 0.5f );

	auto	enter = varying( GPU::float2( GPU::dot( model, fogEnterPlaneS ),
										  GPU::dot( model, fogEnterPlaneT ) ) );

	// The two units multiplied together and by the colour, which is what the
	// default GL_MODULATE texture environment computes and what the fog pass
	// never overrides. Both images carry 255 in all three colour channels, so
	// what the product really is is the fog's colour at the product of the two
	// alphas - but writing the multiply out is what the combiner does, and the
	// blend that follows reads the alpha this leaves.
	setFragment( GPU::sample( fogImage, density )
				 * GPU::sample( fogEnterImage, enter )
				 * fogColor );
}

/*
================================================================================

	idEacpBlendLightProgram

================================================================================
*/

idEacpBlendLightProgram::idEacpBlendLightProgram( const sampling_t &sampling ) {
	// Before compile(), as everywhere else here: the build walk reads each
	// texture's sampling to place the static sampler the generated source
	// points at.
	lightImage.sampling = sampling.projection;
	lightFalloffImage.sampling = sampling.falloff;

	compile();
}

/*
====================
idEacpBlendLightProgram::define

RB_T_BlendLight's two texture units, modulated together and by the light stage's
colour - the same combiner the fog above uses and the same object-linear texgens
feeding it, with the projection's q enabled so that the read is projective.
====================
*/
void idEacpBlendLightProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );

	auto	model = GPU::float4( position, 1.0f );

	setPosition( modelViewProjection * model );

	// Homogeneous, and divided in the fragment stage - which is what a
	// projective texture read is, and what interaction.vfp does with the same
	// three planes.
	auto	projected = varying( GPU::float3( GPU::dot( model, lightProjectionS ),
											  GPU::dot( model, lightProjectionT ),
											  GPU::dot( model, lightProjectionQ ) ) );

	// **The falloff's t is 0.5 here and 0 in the OpenGL build, and the two are
	// the same number on every falloff image the game has.** RB_BlendLight
	// means to set it: it selects texture unit 1 and calls
	// qglTexCoord2f( 0, 0.5 ). But glTexCoord addresses unit 0 whatever
	// glActiveTexture last said - glMultiTexCoord is the call that takes a unit
	// and dhewm3 never makes it - so unit 1 keeps its default current
	// coordinate and the falloff is really read at t = 0.
	//
	// It makes no difference to anything drawn. Every falloff image a blend
	// light can name is a ramp along s alone, constant down t:
	// lights/squarelight1a and lights/xfalloff are 64x8 with all eight rows
	// identical, and shapes/pitFalloff is the same shape. So 0.5 is written
	// here, which is what the code plainly intends, what the interaction
	// program already reads its own falloff at, and what survives a
	// content-driven filter change that t = 0 would not.
	//
	// The one image where the two would disagree is the *generated* one:
	// _noFalloff has a black border row at t = 0, so a blend light falling back
	// to it would draw nothing at all on OpenGL. No blendLight material in the
	// demo falls back - all six declare a lightFalloffImage - so this is a
	// difference that exists on paper and nowhere in the frames.
	auto	falloff = GPU::float2( varying( GPU::dot( model, lightFalloffS ) ), 0.5f );

	setFragment( GPU::sample( lightImage, projected.xy() / projected.z() )
				 * GPU::sample( lightFalloffImage, falloff )
				 * color );
}

/*
================================================================================

	idEacpBlitProgram

================================================================================
*/

idEacpBlitProgram::idEacpBlitProgram() {
	// Linear rather than nearest, and it costs nothing at the size the frame is
	// normally drawn: the quad covers the drawable exactly, so every fragment
	// centre lands on a texel centre and the filter has nothing to interpolate.
	// What it buys is the case where the two disagree - a window resized under
	// a render target sized to glConfig - where nearest would alias hard.
	image.sampling.filter = GPU::TextureFilter::Linear;
	image.sampling.addressMode = GPU::TextureAddressMode::Clamp;

	compile();
}

void idEacpBlitProgram::define( void ) {
	auto	position = vertexInput( &eacpBlitVert_t::xy );
	auto	texcoord = vertexInput( &eacpBlitVert_t::st );

	// Already clip space, so there is no matrix. z is zero and w is one, which
	// is inside the frustum on both APIs - the pipeline tests no depth anyway.
	setPosition( GPU::float4( position, 0.0f, 1.0f ) );

	// **Opaque, and the alpha is not a detail.** OpenGL copies the frame into an
	// image as GL_RGB8, so what a material samples out of _scratch or
	// _currentRender has no alpha channel and reads 1 - and the materials that
	// draw them are `blend blend`, so they are drawn straight over what is under
	// them. eacp has no three-channel format, so the four-channel one has to say
	// the same thing: sampled alpha here is whatever the frame's own stages
	// happened to leave in that channel, and carrying it through makes the
	// berserk and double-vision overlays translucent - the frame blended with
	// itself, which reads as a picture that is simply too bright.
	setFragment( GPU::float4( GPU::sample( image, varying( texcoord ) ).xyz(), 1.0f ) );
}

/*
================================================================================

	idEacpDepthCopyProgram

================================================================================
*/

idEacpDepthCopyProgram::idEacpDepthCopyProgram() {
	// Nearest, and it is the only honest filter for this: a depth value is not a
	// colour and the average of two of them is a surface that is at neither
	// depth. The copy is 1:1 anyway - the destination is the frame target's own
	// size - so there is nothing between texels to interpolate.
	sceneDepth.sampling.filter = GPU::TextureFilter::Nearest;
	sceneDepth.sampling.addressMode = GPU::TextureAddressMode::Clamp;

	compile();
}

void idEacpDepthCopyProgram::define( void ) {
	auto	position = vertexInput( &eacpBlitVert_t::xy );
	auto	texcoord = vertexInput( &eacpBlitVert_t::st );

	setPosition( GPU::float4( position, 0.0f, 1.0f ) );

	// One channel in, one channel out. The destination is R32Float, so only the
	// first of the four is stored; the other three are written for the same
	// reason the blit writes an opaque alpha, which is that a fragment has to
	// produce a colour whatever the attachment keeps of it.
	auto	depth = GPU::sample( sceneDepth, varying( texcoord ) );

	setFragment( GPU::float4( depth, depth, depth, 1.0f ) );
}

/*
================================================================================

	idEacpSoftParticleProgram

================================================================================
*/

idEacpSoftParticleProgram::idEacpSoftParticleProgram( GPU::TextureSampling sampling ) {
	image.sampling = sampling;

	// _currentDepth's, which is not content: R_DepthImage creates the image
	// TF_NEAREST / TR_CLAMP and CopyDepthbufferToImage writes those two fields
	// back on every capture, so this is a constant rather than a key.
	depthImage.sampling.filter = GPU::TextureFilter::Nearest;
	depthImage.sampling.addressMode = GPU::TextureAddressMode::Clamp;

	compile();
}

/*
====================
idEacpSoftParticleProgram::define

soft_particle.vfp, both halves, line for line.

The vertex half is three lines of the original: the position through the
matrix (`OPTION ARB_position_invariant`), the texture coordinate through the
stage's matrix, and the vertex colour across. The fourth thing it sends is the
clip position itself, which the ARB program did not need because
`fragment.position` was given to it.

The fragment half recovers two depths in Doom units and compares them. Worth
knowing about the two constants: they are `{ 1/3, -0.33316667 }`, and what they
undo is R_SetupProjection's matrix at `r_znear` 3 - a window depth d comes back
as 3 / (d - 0.9995), which is negative and grows away from the eye. The 0.9994
clamp above it is the original's guard for caulk sky, which writes no depth at
all and leaves the far plane's 1 in the buffer; without it the reciprocal of
zero would put the sky at infinity and fade nothing.
====================
*/
void idEacpSoftParticleProgram::define( void ) {
	auto	position = vertexInput( &eacpDrawVert_t::xyz );
	auto	texcoord = vertexInput( &eacpDrawVert_t::st );
	auto	vertexColor = vertexInput( &eacpDrawVert_t::color );

	auto	clip = modelViewProjection * GPU::float4( position, 1.0f );

	setPosition( clip );

	// `DP4 result.texcoord.x, vertex.texcoord, program.env[12]` and its T half,
	// with vertex.texcoord being (s, t, 0, 1) - the same two rows the generic
	// stage program dots, against the same coordinate.
	auto	coordinate = GPU::float4( texcoord, 1.0f, 0.0f );

	auto	st = varying( GPU::float2( GPU::dot( coordinate, textureMatrixS ),
									   GPU::dot( coordinate, textureMatrixT ) ) );

	// `MOV result.color, vertex.color`, through the pair that says whether the
	// stage wanted the array or the constant colour.
	auto	color = varying( vertexColor * colorModulate + colorAdd );

	// fragment.position, rebuilt - see idEacpHeatHazeProgram::define, which
	// needs the same number for the same reason. z as well as x and y here,
	// because this program compares the fragment's own depth against the
	// buffer's rather than only reading the buffer.
	auto	screen = varying( GPU::float4( clip.x(), clip.y(),
										   clip.z(), clip.w() ) );

	auto	ndc = screen.xyz() / screen.w();

	// `MUL tmp.xy, fragment.position, program.env[22]`, with the viewport folded
	// in - the class comment says why that is more than a translation.
	auto	depthCoordinate = ndc.xy() * depthScaleBias.xy() + depthScaleBias.zw();

	// `TEX scene_depth, tmp, texture[1], 2D` then `MIN scene_depth, 0.9994`.
	auto	sceneWindow = GPU::min( GPU::sample( depthImage, depthCoordinate ).x(),
									0.9994f );

	// `MAD tmp, scene_depth, depth_consts.x, depth_consts.y; RCP scene_depth`,
	// twice: once for what the depth buffer holds and once for this fragment.
	auto	sceneDepth = 1.0f / ( sceneWindow * 0.33333333f - 0.33316667f );
	auto	particleDepth = 1.0f / ( ndc.z() * 0.33333333f - 0.33316667f );

	// `ADD tmp, -scene_depth, particle_depth; ADD tmp, tmp, radius;
	//  MUL_SAT fade, tmp, 1/fadeRange`. Both depths are negative and grow away
	// from the eye, so a particle in front of the surface behind it gives a
	// positive difference and the radius is what moves the zero crossing out to
	// where the particle's own volume starts intersecting.
	auto	fade = GPU::clamp( ( particleDepth - sceneDepth + particleRadius.x() )
							   * particleRadius.y(),
							   0.0f, 1.0f );

	// `MUL_SAT near_fade, particle_depth, -particle_radius.z`: the same fade at
	// the other end, so a particle does not pop as it passes through the eye.
	auto	nearFade = GPU::clamp( particleDepth * -particleRadius.z(), 0.0f, 1.0f );

	// `MUL fade, near_fade, fade; ADD_SAT fade, fade, program.env[24]` - the
	// scalar broadcast to four channels and then saturated back up on the ones
	// this blend mode does not fade.
	auto	scaled = fade * nearFade;

	auto	channels = GPU::clamp( GPU::float4( scaled, scaled, scaled, scaled )
								   + colorChannelMask,
								   0.0f, 1.0f );

	// `TEX oColor; MUL oColor, oColor, fade; MUL result.color, oColor,
	//  fragment.color`.
	setFragment( GPU::sample( image, st ) * channels * color );
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
R_EacpStencilFaces

The two facings, from the one name for what a draw is doing to the count.

Every line here is a qglStencilOpSeparate or a qglStencilFunc in
draw_common.cpp's RB_T_Shadow and RB_StencilShadowPass, and the translation is
literal on both counts:

  - **The faces line up directly.** eacp's front face is the counter-clockwise
    winding in clip space, which is also OpenGL's default, so GL_FRONT is
    stencilFront and GL_BACK is stencilBack. That the increments look inverted
    against a textbook depth-fail volume is Doom 3's winding, not a translation
    error: its CT_FRONT_SIDED culls GL_FRONT for the same reason.

  - **The comparison takes the reference on the left.** GL_GEQUAL passes when
    the reference is at least the buffer's value, and Metal and D3D12 order it
    the same way - so ES_LIT keeps a fragment whose count came back down to 128
    or below, which is exactly the fragments no volume closed over.

The wrapping increments are what the buffer being unsigned and 8 bits deep
asks for: a pixel inside more volumes than the buffer can count still comes back
to the right number, because an overflow and the underflow that answers it
cancel. GL only has them as an extension (EXT_stencil_wrap, checked for in
R_CheckPortableExtensions); both of eacp's backends have them outright.
====================
*/
static void R_EacpStencilFaces( eacpStencil_t stencil,
								GPU::StencilFace &front, GPU::StencilFace &back ) {
	// The defaults are always-test and never-write, which is glDisable(
	// GL_STENCIL_TEST ) - so ES_IGNORE is the two faces left alone.
	front = GPU::StencilFace();
	back = GPU::StencilFace();

	switch ( stencil ) {
	case ES_CLEAR:
		front.pass = GPU::StencilOp::Replace;
		back.pass = GPU::StencilOp::Replace;
		break;

	case ES_COUNT_DEPTH_FAIL:
		back.depthFail = GPU::StencilOp::DecrementWrap;
		front.depthFail = GPU::StencilOp::IncrementWrap;
		break;

	case ES_COUNT_DEPTH_FAIL_MIRRORED:
		front.depthFail = GPU::StencilOp::DecrementWrap;
		back.depthFail = GPU::StencilOp::IncrementWrap;
		break;

	case ES_COUNT_DEPTH_PASS:
		back.pass = GPU::StencilOp::IncrementWrap;
		front.pass = GPU::StencilOp::DecrementWrap;
		break;

	case ES_COUNT_DEPTH_PASS_MIRRORED:
		front.pass = GPU::StencilOp::IncrementWrap;
		back.pass = GPU::StencilOp::DecrementWrap;
		break;

	case ES_LIT:
		front.compare = GPU::CompareFunction::GreaterEqual;
		back.compare = GPU::CompareFunction::GreaterEqual;
		break;

	default:
		break;
	}
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

	// A cube ignores whatever the material asked for, which is not this port
	// being lazy - it is idRenderBackendGL::SetCubeImageFilterAndRepeat, whose
	// own comment is "no other clamp mode makes sense". It forces
	// GL_CLAMP_TO_EDGE on S and T however the image was declared, so a cube
	// declared `repeat` samples clamped there and has to sample clamped here.
	// What it means on this backend is one fewer variant rather than one fewer
	// call: the address mode is compiled into the shader, so forcing it is
	// choosing which compiled program the draw goes through.
	if ( image->type == TT_CUBIC ) {
		sampling.addressMode = GPU::TextureAddressMode::Clamp;
	}

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
	// Nothing, and Shutdown below is what makes that safe rather than lucky.
	//
	// This object is a static, so this runs at exit, after the device has gone -
	// and handing a pipeline back to a device that is not there is worse than
	// leaking one. Every member holds what it owns, so what keeps that from
	// happening is not the absence of a destructor here: it is that Shutdown has
	// already emptied them while there was still a device to release them to,
	// and an empty container has nothing left to free.
}

/*
====================
idEacpRenderProgs::Shutdown

Everything the device owns, released in the one window where releasing it is
safe: the backend's Shutdown, which runs before GLimp takes the window away.

Emptying rather than deleting. Each container owns its contents - a
statePipeline_t holds an OwningPointer and the interaction variants are an
OwnedVector - so clearing one *is* the release, and there is no order to get
wrong.
====================
*/
void idEacpRenderProgs::Shutdown( void ) {
	for ( int i = 0 ; i < GPU::samplingConfigurations ; i++ ) {
		for ( int test = 0 ; test < 2 ; test++ ) {
			programVariant_t &	variant = variants[i][test];

			variant.pipelines.clear();
			variant.library.reset();
			variant.program.reset();
		}
	}

	interactions.clear();

	cubes.clear();
	bumpyReflects.clear();
	screens.clear();
	heatHazes.clear();
	colorProcesses.clear();
	softParticles.clear();

	depthCopyPipeline.reset();
	depthCopyLibrary.reset();
	depthCopyProgram.reset();

	shadowPipelines.clear();
	shadowLibrary.reset();
	shadowProgram.reset();

	fogPipelines.clear();
	fogLibrary.reset();
	fogProgram.reset();

	blendLights.clear();

	capturePipeline.reset();
	blitPipeline.reset();
	blitLibrary.reset();
	blitProgram.reset();
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

	for ( int i = 0 ; i < interactions.size() ; i++ ) {
		if ( interactions[i]->program.has_value() ) {
			total++;
		}
	}

	total += CountPrograms( cubes );
	total += CountPrograms( bumpyReflects );
	total += CountPrograms( screens );
	total += CountPrograms( heatHazes );
	total += CountPrograms( colorProcesses );
	total += CountPrograms( softParticles );

	if ( depthCopyProgram.has_value() ) {
		total++;
	}

	if ( shadowProgram.has_value() ) {
		total++;
	}

	if ( fogProgram.has_value() ) {
		total++;
	}

	for ( int i = 0 ; i < blendLights.size() ; i++ ) {
		if ( blendLights[i]->program.has_value() ) {
			total++;
		}
	}

	if ( blitProgram.has_value() ) {
		total++;
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

	for ( int i = 0 ; i < interactions.size() ; i++ ) {
		total += interactions[i]->pipelines.size();
	}

	total += CountPipelines( cubes );
	total += CountPipelines( bumpyReflects );
	total += CountPipelines( screens );
	total += CountPipelines( heatHazes );
	total += CountPipelines( colorProcesses );
	total += CountPipelines( softParticles );

	if ( depthCopyPipeline ) {
		total++;
	}

	total += shadowPipelines.size();
	total += fogPipelines.size();

	for ( int i = 0 ; i < blendLights.size() ; i++ ) {
		total += blendLights[i]->pipelines.size();
	}

	if ( blitPipeline ) {
		total++;
	}

	if ( capturePipeline ) {
		total++;
	}

	return total;
}

/*
====================
idEacpRenderProgs::BuildPipeline

Doom 3's state, compiled. All three programs come through here because the state
is the material's rather than the program's: an interaction is (ONE, ONE) with
the depth write off, a shadow volume writes no channel at all, and the generic
stage is whatever the .mtr asked for - but what any of them *is* to the API is
the same object built the same way.

The three things a pipeline needs that are not the GLS_* bits - the source it
runs, the vertices it reads and what it does to the stencil - are the arguments,
because they are the only part that differs between the callers.
====================
*/
eacp::OwningPointer<GPU::RenderPipeline>
idEacpRenderProgs::BuildPipeline( const GPU::ShaderLibrary &library,
								  const GPU::VertexLayout &layout,
								  int stateBits, int cullType,
								  eacpStencil_t stencil ) {
	eacp::GPU::GPUView *	view = R_EacpGetView();

	if ( !view ) {
		return {};
	}

	GPU::RenderPipelineDescriptor	descriptor;

	descriptor.library = &library;
	descriptor.vertexLayout = layout;
	descriptor.topology = GPU::PrimitiveTopology::Triangles;

	// The render target's, which is the same colour format the drawable has -
	// deliberately, so that these pipelines could draw into either - and
	// single-sampled, because a texture target on eacp has nothing to resolve
	// into and no multisampling to offer. Both backends reject a draw whose
	// pipeline disagrees with the pass it is issued into, so this is not a
	// preference.
	descriptor.colorFormat = GPU::PixelFormat::BGRA8Unorm;
	descriptor.sampleCount = 1;

	descriptor.blend = R_EacpBlendState( stateBits );
	descriptor.colorWriteMask = R_EacpColorWriteMask( stateBits );

	// The attachment carries both planes, so every pipeline drawing into it has
	// to say so - including one that only wants the depth test, and including
	// one that wants neither. What decides whether the test runs is the
	// comparison: a 2D view is Always with no write, which is what
	// RB_BeginDrawingView's glDisable( GL_DEPTH_TEST ) means.
	descriptor.depth = true;
	descriptor.depthCompare = R_EacpDepthCompare( stateBits );
	descriptor.depthWrite = !( stateBits & GLS_DEPTHMASK );

	descriptor.stencil = true;
	R_EacpStencilFaces( stencil, descriptor.stencilFront, descriptor.stencilBack );

	descriptor.cullMode = R_EacpCullMode( cullType );

	eacp::OwningPointer<GPU::RenderPipeline>	pipeline =
		eacp::makeOwned<GPU::RenderPipeline>( GPU::Device::shared(), descriptor );

	if ( !pipeline->isValid() ) {
		common->Warning( "eacp: no pipeline for state 0x%x, cull %i, stencil %i",
						 stateBits, cullType, stencil );
		return {};
	}

	return pipeline;
}

/*
====================
idEacpRenderProgs::PipelineFor

The lookup all three caches share: what a program has already been compiled to
draw in, searched linearly and extended when the state asked for is new.

Linear because the list is short and stays short - the whole of the demo's first
level is 32 pipelines over every program in it - and because what is being
compared is three integers, which is cheaper than anything that would index them.
====================
*/
const GPU::RenderPipeline *
idEacpRenderProgs::PipelineFor( eacp::Vector<statePipeline_t> &pipelines,
								const GPU::ShaderLibrary &library,
								const GPU::VertexLayout &layout,
								int stateBits, int cullType, eacpStencil_t stencil ) {
	// The bits a pipeline is compiled against. Two of GL_State's are not here:
	// GLS_POLYMODE_LINE, which is r_showTris' wireframe and has no eacp
	// counterpart, and GLS_ATEST_BITS, which has zero call sites in this tree -
	// the alpha test Doom 3 actually uses is shaderStage_t::hasAlphaTest, and it
	// reaches StageDraw as its own argument rather than in the bitfield.
	const int	pipelineBits = stateBits
		& ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS
			| GLS_DEPTHMASK | GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK | GLS_ALPHAMASK
			| GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHFUNC_EQUAL );

	for ( int i = 0 ; i < pipelines.size() ; i++ ) {
		if ( pipelines[i].stateBits == pipelineBits
			 && pipelines[i].cullType == cullType
			 && pipelines[i].stencil == stencil ) {
			return pipelines[i].pipeline;
		}
	}

	statePipeline_t	entry;

	entry.stateBits = pipelineBits;
	entry.cullType = cullType;
	entry.stencil = stencil;
	entry.pipeline = BuildPipeline( library, layout, pipelineBits, cullType, stencil );

	// Moved in, not copied: the pipeline has one owner and this is it changing
	// hands. Reading the answer back out of the list afterwards rather than off
	// `entry`, which no longer holds anything.
	return pipelines.add( std::move( entry ) ).pipeline;
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

	// Everything drawn through this program is drawn outside the shadow half of
	// a view - the depth fill, the ambient passes, the whole of the 2D - and
	// none of it reads or writes the stencil. That is Doom 3's own arrangement:
	// RB_ARB2_DrawInteractions leaves the buffer on GL_ALWAYS as it finishes.
	draw.pipeline = PipelineFor( variant.pipelines, *variant.library,
								 variant.program->vertexLayout(),
								 stateBits, cullType, ES_IGNORE );

	return draw;
}

/*
====================
idEacpRenderProgs::CubeDraw

A sky, a diffuse cube or an unbumped reflection. One program per (texgen,
sampling) pair, of which the demo's first level reaches one - the sky, sampled
linear and clamped.

The state is the material's exactly as the explicit stage's is, and the stencil
is ES_IGNORE for the reason StageDraw gives: everything drawn through the ambient
pass is drawn outside the shadow half of a view.
====================
*/
idEacpRenderProgs::cubeDraw_t idEacpRenderProgs::CubeDraw( eacpCubeTexgen_t texgen,
														   const idImage *cube,
														   int stateBits, int cullType ) {
	cubeDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	const GPU::TextureSampling	sampling = R_EacpSampling( cube );
	const int					key = (int)texgen | ( GPU::samplingIndex( sampling ) << 2 );

	texgenVariant_t<idEacpCubeProgram> *	variant =
		TexgenVariant( cubes, key, "cube texgen",
					   [&]( std::optional<idEacpCubeProgram> &program ) {
						   program.emplace( texgen, sampling );
					   } );

	if ( !variant ) {
		return draw;
	}

	draw.program = &*variant->program;

	draw.pipeline = PipelineFor( variant->pipelines, *variant->library,
								 variant->program->vertexLayout(),
								 stateBits, cullType, ES_IGNORE );

	return draw;
}

/*
====================
idEacpRenderProgs::BumpyReflectDraw

The two-textured reflection, keyed on both samplings - the cube's and the bump
map's - because both are baked into the source and either can differ without the
other.
====================
*/
idEacpRenderProgs::bumpyReflectDraw_t
idEacpRenderProgs::BumpyReflectDraw( const idImage *cube, const idImage *bump,
									 int stateBits, int cullType ) {
	bumpyReflectDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	const GPU::TextureSampling	cubeSampling = R_EacpSampling( cube );
	const GPU::TextureSampling	bumpSampling = R_EacpSampling( bump );

	const int	key = GPU::samplingIndex( cubeSampling )
		| ( GPU::samplingIndex( bumpSampling ) << 2 );

	texgenVariant_t<idEacpBumpyReflectProgram> *	variant =
		TexgenVariant( bumpyReflects, key, "bumpy reflection",
					   [&]( std::optional<idEacpBumpyReflectProgram> &program ) {
						   program.emplace( cubeSampling, bumpSampling );
					   } );

	if ( !variant ) {
		return draw;
	}

	draw.program = &*variant->program;

	draw.pipeline = PipelineFor( variant->pipelines, *variant->library,
								 variant->program->vertexLayout(),
								 stateBits, cullType, ES_IGNORE );

	return draw;
}

/*
====================
idEacpRenderProgs::ScreenDraw

TG_SCREEN and TG_SCREEN2, whose one texture is an ordinary 2D one - so the key is
the one sampling and the space is four.
====================
*/
idEacpRenderProgs::screenDraw_t idEacpRenderProgs::ScreenDraw( const idImage *image,
															   int stateBits,
															   int cullType ) {
	screenDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	const GPU::TextureSampling	sampling = R_EacpSampling( image );

	texgenVariant_t<idEacpScreenProgram> *	variant =
		TexgenVariant( screens, GPU::samplingIndex( sampling ), "screen texgen",
					   [&]( std::optional<idEacpScreenProgram> &program ) {
						   program.emplace( sampling );
					   } );

	if ( !variant ) {
		return draw;
	}

	draw.program = &*variant->program;

	draw.pipeline = PipelineFor( variant->pipelines, *variant->library,
								 variant->program->vertexLayout(),
								 stateBits, cullType, ES_IGNORE );

	return draw;
}

/*
====================
idEacpRenderProgs::HeatHazeDraw

The three heatHaze programs, keyed on the two switches that tell them apart and
on all three samplings - the third of which is the normal map's again where
there is no mask, the program declaring a texture it does not read. Two bits a
sampling and one each for the switches is a key space of 256; the demo's first
level reaches two entries, `heatHaze.vfp` on the glass and
`heatHazeWithMask.vfp` on the vent plumes, both with `_currentRender` linear and
clamped and their normal maps linear and repeated.
====================
*/
idEacpRenderProgs::heatHazeDraw_t
idEacpRenderProgs::HeatHazeDraw( const idImage *currentRender, const idImage *normal,
								 const idImage *mask, bool vertexColor,
								 int stateBits, int cullType ) {
	heatHazeDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	idEacpHeatHazeProgram::sampling_t	sampling;

	sampling.currentRender = R_EacpSampling( currentRender );
	sampling.normal = R_EacpSampling( normal );
	sampling.mask = R_EacpSampling( mask ? mask : normal );

	const bool	masked = ( mask != NULL );
	const bool	colored = masked && vertexColor;

	const int	key = (int)masked
		| ( (int)colored << 1 )
		| ( GPU::samplingIndex( sampling.currentRender ) << 2 )
		| ( GPU::samplingIndex( sampling.normal ) << 4 )
		| ( GPU::samplingIndex( sampling.mask ) << 6 );

	texgenVariant_t<idEacpHeatHazeProgram> *	variant =
		TexgenVariant( heatHazes, key, "heat haze",
					   [&]( std::optional<idEacpHeatHazeProgram> &program ) {
						   program.emplace( masked, colored, sampling );
					   } );

	if ( !variant ) {
		return draw;
	}

	draw.program = &*variant->program;

	draw.pipeline = PipelineFor( variant->pipelines, *variant->library,
								 variant->program->vertexLayout(),
								 stateBits, cullType, ES_IGNORE );

	return draw;
}

/*
====================
idEacpRenderProgs::ColorProcessDraw

colorProcess.vfp, whose one texture is always `_currentRender` - so the key is
the one sampling and the space is four, of which the content reaches the linear
clamped one and no other.
====================
*/
idEacpRenderProgs::colorProcessDraw_t
idEacpRenderProgs::ColorProcessDraw( const idImage *currentRender,
									 int stateBits, int cullType ) {
	colorProcessDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	const GPU::TextureSampling	sampling = R_EacpSampling( currentRender );

	texgenVariant_t<idEacpColorProcessProgram> *	variant =
		TexgenVariant( colorProcesses, GPU::samplingIndex( sampling ), "colour process",
					   [&]( std::optional<idEacpColorProcessProgram> &program ) {
						   program.emplace( sampling );
					   } );

	if ( !variant ) {
		return draw;
	}

	draw.program = &*variant->program;

	draw.pipeline = PipelineFor( variant->pipelines, *variant->library,
								 variant->program->vertexLayout(),
								 stateBits, cullType, ES_IGNORE );

	return draw;
}

/*
====================
idEacpRenderProgs::InteractionDraw

The same two lookups for one light against one surface, keyed on all five of the
program's textures at once.

Five two-bit sampling indices is a key space of 1024, which is why this is a
list searched linearly rather than the array the stage program's cache is: a
level reaches a handful of the thousand, and the search is a comparison of one
int against a vector that never grows past what the content contains.

That the *light's* two images count is not an oversight of plan.md section 4.3,
which sized the interaction program at eight on its three material-controlled
maps - it is the part that section did not look at. A light's projected image
and its falloff are declared by the light material, so a light that repeats
where its neighbour clamps is a second program, and a projection sampled with
the wrong address mode tiles a light across a level rather than dimming it.
====================
*/
idEacpRenderProgs::interactionDraw_t
idEacpRenderProgs::InteractionDraw( const idImage *bump, const idImage *falloff,
									const idImage *projection, const idImage *diffuse,
									const idImage *specular,
									int stateBits, int cullType, eacpStencil_t stencil ) {
	interactionDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	idEacpInteractionProgram::sampling_t	sampling;

	sampling.bump = R_EacpSampling( bump );
	sampling.falloff = R_EacpSampling( falloff );
	sampling.projection = R_EacpSampling( projection );
	sampling.diffuse = R_EacpSampling( diffuse );
	sampling.specular = R_EacpSampling( specular );

	const int	key = GPU::samplingIndex( sampling.bump )
		| ( GPU::samplingIndex( sampling.falloff ) << 2 )
		| ( GPU::samplingIndex( sampling.projection ) << 4 )
		| ( GPU::samplingIndex( sampling.diffuse ) << 6 )
		| ( GPU::samplingIndex( sampling.specular ) << 8 );

	interactionVariant_t *	variant = NULL;

	for ( int i = 0 ; i < interactions.size() ; i++ ) {
		if ( interactions[i]->key == key ) {
			variant = interactions[i];
			break;
		}
	}

	if ( !variant ) {
		// Made by the list rather than handed to it, which is what an
		// OwnedVector offers instead of a new: the element is constructed in
		// place and owned from the moment it exists.
		variant = &interactions.createNew();

		variant->key = key;
		variant->program.emplace( sampling );
		variant->library.emplace( GPU::Device::shared(), variant->program->source() );

		if ( !variant->library->isValid() ) {
			common->Warning( "eacp: the interaction shader failed to compile" );
			variant->program.reset();
			return draw;
		}
	}

	if ( !variant->program.has_value() ) {
		return draw;
	}

	draw.program = &*variant->program;

	// Only two states ever reach here - GLS_DEPTHFUNC_EQUAL over what the depth
	// fill wrote, and GLS_DEPTHFUNC_LESS for the translucent surfaces that were
	// never in it - and only two stencils: the mask, under a light whose shadow
	// volumes have just been counted, and nothing at all under one with no
	// shadow-casting surface in view.
	draw.pipeline = PipelineFor( variant->pipelines, *variant->library,
								 variant->program->vertexLayout(),
								 stateBits, cullType, stencil );

	return draw;
}

/*
====================
idEacpRenderProgs::ShadowDraw

One program, compiled the first time a shadow is drawn, and a pipeline per way
the stencil is counted.

There is no sampling dimension because there is no texture, and no alpha test
because there is nothing to test - so where the other two caches are a search
for the right *program* and then for the right pipeline, this is only the
second half.
====================
*/
idEacpRenderProgs::shadowDraw_t idEacpRenderProgs::ShadowDraw( int stateBits, int cullType,
															   eacpStencil_t stencil ) {
	shadowDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	if ( !shadowProgram.has_value() ) {
		shadowProgram.emplace();
		shadowLibrary.emplace( GPU::Device::shared(), shadowProgram->source() );

		if ( !shadowLibrary->isValid() ) {
			common->Warning( "eacp: the shadow volume shader failed to compile" );
			shadowProgram.reset();
			return draw;
		}
	}

	draw.program = &*shadowProgram;

	draw.pipeline = PipelineFor( shadowPipelines, *shadowLibrary,
								 shadowProgram->vertexLayout(),
								 stateBits, cullType, stencil );

	return draw;
}

/*
====================
idEacpRenderProgs::FogDraw

The fog volume, which is the shadow cache's shape again - one program with no
sampling dimension to search, and a pipeline per state.

Two states reach it and they are RB_FogPass's own two: the surfaces inside the
volume at GLS_DEPTHFUNC_EQUAL over what the depth fill wrote, and the light's
frustum triangles at GLS_DEPTHFUNC_LESS with the cull reversed, the frustum
never having been in the depth buffer at all. A mirror doubles both, the cull
being part of a pipeline here.
====================
*/
idEacpRenderProgs::fogDraw_t idEacpRenderProgs::FogDraw( int stateBits, int cullType ) {
	fogDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	if ( !fogProgram.has_value() ) {
		fogProgram.emplace();
		fogLibrary.emplace( GPU::Device::shared(), fogProgram->source() );

		if ( !fogLibrary->isValid() ) {
			common->Warning( "eacp: the fog shader failed to compile" );
			fogProgram.reset();
			return draw;
		}
	}

	draw.program = &*fogProgram;

	draw.pipeline = PipelineFor( fogPipelines, *fogLibrary, fogProgram->vertexLayout(),
								 stateBits, cullType, ES_IGNORE );

	return draw;
}

/*
====================
idEacpRenderProgs::BlendLightDraw

One stage of one blend light: the program its two images are sampled through and
the pipeline that stage's blend mode compiles to.

The state is the widest of any cache here, because it is the light material's
own: RB_BlendLight draws each stage at GLS_DEPTHMASK | stage->drawStateBits |
GLS_DEPTHFUNC_EQUAL, and a stage's bits are whatever its `blend` keyword said -
`add` on fogs/glare, `blend` on fogs/pitFog, `filter` on fogs/filter. So a level
with two blend lights of different materials is two pipelines even if it is one
program.
====================
*/
idEacpRenderProgs::blendLightDraw_t
idEacpRenderProgs::BlendLightDraw( const idImage *projection, const idImage *falloff,
								   int stateBits, int cullType ) {
	blendLightDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	idEacpBlendLightProgram::sampling_t	sampling;

	sampling.projection = R_EacpSampling( projection );
	sampling.falloff = R_EacpSampling( falloff );

	const int	key = GPU::samplingIndex( sampling.projection )
		| ( GPU::samplingIndex( sampling.falloff ) << 2 );

	blendLightVariant_t *	variant = NULL;

	for ( int i = 0 ; i < blendLights.size() ; i++ ) {
		if ( blendLights[i]->key == key ) {
			variant = blendLights[i];
			break;
		}
	}

	if ( !variant ) {
		variant = &blendLights.createNew();

		variant->key = key;
		variant->program.emplace( sampling );
		variant->library.emplace( GPU::Device::shared(), variant->program->source() );

		if ( !variant->library->isValid() ) {
			common->Warning( "eacp: the blend light shader failed to compile" );
			variant->program.reset();
			return draw;
		}
	}

	if ( !variant->program.has_value() ) {
		return draw;
	}

	draw.program = &*variant->program;

	draw.pipeline = PipelineFor( variant->pipelines, *variant->library,
								 variant->program->vertexLayout(),
								 stateBits, cullType, ES_IGNORE );

	return draw;
}

/*
====================
idEacpRenderProgs::BlitProgram

The one quad both blits are drawn with, compiled on the first of them. Which
attachments it is compiled *for* is the two callers' business and is the whole
of what separates them - see BlitDraw and CaptureDraw.

NULL if the shader would not compile, which both callers answer by skipping
their draw.
====================
*/
idEacpBlitProgram *idEacpRenderProgs::BlitProgram( void ) {
	if ( !blitProgram.has_value() ) {
		blitProgram.emplace();
		blitLibrary.emplace( GPU::Device::shared(), blitProgram->source() );

		if ( !blitLibrary->isValid() ) {
			common->Warning( "eacp: the blit shader failed to compile, so the frame "
							 "will not reach the screen" );
			blitProgram.reset();
			return NULL;
		}
	}

	return &*blitProgram;
}

/*
====================
idEacpRenderProgs::BlitDraw

The frame onto the screen, and one of the two pipelines here that PipelineFor
does not build.

Everything else draws into the render target, which is single-sampled and
carries a depth-stencil buffer; this draws into the *drawable*, which is
whatever GPUView was configured with. The two have to be described differently
or the backend rejects the draw - so this builds its descriptor by hand rather
than pretending the difference away with an argument to the shared one.

There is no state to key it on. It is one quad, opaque, with no test of any
kind, drawn once a frame.
====================
*/
idEacpRenderProgs::blitDraw_t idEacpRenderProgs::BlitDraw( void ) {
	blitDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	eacp::GPU::GPUView *	view = R_EacpGetView();

	if ( !view ) {
		return draw;
	}

	draw.program = BlitProgram();

	if ( !draw.program ) {
		return draw;
	}

	if ( !blitPipeline ) {
		GPU::RenderPipelineDescriptor	descriptor;

		descriptor.library = &*blitLibrary;
		descriptor.vertexLayout = blitProgram->vertexLayout();
		descriptor.topology = GPU::PrimitiveTopology::Triangles;

		descriptor.colorFormat = GPU::PixelFormat::BGRA8Unorm;
		descriptor.sampleCount = view->sampleCount();

		// The drawable's own attachments, which the view asked for and every
		// pipeline drawing into it has to name - even this one, which tests
		// neither. Always with no write is the test not happening.
		descriptor.depth = true;
		descriptor.depthCompare = GPU::CompareFunction::Always;
		descriptor.depthWrite = false;
		descriptor.stencil = view->hasStencil();

		blitPipeline.create( GPU::Device::shared(), descriptor );

		if ( !blitPipeline->isValid() ) {
			common->Warning( "eacp: no pipeline for the blit, so the frame will not "
							 "reach the screen" );
			blitPipeline.reset();
			return draw;
		}
	}

	draw.pipeline = blitPipeline;

	return draw;
}

/*
====================
idEacpRenderProgs::CaptureDraw

The same quad into an idImage's texture rather than onto the screen -
_currentRender and _scratch, which is step 4e.3.

A third description of the same shader, and each of the three fields that
differ is the destination's rather than a choice. The image's texture is
RGBA8Unorm, being every other image on this backend's format and what a
material samples; it carries no depth buffer, an image having no use for one;
and it is single-sampled like everything a texture target does.
====================
*/
idEacpRenderProgs::blitDraw_t idEacpRenderProgs::CaptureDraw( void ) {
	blitDraw_t	draw;

	draw.program = BlitProgram();
	draw.pipeline = NULL;

	if ( !draw.program ) {
		return draw;
	}

	if ( !capturePipeline ) {
		GPU::RenderPipelineDescriptor	descriptor;

		descriptor.library = &*blitLibrary;
		descriptor.vertexLayout = blitProgram->vertexLayout();
		descriptor.topology = GPU::PrimitiveTopology::Triangles;

		descriptor.colorFormat = GPU::pixelFormatFor( GPU::TextureFormat::RGBA8Unorm );
		descriptor.sampleCount = 1;

		// No attachment to name, so nothing to test against and nothing to
		// declare. A pipeline claiming a depth buffer the pass has not got is a
		// validation error rather than an untested draw.
		descriptor.depth = false;
		descriptor.stencil = false;

		capturePipeline.create( GPU::Device::shared(), descriptor );

		if ( !capturePipeline->isValid() ) {
			common->Warning( "eacp: no pipeline for the frame capture, so "
							 "_currentRender will not be filled in" );
			capturePipeline.reset();
			return draw;
		}
	}

	draw.pipeline = capturePipeline;

	return draw;
}

/*
====================
idEacpRenderProgs::DepthCopyDraw

The frame's depth buffer into _currentDepth. One program and one pipeline, both
compiled on the first capture of a run - which is the first depth fill of the
first 3D view, so in practice the first frame that draws a world.

Three fields again, and all three are the destination's: R32Float because a
window depth needs the mantissa (see TextureFormat::R32Float), no depth
attachment because the depth here is what is being *read*, and single-sampled
like every texture target.
====================
*/
idEacpRenderProgs::depthCopyDraw_t idEacpRenderProgs::DepthCopyDraw( void ) {
	depthCopyDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	if ( !depthCopyProgram.has_value() ) {
		depthCopyProgram.emplace();
		depthCopyLibrary.emplace( GPU::Device::shared(), depthCopyProgram->source() );

		if ( !depthCopyLibrary->isValid() ) {
			R_EacpShaderCompileFailed( "depth capture" );
			depthCopyLibrary.reset();
			depthCopyProgram.reset();
			return draw;
		}
	}

	if ( !depthCopyLibrary.has_value() ) {
		return draw;
	}

	draw.program = &*depthCopyProgram;

	if ( !depthCopyPipeline ) {
		GPU::RenderPipelineDescriptor	descriptor;

		descriptor.library = &*depthCopyLibrary;
		descriptor.vertexLayout = depthCopyProgram->vertexLayout();
		descriptor.topology = GPU::PrimitiveTopology::Triangles;

		descriptor.colorFormat = GPU::pixelFormatFor( GPU::TextureFormat::R32Float );
		descriptor.sampleCount = 1;

		descriptor.depth = false;
		descriptor.stencil = false;

		depthCopyPipeline.create( GPU::Device::shared(), descriptor );

		if ( !depthCopyPipeline->isValid() ) {
			common->Warning( "eacp: no pipeline for the depth capture, so "
							 "_currentDepth will not be filled in" );
			depthCopyPipeline.reset();
			return draw;
		}
	}

	draw.pipeline = depthCopyPipeline;

	return draw;
}

/*
====================
idEacpRenderProgs::SoftParticleDraw

soft_particle.vfp, whose one variable texture is the particle's own image - so
the key is that one sampling and the space is four. _currentDepth's is a
constant, for the reason the constructor gives.
====================
*/
idEacpRenderProgs::softParticleDraw_t
idEacpRenderProgs::SoftParticleDraw( const idImage *image, int stateBits, int cullType ) {
	softParticleDraw_t	draw;

	draw.program = NULL;
	draw.pipeline = NULL;

	const GPU::TextureSampling	sampling = R_EacpSampling( image );

	texgenVariant_t<idEacpSoftParticleProgram> *	variant =
		TexgenVariant( softParticles, GPU::samplingIndex( sampling ), "soft particle",
					   [&]( std::optional<idEacpSoftParticleProgram> &program ) {
						   program.emplace( sampling );
					   } );

	if ( !variant ) {
		return draw;
	}

	draw.program = &*variant->program;

	draw.pipeline = PipelineFor( variant->pipelines, *variant->library,
								 variant->program->vertexLayout(),
								 stateBits, cullType, ES_IGNORE );

	return draw;
}

/*
====================
	Geometry.

	idVertexCache hands out plain system-memory pointers on this backend -
	ARBVertexBufferObjectAvailable is false, so it never generates a buffer
	object - and a draw has to put those bytes somewhere the GPU can read them.
	StreamingBuffers is that somewhere, and it is idVertexCache's own shape: an
	arena per frame that may still be in flight, recycled only once the frame
	that used it cannot be on the GPU any more.

	Several writes in one frame get several slices of one arena, which is the
	part that matters here: a 3D frame is a thousand draws and more, every one
	of them still queued when the next is written - and as slices they are one
	GPU resource a frame rather than one per draw, which is what the seconds of
	stutter after a level load turned out to be made of.
====================
*/
GPU::BufferRange idEacpRenderProgs::StreamVertices( const void *data, std::size_t bytes ) {
	return vertexStream.write( data, bytes );
}

GPU::BufferRange idEacpRenderProgs::StreamIndices( const void *data, std::size_t bytes ) {
	return indexStream.write( data, bytes );
}
