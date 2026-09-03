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
#include "idlib/hashing/MD4.h"
#include "renderer/tr_local.h"

#include "renderer/Image.h"

/*
PROBLEM: compressed textures may break the zero clamp rule!
*/

static bool FormatIsDXT( int internalFormat ) {
	if ( (internalFormat < GL_COMPRESSED_RGB_S3TC_DXT1_EXT
	       || internalFormat > GL_COMPRESSED_RGBA_S3TC_DXT5_EXT)
	    && internalFormat != GL_COMPRESSED_RGBA_BPTC_UNORM ) {
		return false;
	}
	return true;
}

int MakePowerOfTwo( int num ) {
	int		pot;
	for (pot = 1 ; pot < num ; pot<<=1) {
	}
	return pot;
}

/*
================
BitsForInternalFormat

Used for determining memory utilization
================
*/
int idImage::BitsForInternalFormat( int internalFormat ) const {
	switch ( internalFormat ) {
	case GL_INTENSITY8:
	case 1:
		return 8;
	case 2:
	case GL_LUMINANCE8_ALPHA8:
		return 16;
	case 3:
		return 32;		// on some future hardware, this may actually be 24, but be conservative
	case 4:
		return 32;
	case GL_LUMINANCE8:
		return 8;
	case GL_ALPHA8:
		return 8;
	case GL_RGBA8:
		return 32;
	case GL_RGB8:
		return 32;		// on some future hardware, this may actually be 24, but be conservative
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
		return 4;
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		return 4;
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		return 8;
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		return 8;
	case GL_COMPRESSED_RGBA_BPTC_UNORM:
		return 8;
	case GL_RGBA4:
		return 16;
	case GL_RGB5:
		return 16;
	case GL_COLOR_INDEX8_EXT:
		return 8;
	case GL_COLOR_INDEX:
		return 8;
	case GL_COMPRESSED_RGB_ARB:
		return 4;			// not sure
	case GL_COMPRESSED_RGBA_ARB:
		return 8;			// not sure
	default:
		common->Error( "R_BitsForInternalFormat: BAD FORMAT:%i", internalFormat );
	}
	return 0;
}

//=======================================================================


static byte	mipBlendColors[16][4] = {
	{0,0,0,0},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
};

/*
===============
SelectInternalFormat

This may need to scan six cube map images
===============
*/
int idImage::SelectInternalFormat( const byte **dataPtrs, int numDataPtrs, int width, int height,
									 textureDepth_t minimumDepth ) const {
	int		i, c;
	const byte	*scan;
	int		rgbOr, rgbAnd, aOr, aAnd;
	int		rgbDiffer, rgbaDiffer;

	// TODO: or always use BC7 if available? do textures take longer to load then?
	//       would look better at least...
	const bool useBC7compression = glConfig.bptcTextureCompressionAvailable
						&& globalImages->image_useCompression.GetInteger() == 2;

	// determine if the rgb channels are all the same
	// and if either all rgb or all alpha are 255
	c = width*height;
	rgbDiffer = 0;
	rgbaDiffer = 0;
	rgbOr = 0;
	rgbAnd = -1;
	aOr = 0;
	aAnd = -1;

	for ( int side = 0 ; side < numDataPtrs ; side++ ) {
		scan = dataPtrs[side];
		for ( i = 0; i < c; i++, scan += 4 ) {
			int		cor, cand;

			aOr |= scan[3];
			aAnd &= scan[3];

			cor = scan[0] | scan[1] | scan[2];
			cand = scan[0] & scan[1] & scan[2];

			// if rgb are all the same, the or and and will match
			rgbDiffer |= ( cor ^ cand );

			rgbOr |= cor;
			rgbAnd &= cand;

			cor |= scan[3];
			cand &= scan[3];

			rgbaDiffer |= ( cor ^ cand );
		}
	}

	// we assume that all 0 implies that the alpha channel isn't needed,
	// because some tools will spit out 32 bit images with a 0 alpha instead
	// of 255 alpha, but if the alpha actually is referenced, there will be
	// different behavior in the compressed vs uncompressed states.
	bool needAlpha;
	if ( aAnd == 255 || aOr == 0 ) {
		needAlpha = false;
	} else {
		needAlpha = true;
	}

	// catch normal maps first
	if ( minimumDepth == TD_BUMP ) {
		// image_useNormalCompression 1 used to mean GL_COLOR_INDEX8_EXT here, a
		// palettised normal map for the nv_10 and nv_20 paths through
		// GL_EXT_shared_texture_palette. That extension has been gone from
		// hardware for twenty years and step 5 deleted the path with it, so 1
		// and 2 now mean the same thing.
		if ( globalImages->image_useCompression.GetBool() && globalImages->image_useNormalCompression.GetInteger() && glConfig.textureCompressionAvailable ) {
			if ( useBC7compression ) {
				return GL_COMPRESSED_RGBA_BPTC_UNORM;
			} else {
				// image_useNormalCompression == 2 uses rxgb format which produces really good quality for medium settings
				return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			}
		} else {
			// we always need the alpha channel for bump maps for swizzling
			return GL_RGBA8;
		}
	}

	// allow a complete override of image compression with a cvar
	if ( !globalImages->image_useCompression.GetBool() ) {
		minimumDepth = TD_HIGH_QUALITY;
	}

	if ( minimumDepth == TD_SPECULAR ) {
		// we are assuming that any alpha channel is unintentional
		if ( glConfig.textureCompressionAvailable ) {
			return useBC7compression ? GL_COMPRESSED_RGBA_BPTC_UNORM : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
		} else {
			return GL_RGB5;
		}
	}
	if ( minimumDepth == TD_DIFFUSE ) {
		// we might intentionally have an alpha channel for alpha tested textures
		if ( glConfig.textureCompressionAvailable ) {
			if ( useBC7compression ) {
				return GL_COMPRESSED_RGBA_BPTC_UNORM;
			}
			if ( !needAlpha ) {
				return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
			} else {
				return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
			}
		} else if ( ( aAnd == 255 || aOr == 0 ) ) {
			return GL_RGB5;
		} else {
			return GL_RGBA4;
		}
	}

	// there will probably be some drivers that don't
	// correctly handle the intensity/alpha/luminance/luminance+alpha
	// formats, so provide a fallback that only uses the rgb/rgba formats
	if ( !globalImages->image_useAllFormats.GetBool() ) {
		// pretend rgb is varying and inconsistant, which
		// prevents any of the more compact forms
		rgbDiffer = 1;
		rgbaDiffer = 1;
		rgbAnd = 0;
	}

	// cases without alpha
	if ( !needAlpha ) {
		if ( minimumDepth == TD_HIGH_QUALITY ) {
			return GL_RGB8;			// four bytes
		}
		if ( glConfig.textureCompressionAvailable ) {
			return useBC7compression ? GL_COMPRESSED_RGBA_BPTC_UNORM    // 1byte/pixel
			                         : GL_COMPRESSED_RGB_S3TC_DXT1_EXT; // half byte
		}
		return GL_RGB5;			// two bytes
	}

	// cases with alpha
	if ( !rgbaDiffer ) {
		if ( minimumDepth != TD_HIGH_QUALITY && glConfig.textureCompressionAvailable ) {
			return useBC7compression ? GL_COMPRESSED_RGBA_BPTC_UNORM : GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; // one byte
		}
		return GL_INTENSITY8;	// single byte for all channels
	}

#if 0
	// we don't support alpha textures any more, because there
	// is a discrepancy in the definition of TEX_ENV_COMBINE that
	// causes them to be treated as 0 0 0 A, instead of 1 1 1 A as
	// normal texture modulation treats them
	if ( rgbAnd == 255 ) {
		return GL_ALPHA8;		// single byte, only alpha
	}
#endif

	if ( minimumDepth == TD_HIGH_QUALITY ) {
		return GL_RGBA8;	// four bytes
	}
	if ( glConfig.textureCompressionAvailable ) {
		return useBC7compression ? GL_COMPRESSED_RGBA_BPTC_UNORM : GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; // one byte
	}
	if ( !rgbDiffer ) {
		return GL_LUMINANCE8_ALPHA8;	// two bytes, max quality
	}
	return GL_RGBA4;	// two bytes
}

