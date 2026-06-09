cmake_minimum_required(VERSION 3.22)

project(carplay_voice_activation
    VERSION 1.0.0
    DESCRIPTION "CarPlay Accessory Voice Activation Library"
    LANGUAGES C CXX   # CXX needed for the audio3a submodule
)

# ---------------------------------------------------------------------------
# Architecture guard
# ---------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        message(FATAL_ERROR
            "This library targets Android arm64-v8a. "
            "Pass -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a to CMake.")
    endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# audio3a submodule (AndroidAudioProcessModule)
#
# The submodule's CMakeLists.txt forces CMAKE_INSTALL_PREFIX to its own
# dist/ directory.  We save and restore our prefix around add_subdirectory.
# ---------------------------------------------------------------------------

set(_saved_install_prefix "${CMAKE_INSTALL_PREFIX}")

# The submodule's CMakeLists.txt references "WebRtcApm/" (capital W) but the
# directory on disk is "webRtcApm/" (lower w).  On case-sensitive Linux hosts
# (e.g. CI) CMake would fail to find the .a file.  Create a compatibility
# symlink when needed (no-op on Windows and macOS which are case-insensitive).
if(CMAKE_HOST_UNIX)
    set(_apm_sub "${CMAKE_CURRENT_SOURCE_DIR}/third_party/AndroidAudioProcessModule")
    if(IS_DIRECTORY "${_apm_sub}/webRtcApm" AND
       NOT IS_DIRECTORY "${_apm_sub}/WebRtcApm")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink webRtcApm WebRtcApm
            WORKING_DIRECTORY "${_apm_sub}"
        )
    endif()
endif()

add_subdirectory(
    third_party/AndroidAudioProcessModule
    third_party/AndroidAudioProcessModule/build
    EXCLUDE_FROM_ALL   # don't build unless required by our targets
)

# Restore our install prefix after the submodule has overridden it.
set(CMAKE_INSTALL_PREFIX "${_saved_install_prefix}" CACHE PATH
    "Install prefix for carplay_voice_activation" FORCE)

# Targets provided by the submodule:
#   audio3a_shared  →  libaudio3a.so  (WebRTC APM statically linked in)
#   audio3a_static  →  libaudio3a.a   (thin wrapper; needs webrtc_apm too)
#   webrtc_apm      →  libwebrtc_audio_processing.a  (IMPORTED STATIC)

# ---------------------------------------------------------------------------
# hey_siri_kws (pre-built shared library + TFLite runtime)
#
# Uses the pre-built dist/arm64-v8a/ outputs committed in the submodule.
# ---------------------------------------------------------------------------

set(KWS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hey_siri_kws")
set(KWS_DIST "${KWS_DIR}/dist/arm64-v8a")

if(NOT EXISTS "${KWS_DIST}/include/hey_siri_kws.h")
    message(FATAL_ERROR "hey_siri_kws.h not found. Run: git submodule update --init")
endif()

set(KWS_SO "${KWS_DIST}/lib/libhey_siri_kws.so")
set(KWS_TFLITE_SO  "${KWS_DIST}/runtime/libtensorflowlite_jni.so")
set(KWS_LIBCXX_SO  "${KWS_DIST}/runtime/libc++_shared.so")

add_library(hey_siri_kws SHARED IMPORTED GLOBAL)
set_target_properties(hey_siri_kws PROPERTIES
    IMPORTED_LOCATION             "${KWS_SO}"
    INTERFACE_INCLUDE_DIRECTORIES "${KWS_DIST}/include"
)

# TFLite and libc++_shared are runtime-only deps (loaded by libhey_siri_kws.so
# at process startup); we don't link them at CMake level but install them so
# the loader can find them.

# ---------------------------------------------------------------------------
# TenVad (pre-built shared library)
#
# Located at third_party/TenVad/ inside this repository.
# ---------------------------------------------------------------------------

set(TENVAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/TenVad")

if(NOT EXISTS "${TENVAD_DIR}/include/ten_vad.h")
    message(FATAL_ERROR "ten_vad.h not found under ${TENVAD_DIR}/include")
endif()

set(TENVAD_SO "${TENVAD_DIR}/lib/libten_vad.so")
if(NOT EXISTS "${TENVAD_SO}")
    message(FATAL_ERROR "libten_vad.so not found at ${TENVAD_SO}")
endif()

add_library(ten_vad SHARED IMPORTED GLOBAL)
set_target_properties(ten_vad PROPERTIES
    IMPORTED_LOCATION             "${TENVAD_SO}"
    INTERFACE_INCLUDE_DIRECTORIES "${TENVAD_DIR}/include"
)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

set(CPVA_SOURCES
    native/src/carplay_voice_activation.c
)

set(CPVA_HEADERS
    native/include/carplay_voice_activation.h
)

# ---------------------------------------------------------------------------
# Shared library  →  libcarplay_voice_activation.so
# ---------------------------------------------------------------------------

add_library(carplay_voice_activation SHARED
    ${CPVA_SOURCES}
    ${CPVA_HEADERS}
)

target_include_directories(carplay_voice_activation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/native/include>
        $<INSTALL_INTERFACE:include>
    # audio3a_shared's PUBLIC include dirs (audio3a.h) propagate transitively
)

target_compile_options(carplay_voice_activation PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fvisibility=hidden
)

# Link audio3a_shared: its PUBLIC include dirs (audio3a.h) are automatically
# consumed by our target so we do not need an explicit include_directories.
target_link_libraries(carplay_voice_activation
    PRIVATE
        audio3a_shared
        ten_vad
        hey_siri_kws
        $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Android>:log>
)

# ---------------------------------------------------------------------------
# Static library  →  libcarplay_voice_activation.a
# ---------------------------------------------------------------------------

add_library(carplay_voice_activation_static STATIC
    ${CPVA_SOURCES}
    ${CPVA_HEADERS}
)

set_target_properties(carplay_voice_activation_static PROPERTIES
    OUTPUT_NAME carplay_voice_activation
)

target_include_directories(carplay_voice_activation_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/native/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(carplay_voice_activation_static PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Static variant: consumer must also link libaudio3a.a + libwebrtc_audio_processing.a
target_link_libraries(carplay_voice_activation_static
    PUBLIC
        audio3a_static
        webrtc_apm
        ten_vad
        hey_siri_kws
        $<$<STREQUAL:${CMAKE_SYSTEM_NAME},Android>:log>
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS carplay_voice_activation carplay_voice_activation_static
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES ${CPVA_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Bundle all runtime .so files so the final app only needs one lib directory.
install(FILES
    $<TARGET_FILE:audio3a_shared>
    "${TENVAD_SO}"
    "${KWS_SO}"
    "${KWS_TFLITE_SO}"
    "${KWS_LIBCXX_SO}"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# CMake package-config export is intentionally omitted: the dependency chain
# contains IMPORTED pre-built shared libraries (audio3a, ten_vad, hey_siri_kws)
# that cannot be placed in a CMake export set.  Consumers should link directly
# to the installed .so files.
