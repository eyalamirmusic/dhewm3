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

#ifndef __MEGATEXTURE_H__
#define __MEGATEXTURE_H__

#include "renderer/Model.h"

class idTextureTile {
public:
	int		x, y;
};

static const int TILE_PER_LEVEL = 4;
static const int MAX_MEGA_CHANNELS = 3;		// normal, diffuse, specular
static const int MAX_LEVELS = 12;
static const int MAX_LEVEL_WIDTH = 512;
static const int TILE_SIZE = MAX_LEVEL_WIDTH / TILE_PER_LEVEL;

class	idMegaTexture;

class idTextureLevel {
public:
	idMegaTexture	*mega;

	int				tileOffset;
	int				tilesWide;
	int				tilesHigh;

	idImage			*image;
	idTextureTile	tileMap[TILE_PER_LEVEL][TILE_PER_LEVEL];

	float			parms[4];

	void			Invalidate();
};

typedef struct {
	int		tileSize;
	int		tilesWide;
	int		tilesHigh;
} megaTextureHeader_t;


// Step 5 deleted the half of this class that drew: SetMappingForSurface,
// BindForViewOrigin, Unbind, SetViewOrigin and the tile streaming under them
// were called from draw_common.cpp and nowhere else, and they bound seven
// texture units and set ARB vertex program parameters to do it. What is left is
// the offline half - MakeMegaTexture builds a .mega file - plus the parse-time
// InitFromMegaFile, so that a material naming a megaTexture still parses into
// the shape it did before. Nothing draws one on this build.
class idMegaTexture {
public:
	bool	InitFromMegaFile( const char *fileBase );

	static	void MakeMegaTexture_f( const idCmdArgs &args );
private:
friend class idTextureLevel;
	static void	GenerateMegaMipMaps( megaTextureHeader_t *header, idFile *file );
	static void	GenerateMegaPreview( const char *fileName );

	idFile			*fileHandle;

	const srfTriangles_t *currentTriMapping;

	idVec3			currentViewOrigin;

	float			localViewToTextureCenter[2][4];

	int				numLevels;
	idTextureLevel	levels[MAX_LEVELS];				// 0 is the highest resolution
	megaTextureHeader_t	header;

	static idCVar	r_megaTextureLevel;
	static idCVar	r_showMegaTexture;
	static idCVar	r_showMegaTextureLabels;
	static idCVar	r_skipMegaTexture;
	static idCVar	r_terrainScale;
};

#endif
