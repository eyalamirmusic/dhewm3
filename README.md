# ABOUT

_dhewm 3_ is a _Doom 3_ GPL source port. **This fork targets macOS (universal binaries) and Windows only.**

The goal of _dhewm 3_ is bring _DOOM 3_ with the help of SDL to all suitable
platforms.

Bugs present in the original _DOOM 3_ will be fixed (when identified) without
altering the original gameplay.

**The official homepage is:** https://dhewm3.org

**Mods supported by dhewm3:** https://dhewm3.org/mods.html

**Mod SDK for dhewm3:**  https://github.com/dhewm/dhewm3-sdk

**The project is hosted at:** https://github.com/dhewm

**Download the latest release:** https://github.com/dhewm/dhewm3/releases/latest

**Consult the FAQ at:** https://github.com/dhewm/dhewm3/wiki/FAQ

**Report bugs here:** https://github.com/dhewm/dhewm3/issues


# CHANGES

Compared to the original _DOOM 3_, the changes of _dhewm 3_ worth mentioning are:

- 64-bit port
- SDL for low-level OS support, OpenGL and input handling
- OpenAL for audio output, all OS-specific audio backends are gone
- OpenAL EFX for EAX reverb effects (read: EAX-like sound effects on all platforms/hardware)
- Gamepad support
    - *Rumble* is currently **not** supported
- Better support for widescreen (and arbitrary display resolutions)
- A build system based on CMake
- An **advanced**, mod-independent **settings menu** (opened with `F10` by default)

See [Changelog.md](./Changelog.md) for a more complete changelog.


# GENERAL NOTES

## Game data and patching

This source release does not contain any game data, the game data is still
covered by the original EULA and must be obeyed as usual.

You must patch the game to the latest version (1.3.1). See the FAQ for details, including
how to get the game data from Steam on Linux or OSX.

Note that the original _Doom 3_ and _Doom 3: Resurrection of Evil_ (together with
_DOOM 3: BFG Edition_, which is *not* supported by dhewm3) are available from the Steam Store at

https://store.steampowered.com/app/208200/DOOM_3/

See https://dhewm3.org/#how-to-install for game data installation instructions.

## Configuration

See [Configuration.md](./Configuration.md) for dhewm3-specific configuration, especially for 
using gamepads or the **new settings menu**.

## Compiling

