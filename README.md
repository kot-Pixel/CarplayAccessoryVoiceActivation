# CarPlay Accessory Voice Activation Library

Pure-C library (`libcarplay_voice_activation`) that provides the API surface required by a CarPlay accessory to support Siri voice activation, as specified in the Apple CarPlay Accessory Interface Specification (§ 3.3.7.2).

The library bundles a complete audio processing pipeline:

| Stage | Library | Description |
|-------|---------|-------------|
| Front-end 3A | `libaudio3a` | High-pass filter, noise suppression, acoustic echo cancellation (10 ms frames) |
| Keyword Spotting | `libhey_siri_kws` | "Hey Siri" detection on 20 ms frames via TensorFlow Lite |
| Voice Activity Detection | `libten_vad` | Speech onset / offset detection |

---

## Supported detection modes

| Mode | Enum | Description |
|------|------|-------------|
| Keyword Detection | `CPVA_MODE_KEYWORD_DETECTION` | Detect the "Hey Siri" keyword in uplink audio |
| Voice Activity Detection | `CPVA_MODE_VOICE_ACTIVITY_DETECTION` | Detect Start-of-Speech (SoS) events |
| Deactivated | `CPVA_MODE_DEACTIVATED` | Voice activation disabled |

---

## Repository layout

```
CarplayAccessoryVoiceActivation/
├── CMakeLists.txt
├── cmake/
│   └── build_android_arm64.sh          # NDK cross-compile helper script
├── native/
│   ├── include/
│   │   └── carplay_voice_activation.h  # Public C API (only file callers need)
│   └── src/
│       └── carplay_voice_activation.c  # Implementation
└── third_party/
    ├── AndroidAudioProcessModule/      # 3A (git submodule, built from source)
    ├── TenVad/                         # VAD pre-built .so + header
    └── hey_siri_kws/                   # KWS pre-built .so + header
```

---

## Building for Android arm64-v8a

### Prerequisites

- Android NDK r25c or later
- CMake ≥ 3.22
- Ninja (`apt install ninja-build` on Ubuntu/WSL)
- Git (submodules must be initialised)

### Quick start (WSL / Linux)

```sh
export ANDROID_NDK_HOME=/home/wdf/android-ndk-r28b
export ANDROID_API_LEVEL=26          # Android 8.0+

cd cmake
sh build_android_arm64.sh
```

### Manual CMake invocation

```sh
cmake -S . -B build/android-arm64-v8a \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=build/android-arm64-v8a/install

cmake --build build/android-arm64-v8a --parallel
cmake --install build/android-arm64-v8a
```

### Build output

```
build/android-arm64-v8a/install/
├── include/
│   └── carplay_voice_activation.h
└── lib/
    ├── libcarplay_voice_activation.so   ← main library (link this)
    ├── libcarplay_voice_activation.a    ← static variant
    ├── libaudio3a.so                    ← 3A runtime dep
    ├── libten_vad.so                    ← VAD runtime dep
    └── libhey_siri_kws.so               ← KWS (TFLite statically linked inside)
```

---

## Third-party integration

### 1 — Copy the install tree into your Android project

Place every `.so` from `install/lib/` and the header from `install/include/` into your project:

```
your-android-project/
├── app/src/main/
│   ├── cpp/
│   │   └── carplay_voice_activation.h      ← copy from install/include/
│   └── jniLibs/arm64-v8a/
│       ├── libcarplay_voice_activation.so
│       ├── libaudio3a.so
│       ├── libten_vad.so
│       └── libhey_siri_kws.so              ← TFLite statically linked inside
```

All four `.so` files must be present. `libhey_siri_kws.so` has TFLite statically linked inside — no separate TFLite or libc++ runtime files are needed.

---

### 2 — Link the library

#### Option A — CMake (`CMakeLists.txt`)

```cmake
cmake_minimum_required(VERSION 3.22)
project(YourCarPlayApp)

# Directory where the install/lib and install/include trees live
set(CPVA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/carplay_voice_activation")

add_library(carplay_voice_activation SHARED IMPORTED)
set_target_properties(carplay_voice_activation PROPERTIES
    IMPORTED_LOCATION "${CPVA_ROOT}/lib/libcarplay_voice_activation.so"
    INTERFACE_INCLUDE_DIRECTORIES "${CPVA_ROOT}/include"
)

add_library(your_jni_lib SHARED your_jni.c)
target_link_libraries(your_jni_lib
    PRIVATE carplay_voice_activation
            log
)
```

#### Option B — Android.mk

