#!/bin/bash
# =============================================================================
# FFmpeg Windows x64 Build Script — Minimal size with required codecs
# =============================================================================
#
# Prerequisites: MSYS2 with MinGW64 environment
#   Run this script from the MSYS2 MinGW64 shell (NOT the MSYS2 MSYS shell)
#
# Required codecs:
#   - H.264 (libx264)
#   - H.265/HEVC (libx265)
#   - Apple ProRes (built-in)
#   - DNxHD/DNxHR (built-in)
#   - NVIDIA NVENC hardware encoding
#   - DELTACAST VideoMaster input device
#
# =============================================================================

set -e

# ─── Configuration ───────────────────────────────────────────────────────────

# VideoMaster SDK paths — adjust these to your SDK installation
VM_INCLUDE="${VM_INCLUDE:-../../sdk/VideoMaster/resources/include}"
VM_LIB_ORIG="${VM_LIB:-../../sdk/VideoMaster/resources/lib}"

# DeckLink SDK path
DL_INCLUDE="${DL_INCLUDE:-../../sdk/Decklink/Blackmagic_DeckLink_SDK_14.2/Win/include}"

# Number of parallel build jobs
JOBS="${JOBS:-$(nproc)}"

# ─── Step 1: Install dependencies via pacman ─────────────────────────────────

echo "=== Installing build dependencies ==="
pacman -S --needed --noconfirm \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-nasm \
    mingw-w64-x86_64-pkg-config \
    mingw-w64-x86_64-x264 \
    mingw-w64-x86_64-x265 \
    mingw-w64-x86_64-fdk-aac \
    mingw-w64-x86_64-aom \
    mingw-w64-x86_64-libvpx \
    mingw-w64-x86_64-ffnvcodec-headers \
    mingw-w64-x86_64-tools-git \
    make \
    diffutils

# ─── Step 2: Generate MinGW import libraries for VideoMaster DLLs ────────────

echo ""
echo "=== Generating MinGW import libraries for VideoMaster ==="
VM_LIB="build_vm_lib"
mkdir -p "${VM_LIB}"
for dll in "${VM_LIB_ORIG}"/*.dll; do
    name="$(basename "${dll}" .dll)"
    # Convert to lowercase for -l flag compatibility (e.g. VideoMasterHD -> videomasterhd)
    lower="$(echo "${name}" | tr '[:upper:]' '[:lower:]')"
    outlib="${VM_LIB}/lib${lower}.a"
    if [ ! -f "${outlib}" ]; then
        deffile="${VM_LIB}/${lower}.def"
        echo "  ${name}.dll -> lib${lower}.a"
        gendef - "${dll}" > "${deffile}" 2>/dev/null
        dlltool -d "${deffile}" -l "${outlib}" -D "${name}.dll"
    fi
done

# ─── Step 3: Generate DeckLink headers from IDL files ────────────────────────

echo ""
echo "=== Generating DeckLink headers from IDL files ==="
if [ ! -f "${DL_INCLUDE}/DeckLinkAPI.h" ]; then
    echo "  widl: DeckLinkAPI.idl -> DeckLinkAPI.h"
    widl -h -o "${DL_INCLUDE}/DeckLinkAPI.h" "${DL_INCLUDE}/DeckLinkAPI.idl"
else
    echo "  DeckLinkAPI.h already exists, skipping"
fi
if [ ! -f "${DL_INCLUDE}/DeckLinkAPI_i.c" ]; then
    echo "  widl: DeckLinkAPI.idl -> DeckLinkAPI_i.c"
    widl -u -o "${DL_INCLUDE}/DeckLinkAPI_i.c" "${DL_INCLUDE}/DeckLinkAPI.idl"
else
    echo "  DeckLinkAPI_i.c already exists, skipping"
fi

# ─── Step 4: Configure FFmpeg ────────────────────────────────────────────────

echo ""
echo "=== Configuring FFmpeg ==="
echo "  VideoMaster include: ${VM_INCLUDE}"
echo "  VideoMaster lib:     ${VM_LIB} (from ${VM_LIB_ORIG})"
echo "  DeckLink include:    ${DL_INCLUDE}"
echo ""

./configure \
    --arch=x86_64 \
    --target-os=mingw32 \
    \
    --enable-gpl \
    --enable-nonfree \
    --enable-version3 \
    \
    --enable-static \
    --disable-shared \
    \
    --enable-small \
    --disable-debug \
    --disable-doc \
    --disable-htmlpages \
    --disable-manpages \
    --disable-podpages \
    --disable-txtpages \
    \
    --disable-ffplay \
    \
    --enable-libaom \
    --enable-libvpx \
    --enable-libfdk-aac \
    --enable-libx264 \
    --enable-libx265 \
    --enable-nvenc \
    --enable-nvdec \
    --enable-cuvid \
    --enable-ffnvcodec \
    \
    --enable-decklink \
    --enable-videomaster \
    --pkg-config-flags="--static" \
    --extra-cflags="-I${VM_INCLUDE} -I${DL_INCLUDE} -Wno-error=incompatible-pointer-types" \
    --extra-cxxflags="-I${VM_INCLUDE} -I${DL_INCLUDE} -fext-numeric-literals" \
    --extra-ldflags="-L${VM_LIB}" \
    --extra-ldexeflags="-static -static-libgcc -static-libstdc++" \
    --extra-libs="-lole32 -loleaut32 -luuid -lshlwapi" \
    \
    "$@"

# ─── Step 5: Build ───────────────────────────────────────────────────────────

echo ""
echo "=== Building FFmpeg (${JOBS} jobs) ==="
make -j"${JOBS}"

# ─── Step 6: Strip binaries for minimal size ─────────────────────────────────

echo ""
echo "=== Stripping binaries ==="
strip -s ffmpeg.exe 2>/dev/null && echo "  Stripped ffmpeg.exe" || true
strip -s ffprobe.exe 2>/dev/null && echo "  Stripped ffprobe.exe" || true

# ─── Done ────────────────────────────────────────────────────────────────────

echo ""
echo "=== Build complete ==="
echo ""
ls -lh ffmpeg.exe ffprobe.exe 2>/dev/null
echo ""
echo "Codecs included:"
echo "  - H.264 encoder:   libx264 + h264_nvenc (NVIDIA)"
echo "  - H.265 encoder:   libx265 + hevc_nvenc (NVIDIA)"
echo "  - ProRes encoder:  prores, prores_aw, prores_ks (built-in)"
echo "  - DNxHD encoder:   dnxhd (built-in)"
echo "  - NVENC/NVDEC:     hardware acceleration (requires NVIDIA GPU + driver)"
echo "  - VideoMaster:     DELTACAST capture input device"