This fork supports **macOS** (as a universal arm64 + x86_64 binary) and **Windows**.
The build system is [CMake](http://cmake.org/) 3.21 or newer.

Required libraries are not part of the tree:

- [OpenAL Soft](https://openal-soft.org/) (Apple's and Creative's OpenAL are made of fail)
- [SDL 3](https://libsdl.org/)
- libcurl (optional, required for server downloads)

The build is the usual CMake two-liner, run from the root of the repository:

```
cmake -B build
cmake --build build --parallel
```

The results end up in `build/neo/`: `dhewm3.app` (or `dhewm3.exe`), plus the `base`
and `d3xp` game libraries next to it.

Useful options (pass as `-DOPTION=VALUE` to the first command):

| Option | Default | Meaning |
| ------ | ------- | ------- |
| `CMAKE_OSX_ARCHITECTURES` | `arm64;x86_64` | macOS only: which architectures to build for |
| `CMAKE_PREFIX_PATH` | - | where to look for SDL3 and OpenAL Soft |
| `BASE` / `D3XP` | `ON` | build the Doom 3 / Resurrection of Evil game code |
| `HARDLINK_GAME` | `OFF` | link the game code into the executable instead of separate libraries |
| `ASAN` / `UBSAN` | `OFF` | build with the Address / Undefined Behavior Sanitizer |

Run `cmake -LH -B build` for the full list.

### macOS

Install the dependencies with [Homebrew](https://brew.sh):

```
brew install cmake sdl3 openal-soft
```

Homebrew keeps openal-soft keg-only, but CMake finds it anyway - the buildsystem asks
`brew` where it lives.

Homebrew only ships single-architecture libraries, though, so they can't go into a
universal binary. For a **native-only** build - which is what you want while developing -
say so explicitly:

```
cmake -B build -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --parallel
```

For an actual **universal** build you need universal SDL3 and OpenAL Soft. Build them
from source once and point CMake at the result:

```
cmake -S /path/to/SDL -B build-sdl -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_INSTALL_PREFIX=$PWD/deps
cmake --build build-sdl --parallel && cmake --install build-sdl
# ... and the same for openal-soft ...

cmake -B build -DCMAKE_PREFIX_PATH=$PWD/deps
cmake --build build --parallel
```

If a dependency turns out not to be universal, CMake says so at configure time instead
of letting the linker fail with something cryptic. See
[.github/workflows/macos.yml](.github/workflows/macos.yml) for the exact commands CI uses.

Once it's built you can run it right there:

```
./build/neo/dhewm3.app/Contents/MacOS/dhewm3 +set fs_basepath /path/to/your/doom3/
```

*Replace `/path/to/your/doom3/` with the path to your Doom 3 installation - the directory
that contains `base/` with `pak000.pk4` to `pak008.pk4`.*

### Windows

Get the dependencies with [vcpkg](https://vcpkg.io/):

```
vcpkg install sdl3 openal-soft curl
```

Then point CMake at vcpkg's toolchain file and pick an architecture:

```
cmake -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake -B build
cmake --build build --config RelWithDebInfo
```

Use `-A Win32` for a 32bit build. Opening the generated solution in Visual Studio works
too - `dhewm3` is already set as the startup project.

## Contributing

Contributions in the form of Pull Requests or by creating (meaningful) bugreports are welcome!

But please note that **only human-written code** is accepted for dhewm3.

#### Do not submit code developed with the assistance of generative "AI"!

... like Microsoft Copilot, Anthropic Claude, ChatGPT, etc.

Don't generate bugreports/issues or comments with "AI" either.

Doing it anyway may get you banned from this project.

This is not open for discussion.

## Back End Rendering of Stencil Shadows

The Doom 3 GPL source code release **did** not include functionality enabling rendering
of stencil shadows via the "depth fail" method, a functionality commonly known as
"Carmack's Reverse".  
It has been restored in dhewm3 1.5.1 after Creative Labs' [patent](https://patents.google.com/patent/US6384822B1/en)
finally expired.

Note that this did not change the visual appearance of the game, and didn't seem to
make a noticeable performance difference (on halfway-recent hardware) either.

## MayaImport

The code for our Maya export plugin is included, if you are a Maya licensee
you can obtain the SDK from Autodesk.


# LICENSE

See COPYING.txt for the GNU GENERAL PUBLIC LICENSE

ADDITIONAL TERMS:  The Doom 3 GPL Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU GPL which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

EXCLUDED CODE:  The code described below and contained in the Doom 3 GPL Source Code release is not part of the Program covered by the GPL and is expressly excluded from its terms.  You are solely responsible for obtaining from the copyright holder a license for such code and complying with the applicable license terms.

## Dear ImGui

neo/libs/imgui/*

The MIT License (MIT)

Copyright (c) 2014-2024 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## PropTree

neo/tools/common/PropTree/*

Copyright (C) 1998-2001 Scott Ramsay

sramsay@gonavi.com

http://www.gonavi.com

This material is provided "as is", with absolutely no warranty expressed
or implied. Any use is at your own risk.

Permission to use or copy this software for any purpose is hereby granted
without fee, provided the above notices are retained on all copies.
Permission to modify the code and to distribute modified code is granted,
provided the above notices are retained, and a notice that the code was
modified is included with the above copyright notice.

If you use this code, drop me an email.  I'd like to know if you find the code
useful.

## Base64 implementation

neo/idlib/Base64.cpp

Copyright (c) 1996 Lars Wirzenius.  All rights reserved.

June 14 2003: TTimo <ttimo@idsoftware.com>

modified + endian bug fixes

http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=197039

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

## miniz

src/framework/miniz/*

The MIT License (MIT)

Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## IO on .zip files using minizip

src/framework/minizip/*

Copyright (C) 1998-2010 Gilles Vollant (minizip) ( http://www.winimage.com/zLibDll/minizip.html )

Modifications of Unzip for Zip64
Copyright (C) 2007-2008 Even Rouault

Modifications for Zip64 support
Copyright (C) 2009-2010 Mathias Svensson ( http://result42.com )

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

## MD4 Message-Digest Algorithm

neo/idlib/hashing/MD4.cpp

Copyright (C) 1991-2, RSA Data Security, Inc. Created 1991. All
rights reserved.

License to copy and use this software is granted provided that it
is identified as the "RSA Data Security, Inc. MD4 Message-Digest
Algorithm" in all material mentioning or referencing this software
or this function.

License is also granted to make and use derivative works provided
that such works are identified as "derived from the RSA Data
Security, Inc. MD4 Message-Digest Algorithm" in all material
mentioning or referencing the derived work.

RSA Data Security, Inc. makes no representations concerning either
the merchantability of this software or the suitability of this
software for any particular purpose. It is provided "as is"
without express or implied warranty of any kind.

These notices must be retained in any copies of any part of this
documentation and/or software.

## MD5 Message-Digest Algorithm

neo/idlib/hashing/MD5.cpp

This code implements the MD5 message-digest algorithm.
The algorithm is due to Ron Rivest.  This code was
written by Colin Plumb in 1993, no copyright is claimed.
This code is in the public domain; do with it what you wish.

## CRC32 Checksum

neo/idlib/hashing/CRC32.cpp

Copyright (C) 1995-1998 Mark Adler

## stb_image and stb_vorbis

neo/renderer/stb_image.h
neo/sound/stb_vorbis.h

Used to decode JPEG and OGG Vorbis files.

from https://github.com/nothings/stb/

Copyright (c) 2017 Sean Barrett

Released under MIT License and Unlicense (Public Domain)

## Brandelf utility

neo/sys/linux/setup/brandelf.c

Copyright (c) 1996 Søren Schmidt
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer
   in this position and unchanged.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software withough specific prior written permission

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

`$FreeBSD: src/usr.bin/brandelf/brandelf.c,v 1.16 2000/07/02 03:34:08 imp Exp $`

## makeself - Make self-extractable archives on Unix

neo/sys/linux/setup/makeself/*, neo/sys/linux/setup/makeself/README
Copyright (c) Stéphane Peter
Licensing: GPL v2
