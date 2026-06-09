#!/bin/sh
# ---------------------------------------------------------------------------
# build_android_arm64.sh
#
# Cross-compile libcarplay_voice_activation for Android arm64-v8a.
# audio3a (3A front-end) is built from source via the git submodule at
#   third_party/AndroidAudioProcessModule
#
# Prerequisites:
#   ANDROID_NDK_HOME  – path to Android NDK root (r27+ recommended)
#   ANDROID_API_LEVEL – optional, defaults to 26 (Android 8.0)
#
# Quick start:
#   export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/27.x.y
#   bash cmake/build_android_arm64.sh
# ---------------------------------------------------------------------------

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

: "${ANDROID_NDK_HOME:?Please set ANDROID_NDK_HOME to your NDK root directory}"
: "${ANDROID_API_LEVEL:=26}"

BUILD_DIR="${PROJECT_ROOT}/build/android-arm64-v8a"
INSTALL_DIR="${BUILD_DIR}/install"
TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake"
NDK_PREBUILT="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64"

validate_ndk() {
    if [ ! -d "${ANDROID_NDK_HOME}" ]; then
        echo "ERROR: ANDROID_NDK_HOME does not exist: ${ANDROID_NDK_HOME}" >&2
        echo "" >&2
        echo "WSL tip: if the NDK is on Windows drive G:, mount it first:" >&2
        echo "  sudo mkdir -p /mnt/g" >&2
        echo "  sudo mount -t drvfs G: /mnt/g" >&2
        echo "  export ANDROID_NDK_HOME=/mnt/g/android-ndk-r27d" >&2
        echo "" >&2
        echo "Use the Linux NDK package (prebuilt/linux-x86_64), not the Windows-only zip." >&2
        exit 1
    fi

    if [ ! -f "${TOOLCHAIN_FILE}" ]; then
        echo "ERROR: NDK toolchain file not found:" >&2
        echo "  ${TOOLCHAIN_FILE}" >&2
        echo "Check that ANDROID_NDK_HOME points to the NDK root (contains build/cmake/)." >&2
        exit 1
    fi

    if [ ! -d "${NDK_PREBUILT}" ]; then
        echo "ERROR: Linux NDK host tools not found:" >&2
        echo "  ${NDK_PREBUILT}" >&2
        echo "Download the Linux NDK (linux-x86_64 prebuilt), not android-ndk-*-windows.zip." >&2
        exit 1
    fi
}

if command -v ninja >/dev/null 2>&1; then
    CMAKE_GENERATOR="-G Ninja"
else
    CMAKE_GENERATOR=""
    echo "WARNING: ninja not found; falling back to default generator." >&2
fi

validate_ndk

echo "========================================================"
echo " CarPlay Voice Activation — Android arm64-v8a build"
echo "========================================================"
echo " NDK:          ${ANDROID_NDK_HOME}"
echo " API level:    ${ANDROID_API_LEVEL}"
echo " Build dir:    ${BUILD_DIR}"
echo " Install dir:  ${INSTALL_DIR}"
echo " Generator:    ${CMAKE_GENERATOR:-default}"
echo "========================================================"

# Ensure the submodule is populated.
if [ ! -f "${PROJECT_ROOT}/third_party/AndroidAudioProcessModule/CMakeLists.txt" ]; then
    echo "==> Initialising git submodules..."
    git -C "${PROJECT_ROOT}" submodule update --init --recursive
fi

echo "==> CMake configure..."
cmake -S "${PROJECT_ROOT}" \
      -B "${BUILD_DIR}" \
      ${CMAKE_GENERATOR} \
      -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
      -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-${ANDROID_API_LEVEL} \
      -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

echo "==> Build..."
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo "==> Install..."
cmake --install "${BUILD_DIR}"

echo ""
echo "========================================================"
echo " Done. Output files:"
echo "========================================================"
find "${INSTALL_DIR}" -type f | sort
