#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MUPDF="$ROOT/cpp/third_party/mupdf-src"
BUILD="$ROOT/build_linux"
SDK="$BUILD/mupdf-sdk"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -x "$MUPDF/docucraft/build.sh" ]]; then
    echo "MuPDF submodule missing: git submodule update --init --recursive" >&2
    exit 1
fi

if [[ "${REBUILD_MUPDF:-0}" == 1 || ! -f "$SDK/lib/x86_64/libmupdf.a" ]]; then
    nice -n 10 "$MUPDF/docucraft/build.sh" linux --sdk "$SDK" --jobs "$JOBS"
fi

GENERATOR="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && GENERATOR=Ninja

cmake -S "$ROOT/cpp" -B "$BUILD" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-Wformat -Werror=format-security" \
    -DCMAKE_C_FLAGS="-Wformat -Werror=format-security" \
    -DHARDCOPY_BUILD_CLI=ON \
    -DHARDCOPY_TEXT_MODE=ON \
    -DHARDCOPY_VERTICAL_LAYOUT=ON \
    -DHARDCOPY_EPUB=ON \
    -DHARDCOPY_COMIC=ON \
    -DHARDCOPY_DOCX=OFF \
    -DHARDCOPY_PDFIN=ON \
    -DHARDCOPY_PARALLEL=ON \
    -DMUPDF_ROOT="$SDK"
nice -n 10 cmake --build "$BUILD" -j"$JOBS"

echo "hardcopy-cli: $BUILD/cli/hardcopy-cli"