/*
==================
SetImageFilterAndRepeat
==================
*/
void idImage::SetImageFilterAndRepeat() const {
	renderBackend->SetImageFilterAndRepeat( this );
}

/*
================
idImage::Downsize
helper function that takes the current width/height and might make them smaller
================
*/
void idImage::GetDownsize( int &scaled_width, int &scaled_height ) const {
	int size = 0;

	// perform optional picmip operation to save texture memory
	if ( depth == TD_SPECULAR && globalImages->image_downSizeSpecular.GetInteger() ) {
		size = globalImages->image_downSizeSpecularLimit.GetInteger();
		if ( size == 0 ) {
			size = 64;
		}
	} else if ( depth == TD_BUMP && globalImages->image_downSizeBump.GetInteger() ) {
		size = globalImages->image_downSizeBumpLimit.GetInteger();
		if ( size == 0 ) {
			size = 64;
		}
	} else if ( ( allowDownSize || globalImages->image_forceDownSize.GetBool() ) && globalImages->image_downSize.GetInteger() ) {
		size = globalImages->image_downSizeLimit.GetInteger();
		if ( size == 0 ) {
			size = 256;
		}
	}

	if ( size > 0 ) {
		while ( scaled_width > size || scaled_height > size ) {
			if ( scaled_width > 1 ) {
				scaled_width >>= 1;
			}
			if ( scaled_height > 1 ) {
				scaled_height >>= 1;
			}
		}
	}

	// clamp to minimum size
	if ( scaled_width < 1 ) {
		scaled_width = 1;
	}
	if ( scaled_height < 1 ) {
		scaled_height = 1;
	}

	// clamp size to the hardware specific upper limit
	// scale both axis down equally so we don't have to
	// deal with a half mip resampling
	// This causes a 512*256 texture to sample down to
	// 256*128 on a voodoo3, even though it could be 256*256
	while ( scaled_width > glConfig.maxTextureSize
		|| scaled_height > glConfig.maxTextureSize ) {
		scaled_width >>= 1;
		scaled_height >>= 1;
	}
}

