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

echo "========================================================"
echo " CarPlay Voice Activation — Android arm64-v8a build"
echo "========================================================"
echo " NDK:          ${ANDROID_NDK_HOME}"
echo " API level:    ${ANDROID_API_LEVEL}"
echo " Build dir:    ${BUILD_DIR}"
echo " Install dir:  ${INSTALL_DIR}"
echo "========================================================"

# Ensure the submodule is populated.
if [ ! -f "${PROJECT_ROOT}/third_party/AndroidAudioProcessModule/CMakeLists.txt" ]; then
    echo "==> Initialising git submodules..."
    git -C "${PROJECT_ROOT}" submodule update --init --recursive
fi

echo "==> CMake configure..."
cmake -S "${PROJECT_ROOT}" \
      -B "${BUILD_DIR}" \
      -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
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
