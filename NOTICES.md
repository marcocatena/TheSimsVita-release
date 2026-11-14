# Third-party notices

The Sims for PS Vita is closed source, apart from the parts below. The licences' full texts are in
[`LICENSES/`](LICENSES/).

## In the engine

### FreeSims and FreeSO: Mozilla Public License 2.0

Parts of the engine (the IFF chunk readers, the HIT sound engine, the XA decoder, the SimAntics
virtual machine's memory, queue and primitives, animation, rooms, roofs, buy mode and others) are
ported from [FreeSims](https://github.com/francot514/FreeSims) and
[FreeSO](https://github.com/riperiperi/FreeSO), under the Mozilla Public License 2.0. The source of
those files, as built into each release, is in [`source/`](source/). Licence:
[`LICENSES/MPL-2.0.txt`](LICENSES/MPL-2.0.txt).

### minimp3: CC0 1.0

[minimp3](https://github.com/lieff/minimp3) by lieff, public domain (CC0 1.0). Licence:
[`LICENSES/minimp3-CC0.txt`](LICENSES/minimp3-CC0.txt).

### stb_image: public domain

[stb_image](https://github.com/nothings/stb) by Sean Barrett, used under its public-domain
alternative (The Unlicense).

## Linked libraries

### vitaGL and vitaShaRK: GNU LGPL 3.0

[vitaGL](https://github.com/Rinnegatamante/vitaGL) (commit 2bdbe89) and
[vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) (v1.7) by Rinnegatamante, statically
linked. Licence: [`LICENSES/LGPL-3.0.txt`](LICENSES/LGPL-3.0.txt), which builds on
[`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt).

Each release has their source as linked (`TheSimsVita-vX.Y-lgpl-sources.zip`) and the app's object
files and link steps for linking it with modified versions of them
(`TheSimsVita-vX.Y-relink-kit.zip`).

### pthread-embedded: GNU LGPL 2.1

[pthread-embedded](https://github.com/vitasdk/pthread-embedded) (from Pthreads-win32), as built by
the VitaSDK, statically linked. Licence and copyright: [`LICENSES/pthread-embedded.txt`](LICENSES/pthread-embedded.txt).
Its source and the relink kit are with each release, as for vitaGL.

### SDL 2: zlib licence

[Simple DirectMedia Layer](https://www.libsdl.org/) 2.32.8 (the VitaSDK's vitaGL build).
Copyright (C) 1997-2025 Sam Lantinga. Licence: [`LICENSES/SDL2-Zlib.txt`](LICENSES/SDL2-Zlib.txt).

### math-neon: MIT

[math-neon](https://github.com/Rinnegatamante/math-neon) (commit 0faab81).

```
Copyright (c) 2015 Lachlan Tychsen-Smith (lachlan.ts@gmail.com)

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
```

### The VitaSDK's C and C++ libraries

newlib (the C library): BSD-style licences, collected in
[`LICENSES/newlib.txt`](LICENSES/newlib.txt). The GCC runtime and C++ library (libgcc, libstdc++):
GPL 3 with the GCC Runtime Library Exception. The VitaSDK's import stubs
([vita-headers](https://github.com/vitasdk/vita-headers)): MIT.

## The game

The Sims, its data and art belong to Electronic Arts. The app's home screen art, start-up screen
and remastered interface buttons are made from the game's artwork. None of the game's files are
included: the app reads your own copy.