/*
================
GenerateImage

The alpha channel bytes should be 255 if you don't
want the channel.

We need a material characteristic to ask for specific texture modes.

Designed limitations of flexibility:

No support for texture borders.

No support for texture border color.

No support for texture environment colors or GL_BLEND or GL_DECAL
texture environments, because the automatic optimization to single
or dual component textures makes those modes potentially undefined.

No non-power-of-two images.

No palettized textures.

There is no way to specify separate wrap/clamp values for S and T

There is no way to specify explicit mip map levels

================
*/
void idImage::GenerateImage( const byte *pic, int width, int height,
					   textureFilter_t filterParm, bool allowDownSizeParm,
					   textureRepeat_t repeatParm, textureDepth_t depthParm ) {
	bool	preserveBorder;
	byte		*scaledBuffer;
	int			scaled_width, scaled_height;
	byte		*shrunk;

	PurgeImage();

	filter = filterParm;
	allowDownSize = allowDownSizeParm;
	repeat = repeatParm;
	depth = depthParm;

	// if we don't have a rendering context, just return after we
	// have filled in the parms.  We must have the values set, or
	// an image match from a shader before OpenGL starts would miss
	// the generated texture
	if ( !glConfig.isInitialized ) {
		return;
	}

	// don't let mip mapping smear the texture into the clamped border
	if ( repeat == TR_CLAMP_TO_ZERO ) {
		preserveBorder = true;
	} else {
		preserveBorder = false;
	}

	// make sure it is a power of 2
	scaled_width = MakePowerOfTwo( width );
	scaled_height = MakePowerOfTwo( height );

	if ( scaled_width != width || scaled_height != height ) {
		common->Error( "R_CreateImage: not a power of 2 image" );
	}

	// Optionally modify our width/height based on options/hardware
	GetDownsize( scaled_width, scaled_height );

	scaledBuffer = NULL;

	// generate the texture number
	renderBackend->AllocImage( this );

	// select proper internal format before we resample
	internalFormat = SelectInternalFormat( &pic, 1, width, height, depth );

	// copy or resample data as appropriate for first MIP level
	if ( ( scaled_width == width ) && ( scaled_height == height ) ) {
		// we must copy even if unchanged, because the border zeroing
		// would otherwise modify const data
		scaledBuffer = (byte *)R_StaticAlloc( sizeof( unsigned ) * scaled_width * scaled_height );
		memcpy (scaledBuffer, pic, width*height*4);
	} else {
		// resample down as needed (FIXME: this doesn't seem like it resamples anymore!)
		// scaledBuffer = R_ResampleTexture( pic, width, height, width >>= 1, height >>= 1 );
		scaledBuffer = R_MipMap( pic, width, height, preserveBorder );
		width >>= 1;
		height >>= 1;
		if ( width < 1 ) {
			width = 1;
		}
		if ( height < 1 ) {
			height = 1;
		}

		while ( width > scaled_width || height > scaled_height ) {
			shrunk = R_MipMap( scaledBuffer, width, height, preserveBorder );
			R_StaticFree( scaledBuffer );
			scaledBuffer = shrunk;

			width >>= 1;
			height >>= 1;
			if ( width < 1 ) {
				width = 1;
			}
			if ( height < 1 ) {
				height = 1;
			}
		}

		// one might have shrunk down below the target size
		scaled_width = width;
		scaled_height = height;
	}

	uploadHeight = scaled_height;
	uploadWidth = scaled_width;
	type = TT_2D;

	// zero the border if desired, allowing clamped projection textures
	// even after picmip resampling or careless artists.
	if ( repeat == TR_CLAMP_TO_ZERO ) {
		byte	rgba[4];

		rgba[0] = rgba[1] = rgba[2] = 0;
		rgba[3] = 255;
		R_SetBorderTexels( (byte *)scaledBuffer, width, height, rgba );
	}
	if ( repeat == TR_CLAMP_TO_ZERO_ALPHA ) {
		byte	rgba[4];

		rgba[0] = rgba[1] = rgba[2] = 255;
		rgba[3] = 0;
		R_SetBorderTexels( (byte *)scaledBuffer, width, height, rgba );
	}

	if ( generatorFunction == NULL && ( (depth == TD_BUMP && globalImages->image_writeNormalTGA.GetBool()) || (depth != TD_BUMP && globalImages->image_writeTGA.GetBool()) ) ) {
		// Optionally write out the texture to a .tga
		char filename[MAX_IMAGE_NAME];
		ImageProgramStringToCompressedFileName( imgName, filename );
		char *ext = strrchr(filename, '.');
		if ( ext ) {
			strcpy( ext, ".tga" );
			// swap the red/alpha for the write
			/*
			if ( depth == TD_BUMP ) {
				for ( int i = 0; i < scaled_width * scaled_height * 4; i += 4 ) {
					scaledBuffer[ i ] = scaledBuffer[ i + 3 ];
					scaledBuffer[ i + 3 ] = 0;
				}
			}
			*/
			R_WriteTGA( filename, scaledBuffer, scaled_width, scaled_height, false );

			// put it back
			/*
			if ( depth == TD_BUMP ) {
				for ( int i = 0; i < scaled_width * scaled_height * 4; i += 4 ) {
					scaledBuffer[ i + 3 ] = scaledBuffer[ i ];
					scaledBuffer[ i ] = 0;
				}
			}
			*/
		}
	}

	// swap the red and alpha for rxgb support
	// do this even on tga normal maps so we only have to use
	// one fragment program
	// if the image is precompressed ( either in palletized mode or true rxgb mode )
	// then it is loaded above and the swap never happens here
	if ( depth == TD_BUMP && globalImages->image_useNormalCompression.GetInteger() != 1 ) {
		for ( int i = 0; i < scaled_width * scaled_height * 4; i += 4 ) {
			scaledBuffer[ i + 3 ] = scaledBuffer[ i ];
			scaledBuffer[ i ] = 0;
		}
	}
	// upload the main image level
	Bind();


	renderBackend->UploadImageLevel( this, 0, 0, internalFormat, scaled_width, scaled_height, GL_RGBA, scaledBuffer );

	// create and upload the mip map levels, which we do in all cases, even if we don't think they are needed
	int		miplevel;

	miplevel = 0;
	while ( scaled_width > 1 || scaled_height > 1 ) {
		// preserve the border after mip map unless repeating
		shrunk = R_MipMap( scaledBuffer, scaled_width, scaled_height, preserveBorder );
		R_StaticFree( scaledBuffer );
		scaledBuffer = shrunk;

		scaled_width >>= 1;
		scaled_height >>= 1;
		if ( scaled_width < 1 ) {
			scaled_width = 1;
		}
		if ( scaled_height < 1 ) {
			scaled_height = 1;
		}
		miplevel++;

		// this is a visualization tool that shades each mip map
		// level with a different color so you can see the
		// rasterizer's texture level selection algorithm
		// Changing the color doesn't help with lumminance/alpha/intensity formats...
		if ( depth == TD_DIFFUSE && globalImages->image_colorMipLevels.GetBool() ) {
			R_BlendOverTexture( (byte *)scaledBuffer, scaled_width * scaled_height, mipBlendColors[miplevel] );
		}

		// upload the mip map
		renderBackend->UploadImageLevel( this, 0, miplevel, internalFormat, scaled_width, scaled_height,
			GL_RGBA, scaledBuffer );
	}

	if ( scaledBuffer != 0 ) {
		R_StaticFree( scaledBuffer );
	}

	SetImageFilterAndRepeat();

	// The whole chain has been handed over, down to 1x1. Last of all, so that
	// nothing an image's loader does can arrive after the texture is built -
	// which is the property the seam's comment on this call is written against,
	// not an ordering the eacp backend happens to need.
	renderBackend->FinishImage( this );

	// see if we messed anything up
	GL_CheckErrors();
}