```makefile
LOCAL_PATH := $(call my-dir)
CPVA_ROOT  := $(LOCAL_PATH)/carplay_voice_activation

include $(CLEAR_VARS)
LOCAL_MODULE           := carplay_voice_activation
LOCAL_SRC_FILES        := $(CPVA_ROOT)/lib/libcarplay_voice_activation.so
LOCAL_EXPORT_C_INCLUDES := $(CPVA_ROOT)/include
include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE        := your_jni_lib
LOCAL_SRC_FILES     := your_jni.c
LOCAL_SHARED_LIBRARIES := carplay_voice_activation
include $(BUILD_SHARED_LIBRARY)
```

---

### 3 — API usage

#### 3.1 Minimal example — KWS mode

```c
#include "carplay_voice_activation.h"
#include <stdio.h>

/* Called when "Hey Siri" is detected */
static void on_request_siri(cpva_siri_action_t action,
                             int64_t            siri_trigger_ts_us,
                             void              *user_data)
{
    /* Forward requestSiri(voiceActivation, siriTriggerTimestamp) to CarPlay device.
     * siri_trigger_ts_us is the µs timestamp of the start of "Siri" phoneme,
     * using the same clock as the timestamps passed to cpva_feed_audio(). */
    printf("requestSiri ts=%lld µs\n", (long long)siri_trigger_ts_us);
}

/* Optional — receive per-frame KWS score for logging / threshold tuning */
static void on_kws_score(float score, int triggered,
                         int64_t ts_us, void *user_data)
{
    if (triggered)
        printf("KWS triggered: score=%.3f ts=%lld\n", score, (long long)ts_us);
}

int main(void)
{
    /* Audio format: 16 kHz mono 16-bit PCM (required by all engines) */
    cpva_audio_format_t fmt = {
        .sample_rate_hz = 16000,
        .channels       = 1,
        .bits_per_sample = 16,
    };

    /* 3A front-end configuration (all fields optional — zeros = defaults) */
    cpva_3a_config_t a3 = {
        .enable_hpf       = 1,
        .enable_ns        = 1,
        .ns_level         = CPVA_NS_LEVEL_MODERATE,
        .enable_aec       = 1,
        .aec_delay_ms     = 0,   /* auto-detect */
    };

    /* KWS configuration (all fields optional — zeros = defaults) */
    cpva_kws_config_t kws = {
        .threshold                = 0.70f,  /* detection sensitivity */
        .keyword_to_siri_onset_ms = 400,    /* timing offset for siriTriggerTimestamp */
        .cooldown_ms              = 1500,   /* minimum gap between triggers */
    };

    /* Callbacks */
    cpva_callbacks_t cbs = {
        .on_request_siri = on_request_siri,
        .on_kws_score    = on_kws_score,    /* optional */
    };

    /* Init */
    cpva_context_t *ctx = NULL;
    cpva_error_t err = cpva_init(&ctx, &fmt, &a3, &kws, /*vad_config=*/NULL,
                                 &cbs, /*user_data=*/NULL);
    if (err != CPVA_OK) { /* handle error */ return 1; }

    /* Set mode from CarPlay SET_PARAMETER(voiceActivationMode) */
    cpva_set_voice_activation_mode(ctx, CPVA_MODE_KEYWORD_DETECTION);

    /* Feed microphone PCM in real time.
     * frame_count can be any size; internal buffers handle chunking.
     * timestamp_us must be the capture time of the FIRST sample in the buffer,
     * using a monotonic clock consistent across all calls. */
    int16_t pcm_buf[160]; /* e.g. 10 ms at 16 kHz */
    int64_t timestamp_us = 0;
    while (/* audio loop */ 0) {
        /* ... fill pcm_buf from microphone ... */
        cpva_feed_audio(ctx, pcm_buf, 160, timestamp_us);
        timestamp_us += 10000; /* 10 ms */
    }

    /* Teardown */
    cpva_destroy(ctx);
    return 0;
}
```

#### 3.2 AEC — feeding speaker reference audio

When AEC is enabled, provide the far-end (speaker) signal so the library can cancel echo:

```c
/* Notify the library that speaker playback has started / stopped */
cpva_notify_speaker_active(ctx, 1 /*active*/);

/* Feed speaker PCM in parallel with microphone audio.
 * Must use the same sample rate and format as cpva_feed_audio(). */
cpva_feed_speaker_audio(ctx, speaker_pcm, sample_count, speaker_ts_us);

/* Adjust AEC reference delay at runtime if needed */
cpva_set_aec_delay_ms(ctx, 50);
```

#### 3.3 VAD mode

