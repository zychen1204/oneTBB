#!/usr/bin/env bash
# Builds the oneTBB-ported PARSEC TBB benchmarks (bodytrack, fluidanimate).
#
# usage:
#   TBB_INC=~/ControlStealing/oneTBB/include \
#   TBB_LIB=~/ControlStealing/build/caws \
#   [STATIC=1] ./build.sh <outdir>
#
# STATIC=1 links libtbb.a fully static (for gem5 SE mode); default links
# libtbb.so dynamically (for native runs via LD_LIBRARY_PATH).
set -euo pipefail
cd "$(dirname "$0")"

OUT=${1:?usage: build.sh <outdir>}
TBB_INC=${TBB_INC:?set TBB_INC to oneTBB include dir}
TBB_LIB=${TBB_LIB:?set TBB_LIB to dir containing libtbb.so or libtbb.a}
STATIC=${STATIC:-0}

CXX=${CXX:-g++}
# HAVE_STDINT_H: normally set by PARSEC's configure; without it FlexIO.h
# typedefs DWORD as unsigned long (8 bytes on LP64) and BMP loading breaks.
CXXFLAGS="-O3 -std=gnu++14 -DNDEBUG -D_GNU_SOURCE -DHAVE_STDINT_H=1 -I$TBB_INC"
if [[ $STATIC == 1 ]]; then
    LINK="-static -Wl,--whole-archive $TBB_LIB/libtbb.a -Wl,--no-whole-archive -lpthread -ldl"
else
    LINK="-L$TBB_LIB -ltbb -lpthread -ldl"
fi

mkdir -p "$OUT"

echo "== fluidanimate (tbb) =="
$CXX $CXXFLAGS fluidanimate/tbb.cpp fluidanimate/cellpool.cpp $LINK \
    -o "$OUT/fluidanimate"
$CXX -O2 fluidanimate/fluidcmp.cpp -o "$OUT/fluidcmp"

echo "== bodytrack (tbb, thread model 1) =="
BT_SRC=(bodytrack/TrackingBenchmark/main.cpp
        bodytrack/TrackingBenchmark/AnnealingFactor.cpp
        bodytrack/TrackingBenchmark/BodyGeometry.cpp
        bodytrack/TrackingBenchmark/BodyPose.cpp
        bodytrack/TrackingBenchmark/CameraModel.cpp
        bodytrack/TrackingBenchmark/CovarianceMatrix.cpp
        bodytrack/TrackingBenchmark/ImageMeasurements.cpp
        bodytrack/TrackingBenchmark/ImageProjection.cpp
        bodytrack/TrackingBenchmark/RandomGenerator.cpp
        bodytrack/TrackingBenchmark/TrackingModel.cpp
        bodytrack/TrackingBenchmark/TrackingModelTBB.cpp
        bodytrack/FlexImageLib/FlexIO.cpp
        bodytrack/FlexImageLib/FlexImage.cpp)
$CXX $CXXFLAGS -DUSE_TBB -Ibodytrack/FlexImageLib "${BT_SRC[@]}" $LINK \
    -o "$OUT/bodytrack"

echo "BUILD OK -> $OUT"