/*
====================
GenerateCubeImage

Non-square cube sides are not allowed
====================
*/
void idImage::GenerateCubeImage( const byte *pic[6], int size,
					   textureFilter_t filterParm, bool allowDownSizeParm,
					   textureDepth_t depthParm ) {
	int			scaled_width, scaled_height;
	int			width, height;
	int			i;

	PurgeImage();

	filter = filterParm;
	allowDownSize = allowDownSizeParm;
	depth = depthParm;

	type = TT_CUBIC;

	// if we don't have a rendering context, just return after we
	// have filled in the parms.  We must have the values set, or
	// an image match from a shader before OpenGL starts would miss
	// the generated texture
	if ( !glConfig.isInitialized ) {
		return;
	}

	if ( ! glConfig.cubeMapAvailable ) {
		return;
	}

	width = height = size;

	// generate the texture number
	renderBackend->AllocImage( this );

	// select proper internal format before we resample
	internalFormat = SelectInternalFormat( pic, 6, width, height, depth );

	// don't bother with downsample for now
	scaled_width = width;
	scaled_height = height;

	uploadHeight = scaled_height;
	uploadWidth = scaled_width;

	Bind();

	renderBackend->SetCubeImageFilterAndRepeat( this );

	// upload the base level
	// FIXME: support GL_COLOR_INDEX8_EXT?
	for ( i = 0 ; i < 6 ; i++ ) {
		renderBackend->UploadImageLevel( this, i, 0, internalFormat, scaled_width, scaled_height,
			GL_RGBA, pic[i] );
	}


	// create and upload the mip map levels
	int		miplevel;
	byte	*shrunk[6];

	for ( i = 0 ; i < 6 ; i++ ) {
		shrunk[i] = R_MipMap( pic[i], scaled_width, scaled_height, false );
	}

	miplevel = 1;
	while ( scaled_width > 1 ) {
		for ( i = 0 ; i < 6 ; i++ ) {
			byte	*shrunken;

			renderBackend->UploadImageLevel( this, i, miplevel, internalFormat,
				scaled_width / 2, scaled_height / 2,
				GL_RGBA, shrunk[i] );

			if ( scaled_width > 2 ) {
				shrunken = R_MipMap( shrunk[i], scaled_width/2, scaled_height/2, false );
			} else {
				shrunken = NULL;
			}

			R_StaticFree( shrunk[i] );
			shrunk[i] = shrunken;
		}

		scaled_width >>= 1;
		scaled_height >>= 1;
		miplevel++;
	}

	// Six faces and all their levels are in. Note that this loader's filter call
	// is SetCubeImageFilterAndRepeat, made above *before* the first upload -
	// which is exactly why the end of an upload has an entry point of its own
	// rather than being read off that one.
	renderBackend->FinishImage( this );

	// see if we messed anything up
	GL_CheckErrors();
}


/*
================
ImageProgramStringToFileCompressedFileName
================
*/
void idImage::ImageProgramStringToCompressedFileName( const char *imageProg, char *fileName ) const {
	const char	*s;
	char	*f;

	strcpy( fileName, "dds/" );
	f = fileName + strlen( fileName );

	int depth = 0;

	// convert all illegal characters to underscores
	// this could conceivably produce a duplicated mapping, but we aren't going to worry about it
	for ( s = imageProg ; *s ; s++ ) {
		if ( *s == '/' || *s == '\\' || *s == '(') {
			if ( depth < 4 ) {
				*f = '/';
				depth ++;
			} else {
				*f = ' ';
			}
			f++;
		} else if ( *s == '<' || *s == '>' || *s == ':' || *s == '|' || *s == '"' || *s == '.' ) {
			*f = '_';
			f++;
		} else if ( *s == ' ' && *(f-1) == '/' ) {	// ignore a space right after a slash
		} else if ( *s == ')' || *s == ',' ) {		// always ignore these
		} else {
			*f = *s;
			f++;
		}
	}
	*f++ = 0;
	strcat( fileName, ".dds" );
}

/*
==================
NumLevelsForImageSize
==================
*/
int	idImage::NumLevelsForImageSize( int width, int height ) const {
	int	numLevels = 1;

	while ( width > 1 || height > 1 ) {
		numLevels++;
		width >>= 1;
		height >>= 1;
	}

	return numLevels;
}

/*
================
ShouldImageBePartialCached

Returns true if there is a precompressed image, and it is large enough
to be worth caching
================
*/
bool idImage::ShouldImageBePartialCached() {
	if ( !glConfig.textureCompressionAvailable ) {
		return false;
	}

	if ( !globalImages->image_useCache.GetBool() ) {
		return false;
	}

	// the allowDownSize flag does double-duty as don't-partial-load
	if ( !allowDownSize ) {
		return false;
	}

	if ( globalImages->image_cacheMinK.GetInteger() <= 0 ) {
		return false;
	}

	// if we are doing a copyFiles, make sure the original images are referenced
	if ( fileSystem->PerformingCopyFiles() ) {
		return false;
	}

	char	filename[MAX_IMAGE_NAME];
	ImageProgramStringToCompressedFileName( imgName, filename );

	// get the file timestamp
	fileSystem->ReadFile( filename, NULL, &timestamp );

	if ( timestamp == FILE_NOT_FOUND_TIMESTAMP ) {
		return false;
	}

	// open it and get the file size
	idFile *f;

	f = fileSystem->OpenFileRead( filename );
	if ( !f ) {
		return false;
	}

	int	len = f->Length();
	fileSystem->CloseFile( f );

	if ( len <= globalImages->image_cacheMinK.GetInteger() * 1024 ) {
		return false;
	}

	// we do want to do a partial load
	return true;
}