```c
/* VAD configuration */
cpva_vad_config_t vad = {
    .speech_confirm_ms    = 300,   /* confirm speech after 300 ms */
    .speech_gap_ms        = 400,   /* allow 400 ms gap before ending */
    .vad_frame_ms         = 20,    /* VAD hop size */
};

/* Receive VAD state transitions */
static void on_vad_state_changed(cpva_vad_state_t new_state,
                                 int64_t          ts_us,
                                 void            *user_data)
{
    switch (new_state) {
        case CPVA_VAD_STATE_SPEECH_DETECTED:
            /* Open auxiliary audio stream to device */
            break;
        case CPVA_VAD_STATE_SILENCE:
            /* Close auxiliary audio stream */
            break;
        default: break;
    }
}

cpva_callbacks_t cbs = {
    .on_request_siri      = on_request_siri,
    .on_vad_state_changed = on_vad_state_changed,  /* optional */
};

cpva_context_t *ctx = NULL;
cpva_init(&ctx, &fmt, &a3, /*kws_config=*/NULL, &vad, &cbs, NULL);
cpva_set_voice_activation_mode(ctx, CPVA_MODE_VOICE_ACTIVITY_DETECTION);
```

#### 3.4 Reflecting device-side state changes

```c
/* Called when CarPlay device sends modesChanged with appState(speech) */
cpva_notify_app_state_speech(ctx, CPVA_APP_STATE_SPEECH_RECOGNIZING);
/* ... when Siri finishes ... */
cpva_notify_app_state_speech(ctx, CPVA_APP_STATE_SPEECH_IDLE);

/* Called when auxiliary audio stream opens / closes */
cpva_notify_auxiliary_audio_stream_active(ctx, 1 /*active*/);
cpva_notify_auxiliary_audio_stream_active(ctx, 0 /*inactive*/);
```

While `appState(speech) == Recognizing` **or** the auxiliary audio stream is active, the library automatically suppresses `on_request_siri` to prevent double-triggers.

#### 3.5 Advertising capabilities in the CarPlay info message

```c
cpva_info_t info;
cpva_get_info(ctx, &info);
/* info.active_language   — currently active language tag */
/* info.supported_languages / info.supported_language_count — full list */
```

---

## Key API reference

### Lifecycle

| Function | Description |
|----------|-------------|
| `cpva_init` | Create a context. Pass NULL for any config struct to use defaults. |
| `cpva_destroy` | Free all resources. Safe to call with NULL. |

### Configuration & mode control

| Function | Description |
|----------|-------------|
| `cpva_set_voice_activation_mode` | Switch KWS / VAD / Deactivated at any time |
| `cpva_set_voice_model_language` | Change keyword-detector language |
| `cpva_set_aec_delay_ms` | Adjust AEC reference delay at runtime |

### Audio input

| Function | Description |
|----------|-------------|
| `cpva_feed_audio` | Push microphone PCM (any frame size) |
| `cpva_feed_speaker_audio` | Push speaker reference PCM for AEC |
| `cpva_notify_speaker_active` | Signal speaker playback start/stop |

### State notifications from device

| Function | Description |
|----------|-------------|
| `cpva_notify_app_state_speech` | Reflect Siri recognizing / idle |
| `cpva_notify_auxiliary_audio_stream_active` | Reflect auxiliary stream state |

### Queries

| Function | Description |
|----------|-------------|
| `cpva_get_voice_activation_mode` | Current mode |
| `cpva_get_info` | Active + supported languages |

### Callbacks

| Callback | When fired |
|----------|-----------|
| `on_request_siri` | Keyword or VAD trigger detected; send `requestSiri(voiceActivation, siriTriggerTimestamp)` |
| `on_kws_score` | Every 20 ms in KWS mode (score + triggered flag) — useful for tuning |
| `on_vad_state_changed` | VAD state-machine transition — use to open/close auxiliary stream |

---

## Spec compliance

| Requirement | How it is met |
|-------------|---------------|
| `requestSiri` blocked while `appState(speech) == Recognizing` | `cpva_notify_app_state_speech` + internal gate |
| `requestSiri` blocked while auxiliary audio stream is active | `cpva_notify_auxiliary_audio_stream_active` + internal gate |
| Mode changeable at any time during session | `cpva_set_voice_activation_mode` is re-entrant |
| Language update via `SET_PARAMETER(voiceModelLanguage)` | `cpva_set_voice_model_language` |
| Active + supported languages in info message | `cpva_get_info` |
| `siriTriggerTimestamp` = start of trigger, not end | `keyword_to_siri_onset_ms` offset back-computed from detection timestamp |
| VAD confirms 300 ms of continuous speech before trigger | Configurable via `cpva_vad_config_t.speech_confirm_ms` |
