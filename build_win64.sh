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
VM_INCLUDE="${VM_INCLUDE:-../../sdk/VideoMaster/windows/x64/resources/include}"
VM_LIB_ORIG="${VM_LIB:-../../sdk/VideoMaster/windows/x64/resources/lib}"

# DeckLink SDK path
DL_INCLUDE="${DL_INCLUDE:-../../sdk/Decklink/Blackmagic_DeckLink_SDK_14.2/Win/include}"

# CUDA Toolkit path — auto-detect or set manually
# Prefer 12.x over 13.x (FFmpeg NPP filters need legacy non-_Ctx functions)
# Create a local symlink to avoid spaces in path (breaks gcc -I flags)
if [ -z "${CUDA_PATH}" ]; then
    for d in /c/Program\ Files/NVIDIA\ GPU\ Computing\ Toolkit/CUDA/v12*/; do
        [ -d "$d" ] && CUDA_PATH="$d" && break
    done
    if [ -z "${CUDA_PATH}" ]; then
        for d in /c/Program\ Files/NVIDIA\ GPU\ Computing\ Toolkit/CUDA/v*/; do
            [ -d "$d" ] && CUDA_PATH="$d" && break
        done
    fi
fi
if [ -n "${CUDA_PATH}" ]; then
    CUDA_LOCAL="build_cuda"
    rm -rf "${CUDA_LOCAL}"
    ln -sf "${CUDA_PATH}" "${CUDA_LOCAL}"
    CUDA_INCLUDE="${CUDA_LOCAL}/include"
    CUDA_LIB="${CUDA_LOCAL}/lib/x64"
fi

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
    mingw-w64-x86_64-clang \
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

# ─── Step 2b: Generate MinGW import libraries for CUDA/NPP DLLs ───────────

if [ -n "${CUDA_PATH}" ]; then
    echo ""
    echo "=== Generating MinGW import libraries for CUDA NPP ==="
    CUDA_LIB_MINGW="build_cuda_lib"
    mkdir -p "${CUDA_LIB_MINGW}"
    for dll in "${CUDA_LOCAL}/bin"/npp*.dll; do
        [ -f "${dll}" ] || continue
        name="$(basename "${dll}" .dll)"
        # Strip the 64_XX suffix: nppc64_12 -> nppc
        short="$(echo "${name}" | sed 's/64_[0-9]*//')"
        outlib="${CUDA_LIB_MINGW}/lib${short}.a"
        if [ ! -f "${outlib}" ]; then
            deffile="${CUDA_LIB_MINGW}/${short}.def"
            echo "  ${name}.dll -> lib${short}.a"
            gendef - "${dll}" > "${deffile}" 2>/dev/null
            dlltool -d "${deffile}" -l "${outlib}" -D "$(basename "${dll}")"
        fi
    done
fi

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
if [ -n "${CUDA_PATH}" ]; then
    echo "  CUDA Toolkit:        ${CUDA_PATH}"
else
    echo "  CUDA Toolkit:        not found (libnpp disabled)"
fi
echo ""

# Build CUDA/NPP flags if toolkit is available
CUDA_CFLAGS=""
CUDA_LDFLAGS=""
CUDA_CONFIGURE=""
if [ -n "${CUDA_PATH}" ]; then
    CUDA_CFLAGS="-I${CUDA_INCLUDE}"
    CUDA_LDFLAGS="-L${CUDA_LIB_MINGW} -L${CUDA_LIB}"
    CUDA_CONFIGURE="--enable-libnpp"
fi

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
    --disable-sdl2 \
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
    --enable-cuda-llvm \
    ${CUDA_CONFIGURE} \
    \
    --enable-decklink \
    --enable-videomaster \
    --extra-cflags="-I${VM_INCLUDE} -I${DL_INCLUDE} ${CUDA_CFLAGS} -Wno-error=incompatible-pointer-types" \
    --extra-cxxflags="-I${VM_INCLUDE} -I${DL_INCLUDE} -fext-numeric-literals" \
    --extra-ldflags="-L${VM_LIB} ${CUDA_LDFLAGS}" \
    --extra-libs="-lstdc++ -lole32 -loleaut32 -luuid -lshlwapi" \
    \
    "$@"

# ─── Step 4b: Patch config.mak for fully static build ──────────────────────
#
# We cannot pass -static during configure because it breaks library detection
# (configure tests use LDEXEFLAGS too, and -static conflicts with -DX264_API_IMPORTS).
# Instead, we inject the static flags into config.mak after configure completes.
#
# 1. Add -static -static-libgcc -static-libstdc++ to LDEXEFLAGS
# 2. Replace -lgcc_s (shared libgcc) with -lgcc (static) — comes from x265 pkg-config
# 3. Remove -DX264_API_IMPORTS — configure sets it for DLL usage, but static x264 has
#    non-prefixed symbols (without __imp_), so this define must be removed

echo ""
echo "=== Patching config.mak for fully static exe ==="
sed -i 's/^LDEXEFLAGS=/LDEXEFLAGS=-static -static-libgcc -static-libstdc++ /' ffbuild/config.mak
sed -i 's/-lgcc_s/-lgcc/g' ffbuild/config.mak
sed -i 's/-DX264_API_IMPORTS//g' ffbuild/config.mak

# ─── Step 5: Build ───────────────────────────────────────────────────────────

echo ""
echo "=== Building FFmpeg (${JOBS} jobs) ==="
make -j"${JOBS}"

# ─── Step 6: Strip binaries for minimal size ─────────────────────────────────

echo ""
echo "=== Stripping binaries ==="
strip -s ffmpeg.exe 2>/dev/null && echo "  Stripped ffmpeg.exe" || true
strip -s ffprobe.exe 2>/dev/null && echo "  Stripped ffprobe.exe" || true

# ─── Step 7: Copy runtime DLLs ────────────────────────────────────────────
#
# NPP libraries are proprietary NVIDIA DLLs — no static version exists.
# Copy them next to ffmpeg.exe so it can find them at runtime.

if [ -n "${CUDA_PATH}" ]; then
    echo ""
    echo "=== Copying CUDA NPP DLLs ==="
    for dll in "${CUDA_LOCAL}/bin"/npp*.dll "${CUDA_LOCAL}/bin"/cudart*.dll; do
        [ -f "${dll}" ] || continue
        cp -u "${dll}" . && echo "  Copied $(basename "${dll}")"
    done
fi

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