/*
================
CheckPrecompressedImage

If fullLoad is false, only the small mip levels of the image will be loaded
================
*/
bool idImage::CheckPrecompressedImage( bool fullLoad ) {
	if ( !glConfig.isInitialized || !glConfig.textureCompressionAvailable ) {
		return false;
	}

#if 1 // ( _D3XP had disabled ) - Allow grabbing of DDS's from original Doom pak files
	// if we are doing a copyFiles, make sure the original images are referenced
	if ( fileSystem->PerformingCopyFiles() ) {
		return false;
	}
#endif

	if ( depth == TD_BUMP && globalImages->image_useNormalCompression.GetInteger() != 2 ) {
		return false;
	}

	// god i love last minute hacks :-)
	if ( com_machineSpec.GetInteger() >= 1 && imgName.Icmpn( "lights/", 7 ) == 0 ) {
		return false;
	}

	char filename[MAX_IMAGE_NAME];
	ImageProgramStringToCompressedFileName( imgName, filename );

	// get the file timestamp
	ID_TIME_T precompTimestamp;
	fileSystem->ReadFile( filename, NULL, &precompTimestamp );


	if ( precompTimestamp == FILE_NOT_FOUND_TIMESTAMP ) {
		return false;
	}

	if ( !generatorFunction && timestamp != FILE_NOT_FOUND_TIMESTAMP ) {
		if ( precompTimestamp < timestamp ) {
			// The image has changed after being precompressed
			return false;
		}
	}

	timestamp = precompTimestamp;

	// open it and just read the header
	idFile *f;

	f = fileSystem->OpenFileRead( filename );
	if ( !f ) {
		return false;
	}

	int	len = f->Length();
	if ( len < sizeof( ddsFileHeader_t ) + 4 ) { // +4 for the magic 'DDS ' fourcc at the beginning
		fileSystem->CloseFile( f );
		return false;
	}

#if 0 // DG: no idea what this was exactly meant to achieve, but it's definitely a bad idea:
	//     we might try to load the lower mipmap levels of the image, but we'd still have
	//     to load the whole .dds file first.
	//     What's even weirder: idImage::ShouldImageBePartiallyCached() returns false
	//     if the file size is LESS THAN image_cacheMinK * 1024...
	if ( !fullLoad && len > globalImages->image_cacheMinK.GetInteger() * 1024 ) {
		len = globalImages->image_cacheMinK.GetInteger() * 1024;
	}
#endif

	byte *data = (byte *)R_StaticAlloc( len );

	f->Read( data, len );

	fileSystem->CloseFile( f );

	unsigned int magic = LittleInt( *(unsigned int *)data );
	ddsFileHeader_t	*_header = (ddsFileHeader_t *)(data + 4);
	int ddspf_dwFlags = LittleInt( _header->ddspf.dwFlags );
	unsigned int ddspf_dwFourCC = LittleInt( _header->ddspf.dwFourCC );

	if ( magic != DDS_MAKEFOURCC('D', 'D', 'S', ' ')) {
		common->Printf( "CheckPrecompressedImage( %s ): magic != 'DDS '\n", imgName.c_str() );
		R_StaticFree( data );
		return false;
	}

	// A paletted .dds needed GL_EXT_shared_texture_palette, which went with the
	// rest of the palettised path in step 5. Load the full image instead.
	if ( ddspf_dwFlags & DDSF_ID_INDEXCOLOR ) {
		R_StaticFree( data );
		return false;
	}

	// DG: same if this is a BC7 (BPTC) texture but the GPU doesn't support that
	//     or if it uses the additional DX10 header and is *not* a BC7 texture
	bool isBC7 = false;
	if ( ddspf_dwFourCC == DDS_MAKEFOURCC( 'D', 'X', '1', '0' ) ) {
		ddsDXT10addHeader_t *dx10Header = (ddsDXT10addHeader_t *)( data + 4 + sizeof(ddsFileHeader_t) );
		unsigned int dxgiFormat = LittleInt( dx10Header->dxgiFormat );
		if ( dxgiFormat == 98 ) {
			isBC7 = true;
		} else {
			common->Warning( "Image file '%s' has unsupported dxgiFormat %d - dhewm3 only supports DXGI_FORMAT_BC7_UNORM (98)!",
			                 filename, dxgiFormat);
			R_StaticFree( data );
			return false;
		}
	} else if ( ddspf_dwFourCC == DDS_MAKEFOURCC( 'B', 'C', '7', '0' )
	           || ddspf_dwFourCC == DDS_MAKEFOURCC( 'B', 'C', '7', 'L' ) )
	{
		isBC7 = true;
	}
	if ( isBC7 && !glConfig.bptcTextureCompressionAvailable ) {
		R_StaticFree( data );
		return false;
	}
	if ( glConfig.bptcTextureCompressionAvailable
	    && globalImages->image_usePrecompressedTextures.GetInteger() == 2 )
	{
		// only high quality compressed textures, i.e. BC7 (BPTC), are welcome
		// or uncompressed ones (that have no FOURCC flag set)
		if ( !isBC7 && (ddspf_dwFlags & DDSF_FOURCC) != 0 ) {
			R_StaticFree( data );
			return false;
		}
	}

	// upload all the levels
	UploadPrecompressedImage( data, len );

	R_StaticFree( data );

	return true;
}

