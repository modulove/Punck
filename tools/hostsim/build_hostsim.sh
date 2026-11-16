#!/bin/sh
# Builds the PC renderer with MSYS2 MinGW g++ (compiles the real Kick/Snare/HiHat code against stub hardware headers)
# Usage: sh tools/hostsim/build_hostsim.sh [outdir]   ->  renders WAVs into outdir (default build/hostsim)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/Punck/src"
OUT="${1:-$ROOT/build/hostsim}"
CXX="${CXX:-/c/msys64/mingw64/bin/g++}"
export PATH="/c/msys64/mingw64/bin:$PATH"
mkdir -p "$OUT/src"

# Quote includes search the including file's own folder first, so copy the voice sources away from the real
# initialisation.h / VoiceManager.h: the stubs then take their place
for f in voices/Kick.h voices/Kick.cpp voices/Snare.h voices/Snare.cpp voices/HiHat.h voices/HiHat.cpp voices/DrumVoice.h voices/Toms.h voices/Toms.cpp voices/KarplusString.h voices/KarplusString.cpp \
		Filter.h BPFilter.h BPFilter.cpp; do
	cp "$SRC/$f" "$OUT/src/"
done

"$CXX" -std=gnu++20 -D_USE_MATH_DEFINES -DTEST -O2 -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
	-I"$ROOT/tools/hostsim/stub" -I"$OUT/src" -I"$SRC/dsp" -I"$SRC/third_party" \
	"$ROOT/tools/hostsim/render.cpp" "$OUT/src/Kick.cpp" "$OUT/src/Snare.cpp" "$OUT/src/HiHat.cpp" "$OUT/src/BPFilter.cpp" "$OUT/src/Toms.cpp" "$OUT/src/KarplusString.cpp" \
	"$SRC/third_party/plaits/resources.cc" "$SRC/third_party/stmlib/dsp/units.cc" "$SRC/third_party/stmlib/utils/random.cc" \
	-o "$OUT/render.exe"
[ -n "$NORUN" ] || "$OUT/render.exe" "$OUT"
