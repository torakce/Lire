#!/bin/bash
# Construit Lire.exe (Windows 64 bits, un seul fichier autonome) depuis Linux avec mingw-w64.
#   prérequis : git, make, x86_64-w64-mingw32-gcc (paquet Debian/Ubuntu « mingw-w64 »)
#   usage     : ./build.sh            -> build/Lire.exe
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MUPDF_VERSION=1.28.6
DEPS="${DEPS:-$HERE/deps}"
MUPDF="$DEPS/mupdf"
CROSS=x86_64-w64-mingw32
OUT="$HERE/build"
mkdir -p "$DEPS" "$OUT"

# 1) MuPDF (moteur de rendu), sources officielles
if [ ! -d "$MUPDF" ]; then
	git clone --depth 1 --branch "$MUPDF_VERSION" https://github.com/ArtifexSoftware/mupdf.git "$MUPDF"
	git -C "$MUPDF" submodule update --init --depth 1 \
		thirdparty/jbig2dec thirdparty/freetype thirdparty/harfbuzz thirdparty/libjpeg \
		thirdparty/lcms2 thirdparty/openjpeg thirdparty/zlib
fi

# Correctifs de performance de Lire sur MuPDF (voir patches/LISEZMOI.md) : appliqués une seule fois
PATCH="$HERE/patches/mupdf-$MUPDF_VERSION-lire.patch"
if ! git -C "$MUPDF" apply --reverse --check "$PATCH" 2>/dev/null; then
	git -C "$MUPDF" apply "$PATCH"
fi

# Uniquement le PDF (et les images) : pas d'EPUB/HTML/XPS/SVG, pas de JavaScript,
# pas de polices de secours Noto (gain de taille) ; les polices standard PDF restent incluses.
MUOPTS="build=release HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_LIBCRYPTO=no \
	html=no xps=no svg=no extract=no mujs=no brotli=no tofu=yes tofu_cjk_ext=yes"
# ARCH_HAS_SSE=0 : seules les fonctions de redressement d'image (inutilisées ici) s'en servent ;
# l'exe reste compatible avec tous les processeurs x64.
make -C "$MUPDF" -j"$(nproc)" $MUOPTS OS=mingw-cross build_prefix=mingw64- \
	CC=$CROSS-gcc CXX=$CROSS-g++ AR=$CROSS-ar XCFLAGS=-DARCH_HAS_SSE=0 libs
LIBDIR=$(ls -d "$MUPDF"/build/mingw64-release* | head -1)

# 2) l'application
$CROSS-windres -O coff --include-dir "$HERE/res" -i "$HERE/res/lire.rc" -o "$OUT/lire_res.o"
$CROSS-gcc -O2 -Wall -municode -mwindows -static -s \
	-I"$MUPDF/include" \
	"$HERE/src/core.c" "$HERE/src/gfx_gl.c" "$HERE/src/win32.c" "$OUT/lire_res.o" \
	"$LIBDIR/libmupdf.a" "$LIBDIR/libmupdf-third.a" \
	-lopengl32 -lgdi32 -lcomdlg32 -lshell32 -ladvapi32 -luser32 -lm \
	-o "$OUT/Lire.exe"
ls -la "$OUT/Lire.exe"