/*
===================
UploadPrecompressedImage

This can be called by the front end during nromal loading,
or by the backend after a background read of the file
has completed
===================
*/
void idImage::UploadPrecompressedImage( byte *data, int len ) {
	ddsFileHeader_t	*header = (ddsFileHeader_t *)(data + 4);

	// ( not byte swapping dwReserved1 dwReserved2 )
	header->dwSize = LittleInt( header->dwSize );
	header->dwFlags = LittleInt( header->dwFlags );
	header->dwHeight = LittleInt( header->dwHeight );
	header->dwWidth = LittleInt( header->dwWidth );
	header->dwPitchOrLinearSize = LittleInt( header->dwPitchOrLinearSize );
	header->dwDepth = LittleInt( header->dwDepth );
	header->dwMipMapCount = LittleInt( header->dwMipMapCount );
	header->dwCaps1 = LittleInt( header->dwCaps1 );
	header->dwCaps2 = LittleInt( header->dwCaps2 );

	header->ddspf.dwSize = LittleInt( header->ddspf.dwSize );
	header->ddspf.dwFlags = LittleInt( header->ddspf.dwFlags );
	header->ddspf.dwFourCC = LittleInt( header->ddspf.dwFourCC );
	header->ddspf.dwRGBBitCount = LittleInt( header->ddspf.dwRGBBitCount );
	header->ddspf.dwRBitMask = LittleInt( header->ddspf.dwRBitMask );
	header->ddspf.dwGBitMask = LittleInt( header->ddspf.dwGBitMask );
	header->ddspf.dwBBitMask = LittleInt( header->ddspf.dwBBitMask );
	header->ddspf.dwABitMask = LittleInt( header->ddspf.dwABitMask );

	// generate the texture number
	renderBackend->AllocImage( this );

	int externalFormat = 0;

	precompressedFile = true;

	uploadWidth = header->dwWidth;
	uploadHeight = header->dwHeight;
	size_t additionalHeaderOffset = 0; // used if the DDS has a DDS_HEADER_DXT10
	if ( header->ddspf.dwFlags & DDSF_FOURCC ) {
		switch ( header->ddspf.dwFourCC ) {
		case DDS_MAKEFOURCC( 'D', 'X', 'T', '1' ):
			if ( header->ddspf.dwFlags & DDSF_ALPHAPIXELS ) {
				internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
			} else {
				internalFormat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
			}
			break;
		case DDS_MAKEFOURCC( 'D', 'X', 'T', '3' ):
			internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
			break;
		case DDS_MAKEFOURCC( 'D', 'X', 'T', '5' ):
			internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			break;
		case DDS_MAKEFOURCC( 'R', 'X', 'G', 'B' ):
			internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			break;
		case DDS_MAKEFOURCC( 'B', 'C', '7', '0' ): // BC7 aka BPTC - inofficial FourCCs
		case DDS_MAKEFOURCC( 'B', 'C', '7', 'L' ):
			internalFormat = GL_COMPRESSED_RGBA_BPTC_UNORM;
			break;
		case DDS_MAKEFOURCC( 'D', 'X', '1', '0' ): // BC7 aka BPTC - the official dxgi way
			additionalHeaderOffset = 20;
			// Note: this is a bit hacky, but in CheckPrecompressedImage() we made sure
			//       that only BC7 UNORM is accepted if the FourCC is 'DX10'
			internalFormat = GL_COMPRESSED_RGBA_BPTC_UNORM;
			break;
		default:
			common->Warning( "Invalid compressed internal format\n" );
			// AllocImage has already run, so a backend that gathers an upload
			// has a slot open on this image. Closing it with no levels builds
			// nothing, which is the right answer, and leaves the slot free for
			// the next image rather than for this one to keep.
			renderBackend->FinishImage( this );
			return;
		}
	} else if ( ( header->ddspf.dwFlags & DDSF_RGBA ) && header->ddspf.dwRGBBitCount == 32 ) {
		externalFormat = GL_BGRA_EXT;
		internalFormat = GL_RGBA8;
	} else if ( ( header->ddspf.dwFlags & DDSF_RGB ) && header->ddspf.dwRGBBitCount == 32 ) {
		externalFormat = GL_BGRA_EXT;
		internalFormat = GL_RGBA8;
	} else if ( ( header->ddspf.dwFlags & DDSF_RGB ) && header->ddspf.dwRGBBitCount == 24 ) {
		if ( header->ddspf.dwFlags & DDSF_ID_INDEXCOLOR ) {
			externalFormat = GL_COLOR_INDEX;
			internalFormat = GL_COLOR_INDEX8_EXT;
		} else {
			externalFormat = GL_BGR_EXT;
			internalFormat = GL_RGB8;
		}
	} else if ( header->ddspf.dwRGBBitCount == 8 ) {
		externalFormat = GL_ALPHA;
		internalFormat = GL_ALPHA8;
	} else {
		common->Warning( "Invalid uncompressed internal format\n" );
		renderBackend->FinishImage( this );	// as above
		return;
	}

	type = TT_2D;			// FIXME: we may want to support pre-compressed cube maps in the future

	Bind();

	int numMipmaps = 1;
	if ( header->dwFlags & DDSF_MIPMAPCOUNT ) {
		numMipmaps = header->dwMipMapCount;
	}

	int uw = uploadWidth;
	int uh = uploadHeight;
	int lastUW = uw, lastUH = uh;

	// We may skip some mip maps if we are downsizing
	int skipMip = 0;
	GetDownsize( uploadWidth, uploadHeight );

	byte *imagedata = data + sizeof(ddsFileHeader_t) + 4 + additionalHeaderOffset;

	for ( int i = 0 ; i < numMipmaps; i++ ) {
		int size = 0;
		if ( FormatIsDXT( internalFormat ) ) {
			size = ( ( uw + 3 ) / 4 ) * ( ( uh + 3 ) / 4 ) *
				(internalFormat <= GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16);
		} else {
			size = uw * uh * (header->ddspf.dwRGBBitCount / 8);
		}

		// The one bound on this walk, and it belongs here because this is the
		// only layer that knows `len`. numMipmaps comes out of the file's own
		// header and the loop follows it whatever the payload actually holds, so
		// a .dds claiming eleven levels with one in it walks `imagedata` off the
		// end of the buffer the whole file was read into. That was harmless for
		// as long as no backend dereferenced the pointer - the OpenGL one handed
		// it to glCompressedTexImage2D, which read it - and it is a heap
		// over-read again now that the eacp backend copies from it.
		//
		// Stop rather than skip: the levels above this one are already uploaded,
		// and numMipmaps is corrected so that the "chain stops short" block
		// below says what actually arrived rather than what the header claimed.
		if ( size <= 0 || imagedata + size > data + len ) {
			common->Warning( "%s: .dds is shorter than its own mip chain (level %i of %i)",
							 imgName.c_str(), i, numMipmaps );
			numMipmaps = i;
			break;
		}

		if ( uw > uploadWidth || uh > uploadHeight ) {
			skipMip++;
		} else {
			if ( FormatIsDXT( internalFormat ) ) {
				renderBackend->UploadCompressedImageLevel( this, i - skipMip, internalFormat, uw, uh, size, imagedata );
			} else {
				renderBackend->UploadImageLevel( this, 0, i - skipMip, internalFormat, uw, uh, externalFormat, imagedata );
			}
		}
		lastUW = uw;
		lastUH = uh;

		imagedata += size;
		uw /= 2;
		uh /= 2;
		if (uw < 1) {
			uw = 1;
		}
		if (uh < 1) {
			uh = 1;
		}
	}
	// in case the mipmap chain is incomplete (doesn't go down to 1x1 pixel)
	// the texture may be shown as black unless GL_TEXTURE_MAX_LEVEL is set accordingly
	if ( lastUW > 1 || lastUH > 1 ) {
		numMipmaps -= skipMip;
		if ( numMipmaps == 1 ) {
			// if there is only one mipmap, just don't use mipmapping for this texture
			if ( filter == TF_DEFAULT ) {
				filter = TF_LINEAR;
			}
		} else {
			renderBackend->SetImageMaxLevel( this, numMipmaps - 1 );
		}
	}

	SetImageFilterAndRepeat();

	// Last, so that a backend building its texture here sees the level count
	// SetImageMaxLevel just gave rather than the one the header claimed. The
	// filter the block above may have dropped to TF_LINEAR is *not* a reason:
	// the eacp backend reads `filter` when level 0 arrives, well before this,
	// and a demotion to TF_LINEAR on a one-level .dds costs it nothing because
	// one supplied level and one built level are the same texture.
	renderBackend->FinishImage( this );
}

/*
===============
ActuallyLoadImage

Absolutely every image goes through this path
On exit, the idImage will have a valid OpenGL texture number that can be bound
===============
*/
void	idImage::ActuallyLoadImage( bool checkForPrecompressed, bool fromBackEnd ) {
	int		width, height;
	byte	*pic;

	// this is the ONLY place generatorFunction will ever be called
	if ( generatorFunction ) {
		generatorFunction( this );
		return;
	}

	// if we are a partial image, we are only going to load from a compressed file
	if ( isPartialImage ) {
		if ( CheckPrecompressedImage( false ) ) {
			return;
		}
		// this is an error -- the partial image failed to load
		MakeDefault();
		return;
	}

	//
	// load the image from disk
	//
	if ( cubeFiles != CF_2D ) {
		byte	*pics[6];

		// we don't check for pre-compressed cube images currently
		R_LoadCubeImages( imgName, cubeFiles, pics, &width, &timestamp );

		if ( pics[0] == NULL ) {
			common->Warning( "Couldn't load cube image: %s", imgName.c_str() );
			MakeDefault();
			return;
		}

		GenerateCubeImage( (const byte **)pics, width, filter, allowDownSize, depth );
		precompressedFile = false;

		for ( int i = 0 ; i < 6 ; i++ ) {
			if ( pics[i] ) {
				R_StaticFree( pics[i] );
			}
		}
	} else {
		// see if we have a pre-generated image file that is
		// already image processed and compressed
		if ( checkForPrecompressed && globalImages->image_usePrecompressedTextures.GetBool() ) {
			if ( CheckPrecompressedImage( true ) ) {
				// we got the precompressed image
				return;
			}
			// fall through to load the normal image
		}

		R_LoadImageProgram( imgName, &pic, &width, &height, &timestamp, &depth );

		if ( pic == NULL ) {
			common->Warning( "Couldn't load image: %s", imgName.c_str() );
			MakeDefault();
			return;
		}
/*
		// swap the red and alpha for rxgb support
		// do this even on tga normal maps so we only have to use
		// one fragment program
		// if the image is precompressed ( either in palletized mode or true rxgb mode )
		// then it is loaded above and the swap never happens here
		if ( depth == TD_BUMP && globalImages->image_useNormalCompression.GetInteger() != 1 ) {
			for ( int i = 0; i < width * height * 4; i += 4 ) {
				pic[ i + 3 ] = pic[ i ];
				pic[ i ] = 0;
			}
		}
*/
		// build a hash for checking duplicate image files
		// NOTE: takes about 10% of image load times (SD)
		// may not be strictly necessary, but some code uses it, so let's leave it in
		imageHash = MD4_BlockChecksum( pic, width * height * 4 );

		GenerateImage( pic, width, height, filter, allowDownSize, repeat, depth );
		timestamp = timestamp;
		precompressedFile = false;

		R_StaticFree( pic );
	}
}

//=========================================================================================================

/*
===============
PurgeImage
===============
*/
void idImage::PurgeImage() {
	if ( texnum != TEXTURE_NOT_LOADED ) {
		renderBackend->FreeImage( this );	// this should be the ONLY place it is ever called!
		texnum = TEXTURE_NOT_LOADED;
	}

	// clear all the current binding caches, so the next bind will do a real one
	for ( int i = 0 ; i < MAX_MULTITEXTURE_UNITS ; i++ ) {
		backEnd.glState.tmu[i].current2DMap = -1;
		backEnd.glState.tmu[i].current3DMap = -1;
		backEnd.glState.tmu[i].currentCubeMap = -1;
	}
}

/*
==============
Bind

Automatically enables 2D mapping, cube mapping, or 3D texturing if needed
==============
*/
void idImage::Bind() {
	// if this is an image that we are caching, move it to the front of the LRU chain
	if ( partialImage ) {
		if ( cacheUsageNext ) {
			// unlink from old position
			cacheUsageNext->cacheUsagePrev = cacheUsagePrev;
			cacheUsagePrev->cacheUsageNext = cacheUsageNext;
		}
		// link in at the head of the list
		cacheUsageNext = globalImages->cacheLRU.cacheUsageNext;
		cacheUsagePrev = &globalImages->cacheLRU;

		cacheUsageNext->cacheUsagePrev = this;
		cacheUsagePrev->cacheUsageNext = this;
	}

	// load the image if necessary (FIXME: not SMP safe!)
	if ( texnum == TEXTURE_NOT_LOADED ) {
		if ( partialImage ) {
			// if we have a partial image, go ahead and use that
			this->partialImage->Bind();

			// start a background load of the full thing if it isn't already in the queue
			if ( !backgroundLoadInProgress ) {
				StartBackgroundImageLoad();
			}
			return;
		}

		// load the image on demand here, which isn't our normal game operating mode
		ActuallyLoadImage( true, true );	// check for precompressed, load is from back end
	}


	// bump our statistic counters
	frameUsed = backEnd.frameCount;
	bindCount++;

	renderBackend->BindImage( this );
}

/*
==============
BindFragment

Fragment programs explicitly say which type of map they want, so we don't need to
do any enable / disable changes
==============
*/
void idImage::BindFragment() {
	// if this is an image that we are caching, move it to the front of the LRU chain
	if ( partialImage ) {
		if ( cacheUsageNext ) {
			// unlink from old position
			cacheUsageNext->cacheUsagePrev = cacheUsagePrev;
			cacheUsagePrev->cacheUsageNext = cacheUsageNext;
		}
		// link in at the head of the list
		cacheUsageNext = globalImages->cacheLRU.cacheUsageNext;
		cacheUsagePrev = &globalImages->cacheLRU;

		cacheUsageNext->cacheUsagePrev = this;
		cacheUsagePrev->cacheUsageNext = this;
	}

	// load the image if necessary (FIXME: not SMP safe!)
	if ( texnum == TEXTURE_NOT_LOADED ) {
		if ( partialImage ) {
			// if we have a partial image, go ahead and use that
			this->partialImage->BindFragment();

			// start a background load of the full thing if it isn't already in the queue
			if ( !backgroundLoadInProgress ) {
				StartBackgroundImageLoad();
			}
			return;
		}

		// load the image on demand here, which isn't our normal game operating mode
		ActuallyLoadImage( true, true );	// check for precompressed, load is from back end
	}


	// bump our statistic counters
	frameUsed = backEnd.frameCount;
	bindCount++;

	renderBackend->BindImageFragment( this );
}


/*
====================
CopyFramebuffer
====================
*/
void idImage::CopyFramebuffer( int x, int y, int imageWidth, int imageHeight, bool useOversizedBuffer ) {
	renderBackend->CopyFramebufferToImage( this, x, y, imageWidth, imageHeight, useOversizedBuffer );
}

/*
====================
CopyDepthbuffer

This should just be part of copyFramebuffer once we have a proper image type field
====================
*/
void idImage::CopyDepthbuffer( int x, int y, int imageWidth, int imageHeight, bool useOversizedBuffer )
{
	renderBackend->CopyDepthbufferToImage( this, x, y, imageWidth, imageHeight, useOversizedBuffer );
}

/*
=============
RB_UploadScratchImage

if rows = cols * 6, assume it is a cube map animation
=============
*/
void idImage::UploadScratch( const byte *data, int cols, int rows ) {
	renderBackend->UploadScratchImage( this, data, cols, rows );
}


void idImage::SetClassification( int tag ) {
	classification = tag;
}

/*
==================
StorageSize
==================
*/
int idImage::StorageSize() const {
	int		baseSize;

	if ( texnum == TEXTURE_NOT_LOADED ) {
		return 0;
	}

	switch ( type ) {
	default:
	case TT_2D:
		baseSize = uploadWidth*uploadHeight;
		break;
	case TT_3D:
		baseSize = uploadWidth*uploadHeight*uploadDepth;
		break;
	case TT_CUBIC:
		baseSize = 6 * uploadWidth*uploadHeight;
		break;
	}

	baseSize *= BitsForInternalFormat( internalFormat );

	baseSize /= 8;

	// account for mip mapping
	baseSize = baseSize * 4 / 3;

	return baseSize;
}

/*
==================
Print
==================
*/
void idImage::Print() const {
	if ( precompressedFile ) {
		common->Printf( "P" );
	} else if ( generatorFunction ) {
		common->Printf( "F" );
	} else {
		common->Printf( " " );
	}

	switch ( type ) {
	case TT_2D:
		common->Printf( " " );
		break;
	case TT_3D:
		common->Printf( "3" );
		break;
	case TT_CUBIC:
		common->Printf( "C" );
		break;
	case TT_RECT:
		common->Printf( "R" );
		break;
	default:
		common->Printf( "<BAD TYPE:%i>", type );
		break;
	}

	common->Printf( "%4i %4i ",	uploadWidth, uploadHeight );

	switch( filter ) {
	case TF_DEFAULT:
		common->Printf( "dflt " );
		break;
	case TF_LINEAR:
		common->Printf( "linr " );
		break;
	case TF_NEAREST:
		common->Printf( "nrst " );
		break;
	default:
		common->Printf( "<BAD FILTER:%i>", filter );
		break;
	}

	switch ( internalFormat ) {
	case GL_INTENSITY8:
	case 1:
		common->Printf( "I     " );
		break;
	case 2:
	case GL_LUMINANCE8_ALPHA8:
		common->Printf( "LA    " );
		break;
	case 3:
		common->Printf( "RGB   " );
		break;
	case 4:
		common->Printf( "RGBA  " );
		break;
	case GL_LUMINANCE8:
		common->Printf( "L     " );
		break;
	case GL_ALPHA8:
		common->Printf( "A     " );
		break;
	case GL_RGBA8:
		common->Printf( "RGBA8 " );
		break;
	case GL_RGB8:
		common->Printf( "RGB8  " );
		break;
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
		common->Printf( "DXT1  " );
		break;
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		common->Printf( "DXT1A " );
		break;
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		common->Printf( "DXT3  " );
		break;
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		common->Printf( "DXT5  " );
		break;
	case GL_COMPRESSED_RGBA_BPTC_UNORM:
		common->Printf( "BC7  " );
		break;
	case GL_RGBA4:
		common->Printf( "RGBA4 " );
		break;
	case GL_RGB5:
		common->Printf( "RGB5  " );
		break;
	case GL_COLOR_INDEX8_EXT:
		common->Printf( "CI8   " );
		break;
	case GL_COLOR_INDEX:
		common->Printf( "CI    " );
		break;
	case GL_COMPRESSED_RGB_ARB:
		common->Printf( "RGBC  " );
		break;
	case GL_COMPRESSED_RGBA_ARB:
		common->Printf( "RGBAC " );
		break;
	case 0:
		common->Printf( "      " );
		break;
	default:
		common->Printf( "<BAD FORMAT:%i>", internalFormat );
		break;
	}

	switch ( repeat ) {
	case TR_REPEAT:
		common->Printf( "rept " );
		break;
	case TR_CLAMP_TO_ZERO:
		common->Printf( "zero " );
		break;
	case TR_CLAMP_TO_ZERO_ALPHA:
		common->Printf( "azro " );
		break;
	case TR_CLAMP:
		common->Printf( "clmp " );
		break;
	default:
		common->Printf( "<BAD REPEAT:%i>", repeat );
		break;
	}

	common->Printf( "%4ik ", StorageSize() / 1024 );

	common->Printf( " %s\n", imgName.c_str() );
}
