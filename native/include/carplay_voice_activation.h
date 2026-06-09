/**
 * carplay_voice_activation.h
 *
 * CarPlay Accessory Voice Activation Library — Public C API
 *
 * Audio processing pipeline:
 *
 *   Mic PCM  ──► cpva_feed_audio()
 *                     │
 *                     ▼
 *              [ 3A front-end ]   ← HPF + NS always on
 *              [ AEC (dynamic)]   ← enabled when speaker is playing
 *                     │
 *                     ▼
 *              [ VAD / KWS  ]     ← mode-dependent detection
 *                     │
 *                     ▼
 *             on_request_siri()  ─► requestSiri(voiceActivation, siriTriggerTimestamp)
 *
 *   Speaker PCM ──► cpva_feed_speaker_audio()  ──► AEC far-end reference
 *
 * Usage flow:
 *   1. Implement cpva_callbacks_t.
 *   2. Call cpva_init() at CarPlay session start.
 *   3. Apply SET_PARAMETER values via cpva_set_voice_activation_mode() /
 *      cpva_set_voice_model_language().
 *   4. Feed microphone PCM via cpva_feed_audio().
 *      Feed speaker PCM via cpva_feed_speaker_audio() + cpva_notify_speaker_active().
 *   5. Mirror device-side state via cpva_notify_app_state_speech() and
 *      cpva_notify_auxiliary_audio_stream_active().
 *   6. Call cpva_destroy() when the session ends.
 */

#ifndef CARPLAY_VOICE_ACTIVATION_H
#define CARPLAY_VOICE_ACTIVATION_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CPVA_API __attribute__((visibility("default")))
#else
#define CPVA_API
#endif

/* -------------------------------------------------------------------------
 * Version
 * ---------------------------------------------------------------------- */

#define CPVA_VERSION_MAJOR 1
#define CPVA_VERSION_MINOR 0
#define CPVA_VERSION_PATCH 0

/* -------------------------------------------------------------------------
 * Opaque handle
 * ---------------------------------------------------------------------- */

typedef struct cpva_context cpva_context_t;

/* -------------------------------------------------------------------------
 * Enumerations
 * ---------------------------------------------------------------------- */

/**
 * Voice activation operating mode.
 * Configured by the device via SET_PARAMETER(voiceActivationMode).
 */
typedef enum {
    CPVA_MODE_DEACTIVATED              = 0,
    CPVA_MODE_KEYWORD_DETECTION        = 1,
    CPVA_MODE_VOICE_ACTIVITY_DETECTION = 2
} cpva_voice_activation_mode_t;

/**
 * Action carried in a requestSiri command.
 */
typedef enum {
    CPVA_SIRI_ACTION_VOICE_ACTIVATION = 0
} cpva_siri_action_t;

/**
 * Device-side speech app-state (appState: speech).
 * The accessory must NOT send requestSiri(voiceActivation) while the state
 * is CPVA_APP_STATE_SPEECH_RECOGNIZING.
 */
typedef enum {
    CPVA_APP_STATE_SPEECH_IDLE        = 0,
    CPVA_APP_STATE_SPEECH_RECOGNIZING = 1
} cpva_app_state_speech_t;

/**
 * VAD internal state, reported via on_vad_state_changed().
 *
 *   SILENCE  ──(speech onset)──► ONSET
 *   ONSET    ──(300 ms speech)──► ACTIVE   → on_request_siri fired
 *   ONSET    ──(non-speech)────► SILENCE   (false alarm, no trigger)
 *   ACTIVE   ──(non-speech)────► GAP
 *   GAP      ──(speech resumes, gap < 400 ms)──► ACTIVE  (same region)
 *   GAP      ──(gap ≥ 400 ms)────────────────► SILENCE
 */
typedef enum {
    CPVA_VAD_STATE_SILENCE = 0, /**< No speech detected; waiting for onset.       */
    CPVA_VAD_STATE_ONSET,       /**< Speech onset; confirming 300 ms continuity.  */
    CPVA_VAD_STATE_ACTIVE,      /**< 300 ms confirmed; requestSiri already sent.  */
    CPVA_VAD_STATE_GAP,         /**< Brief non-speech pause after ACTIVE (≤400ms) */
} cpva_vad_state_t;

/**
 * Noise suppression aggressiveness for the 3A front-end.
 * Maps directly to Audio3aNsLevel in audio3a.h.
 */
typedef enum {
    CPVA_NS_LEVEL_LOW       = 0,
    CPVA_NS_LEVEL_MODERATE  = 1,
    CPVA_NS_LEVEL_HIGH      = 2,
    CPVA_NS_LEVEL_VERY_HIGH = 3
} cpva_ns_level_t;

/**
 * Error / return codes.
 */
typedef enum {
    CPVA_OK                     =  0,
    CPVA_ERR_INVALID_ARG        = -1,
    CPVA_ERR_INVALID_STATE      = -2,
    CPVA_ERR_NOT_INITIALIZED    = -3,
    CPVA_ERR_UNSUPPORTED_LANG   = -4,
    CPVA_ERR_INTERNAL           = -5
} cpva_error_t;

/* -------------------------------------------------------------------------
 * Audio format descriptor
 * ---------------------------------------------------------------------- */

/**
 * PCM audio format.
 * Both microphone (cpva_feed_audio) and speaker (cpva_feed_speaker_audio)
 * must use the same sample_rate_hz.
 * Supported rates: 8000 / 16000 / 32000 / 48000 Hz.
 * channels must be 1 (mono), bits_per_sample must be 16.
 */
typedef struct {
    uint32_t sample_rate_hz;
    uint8_t  channels;
    uint8_t  bits_per_sample;
} cpva_audio_format_t;

/* -------------------------------------------------------------------------
 * KWS configuration  (Keyword Spotting / hey_siri_kws)
 * ---------------------------------------------------------------------- */

/**
 * Configuration for the hey_siri_kws engine used in
 * CPVA_MODE_KEYWORD_DETECTION.
 * Zero-initialise and fill only what you need; unset fields use defaults.
 */
typedef struct {
    /**
     * KWS_LABEL_HEY_SIRI score threshold in [0.0, 1.0].
     * Detection fires when kws_get_label_score(KWS_LABEL_HEY_SIRI) >= threshold.
     * Higher value → fewer false accepts, higher miss rate.
     * Default (0.0f) → 0.70f.
     */
    float threshold;

    /**
     * Estimated duration (ms) from the start of the 'S' phoneme in "Siri"
     * to the moment the KWS engine completes detection of "Hey Siri".
     * Used to back-compute siriTriggerTimestamp from the detection timestamp:
     *   siriTriggerTimestamp = detection_ts - keyword_to_siri_onset_ms * 1000
     * Tune this value to match the engine's actual detection latency.
     * Default (0) → 400 ms.
     */
    int keyword_to_siri_onset_ms;

    /**
     * Minimum gap (ms) between two successive requestSiri triggers.
     * Prevents double-fires on the same utterance when the engine score
     * stays high across multiple consecutive frames.
     * Default (0) → 1500 ms.
     */
    int cooldown_ms;
} cpva_kws_config_t;

/* -------------------------------------------------------------------------
 * VAD configuration  (Voice Activity Detection / TenVad)
 * ---------------------------------------------------------------------- */

/**
 * Configuration for the TenVad engine used in CPVA_MODE_VOICE_ACTIVITY_DETECTION.
 * Zero-initialise and fill only what you need; unset fields use defaults.
 */
typedef struct {
    /**
     * Number of int16 samples per VAD analysis hop.
     * Must match the ten_vad_create() hop_size parameter.
     * Typical values: 160 (10 ms @ 16 kHz), 256 (~16 ms @ 16 kHz).
     * Default (0) → sample_rate_hz / 100  (10 ms, same as the 3A frame size).
     */
    size_t hop_size;

    /**
     * VAD decision threshold in [0.0, 1.0].
     * Voice is flagged when the engine's output probability >= threshold.
     * Higher value → fewer false wakes, higher miss rate.
     * Default (0.0f) → 0.5f.
     */
    float threshold;
} cpva_vad_config_t;

/* -------------------------------------------------------------------------
 * 3A front-end configuration
 * ---------------------------------------------------------------------- */

/**
 * Configuration for the audio 3A front-end (HPF + NS + AEC).
 * Zero-initialise and fill only what you need; unset fields use defaults.
 */
typedef struct {
    /**
     * Render-to-capture round-trip delay estimate in ms.
     * Should reflect the latency from audio3a_process_render() to when that
     * audio is picked up by the microphone.
     * Default: 100 ms.
     */
    int stream_delay_ms;

    /**
     * Noise suppression aggressiveness. Default (0) → CPVA_NS_LEVEL_VERY_HIGH.
     */
    cpva_ns_level_t ns_level;

    /**
     * Set to 1 if AEC should be active immediately after cpva_init().
     * If the speaker is already playing at session start, set this to 1 and
     * call cpva_feed_speaker_audio() right away.
     * Default: 0.
     */
    int aec_active_on_start;
} cpva_3a_config_t;

/* -------------------------------------------------------------------------
 * Language descriptor
 * ---------------------------------------------------------------------- */

#define CPVA_LANGUAGE_TAG_MAX_LEN 16

typedef struct {
    char tag[CPVA_LANGUAGE_TAG_MAX_LEN];
} cpva_language_tag_t;

/* -------------------------------------------------------------------------
 * Callbacks
 * ---------------------------------------------------------------------- */

/**
 * Called when a voice trigger is detected and a requestSiri(voiceActivation)
 * command should be forwarded to the device.
 *
 * @param action                      Always CPVA_SIRI_ACTION_VOICE_ACTIVATION.
 * @param siri_trigger_timestamp_us   Monotonic microseconds of the trigger start:
 *                                    - Keyword Detection: first phoneme ('S').
 *                                    - VAD: Start-of-Speech (SoS) event.
 * @param user_data                   Opaque pointer supplied at cpva_init().
 */
typedef void (*cpva_on_request_siri_fn)(
    cpva_siri_action_t action,
    int64_t            siri_trigger_timestamp_us,
    void              *user_data
);

/**
 * Called on every KWS inference frame (every 20 ms in KWS mode).
 *
 * Provides the raw hey-siri score so the host can display a confidence
 * meter, log detections, or tune the threshold at runtime.
 *
 * @param hey_siri_score  kws_get_label_score(KWS_LABEL_HEY_SIRI) [0.0, 1.0].
 * @param triggered       1 if this frame crossed the threshold and
 *                        on_request_siri() was (or will be) called.
 * @param timestamp_us    Monotonic timestamp (µs) of the KWS frame start.
 * @param user_data       Opaque pointer supplied at cpva_init().
 */
typedef void (*cpva_on_kws_score_fn)(
    float   hey_siri_score,
    int     triggered,
    int64_t timestamp_us,
    void   *user_data
);

/**
 * Called on every VAD state-machine transition.
 *
 * This is the primary VAD notification.  The host uses this to manage the
 * auxiliary audio stream, drive UI, or log speech activity:
 *
 *   SILENCE → ONSET   Pre-buffer mic audio; prepare to open the auxiliary
 *                      input stream to the device (must be ready within 50 ms
 *                      of requestSiri, which fires at ACTIVE).
 *   ONSET   → ACTIVE  300 ms confirmed.  on_request_siri() has just been
 *                      called.  The device will set up the auxiliary stream
 *                      upon receiving requestSiri; the host should respond to
 *                      the device's stream-setup command promptly.
 *   ONSET   → SILENCE False alarm — non-speech occurred before 300 ms.
 *                      Discard any pre-buffered audio.
 *   ACTIVE  → GAP     Brief pause detected (< 400 ms allowed).
 *   GAP     → ACTIVE  Speech resumed — still the same region; no re-trigger.
 *   GAP     → SILENCE Speech region ended (gap ≥ 400 ms).  The host may
 *                      tear down any auxiliary stream it opened.
 *
 * Called synchronously from cpva_feed_audio(); must return quickly.
 *
 * @param new_state    The state just entered.
 * @param timestamp_us Monotonic timestamp (µs) of the audio hop that caused
 *                     the transition.
 * @param probability  TenVad speech-activity probability [0.0, 1.0].
 * @param user_data    Opaque pointer supplied at cpva_init().
 */
typedef void (*cpva_on_vad_state_changed_fn)(
    cpva_vad_state_t  new_state,
    int64_t           timestamp_us,
    float             probability,
    void             *user_data
);

/**
 * Aggregated callback table.
 * Set unused fields to NULL.
 *
 * Auxiliary-stream lifecycle (CarPlay spec §3.3.7.2):
 *   The device opens / closes the auxiliary input audio stream in response
 *   to a requestSiri command.  The host's CarPlay protocol layer handles
 *   this directly; the library signals the relevant moments via:
 *     on_vad_state_changed(ONSET)  → start pre-buffering mic audio
 *     on_request_siri()            → requestSiri sent; device will open stream
 *     on_vad_state_changed(SILENCE)→ speech ended; stream no longer needed
 */
typedef struct {
    /** Required: send requestSiri(voiceActivation, siriTriggerTimestamp) to device. */
    cpva_on_request_siri_fn      on_request_siri;

    /**
     * Optional: KWS per-frame score notification (KWS mode only).
     * Called every 20 ms regardless of whether the threshold was crossed.
     * Useful for tuning, confidence meters, and activity logging.
     */
    cpva_on_kws_score_fn         on_kws_score;

    /**
     * Optional: VAD state-machine transition notification (VAD mode only).
     * Use this to manage the auxiliary audio stream lifecycle, UI, and logging.
     * See cpva_on_vad_state_changed_fn for the full state-transition table.
     */
    cpva_on_vad_state_changed_fn on_vad_state_changed;
} cpva_callbacks_t;

/* -------------------------------------------------------------------------
 * Capabilities info
 * ---------------------------------------------------------------------- */

/**
 * Returned by cpva_get_info() to populate the CarPlay info message response.
 */
typedef struct {
    cpva_language_tag_t active_language;
    uint8_t             supported_language_count;
    cpva_language_tag_t supported_languages[32];
} cpva_info_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * Create and initialise a voice-activation context.
 *
 * Internally creates:
 *   - the 3A front-end processor (audio3a_create)
 *   - the hey_siri_kws engine (kws_create) for KWS mode
 *   - the TenVad engine (ten_vad_create) for VAD mode
 *
 * Both engines are created unconditionally so that mode switching at runtime
 * (cpva_set_voice_activation_mode) has zero latency.
 *
 * @param[out] out_ctx    Receives the allocated context on success.
 * @param      format     PCM format for all audio fed into the library.
 *                        sample_rate_hz must be 16000 for KWS mode.
 * @param      a3_config  3A front-end config; NULL for all defaults.
 * @param      kws_config KWS engine config; NULL for all defaults.
 * @param      vad_config TenVad config; NULL for all defaults.
 * @param      callbacks  Callback table; on_request_siri must not be NULL.
 * @param      user_data  Opaque pointer forwarded to every callback.
 * @return CPVA_OK on success, negative error code otherwise.
 */
CPVA_API cpva_error_t cpva_init(
    cpva_context_t            **out_ctx,
    const cpva_audio_format_t  *format,
    const cpva_3a_config_t     *a3_config,
    const cpva_kws_config_t    *kws_config,
    const cpva_vad_config_t    *vad_config,
    const cpva_callbacks_t     *callbacks,
    void                       *user_data
);

/**
 * Destroy a context and release all resources.  Safe to call with NULL.
 */
CPVA_API void cpva_destroy(cpva_context_t *ctx);

/* -------------------------------------------------------------------------
 * Device → Accessory: configuration (SET_PARAMETER)
 * ---------------------------------------------------------------------- */

/**
 * Update the voice activation mode.
 * Called when the device sends SET_PARAMETER(voiceActivationMode).
 */
CPVA_API cpva_error_t cpva_set_voice_activation_mode(
    cpva_context_t               *ctx,
    cpva_voice_activation_mode_t  mode
);

/**
 * Update the keyword detector language.
 * Called when the device sends SET_PARAMETER(voiceModelLanguage).
 *
 * @return CPVA_ERR_UNSUPPORTED_LANG if the language is not available.
 */
CPVA_API cpva_error_t cpva_set_voice_model_language(
    cpva_context_t            *ctx,
    const cpva_language_tag_t *language
);

/* -------------------------------------------------------------------------
 * Device → Accessory: session-state notifications
 * ---------------------------------------------------------------------- */

/**
 * Notify the library of the device's speech app-state change.
 * While CPVA_APP_STATE_SPEECH_RECOGNIZING, on_request_siri is suppressed.
 */
CPVA_API cpva_error_t cpva_notify_app_state_speech(
    cpva_context_t         *ctx,
    cpva_app_state_speech_t state
);

/**
 * Notify whether the device has an active auxiliary input audio stream.
 * While active, on_request_siri is suppressed.
 *
 * @param active  Non-zero if the stream is active.
 */
CPVA_API cpva_error_t cpva_notify_auxiliary_audio_stream_active(
    cpva_context_t *ctx,
    int             active
);

/* -------------------------------------------------------------------------
 * Speaker (far-end) audio — AEC reference
 * ---------------------------------------------------------------------- */

/**
 * Enable or disable AEC in the 3A front-end.
 *
 * Call with active=1 when the car speakers begin playing audio (music,
 * navigation prompts, CarPlay audio, etc.) and active=0 when they stop.
 * NS and HPF continue running regardless of this setting.
 *
 * Maps to audio3a_set_aec_active() internally.
 *
 * @param active  1 = enable AEC, 0 = disable AEC.
 */
CPVA_API cpva_error_t cpva_notify_speaker_active(
    cpva_context_t *ctx,
    int             active
);

/**
 * Feed a block of speaker (far-end / render) PCM as the AEC reference.
 *
 * Must be called once per 10 ms block written to the speaker output,
 * BEFORE the corresponding microphone frame is fed via cpva_feed_audio().
 * If AEC is inactive this is a no-op.
 *
 * The library internally slices the input into 10 ms frames
 * (sample_rate_hz / 100 samples each) before passing to audio3a_process_render().
 *
 * @param pcm_frames               Mono signed-16-bit PCM.
 * @param frame_count              Number of samples.
 * @param first_frame_timestamp_us Monotonic timestamp (µs) of the first sample.
 */
CPVA_API cpva_error_t cpva_feed_speaker_audio(
    cpva_context_t *ctx,
    const int16_t  *pcm_frames,
    size_t          frame_count,
    int64_t         first_frame_timestamp_us
);

/* -------------------------------------------------------------------------
 * Microphone (near-end) audio
 * ---------------------------------------------------------------------- */

/**
 * Feed a block of microphone PCM for front-end processing and detection.
 *
 * Internal pipeline per 10 ms frame:
 *   1. audio3a_process_capture()  — HPF + NS + (AEC if active)
 *   2. KWS engine                 — (TODO) keyword detection
 *      VAD engine                 — (TODO) Start-of-Speech detection
 *   3. on_request_siri()          — fired when a trigger is confirmed and
 *                                   not inhibited by device state
 *
 * The library internally accumulates samples and slices into 10 ms frames.
 * Callers may feed any number of samples per call.
 *
 * @param pcm_frames               Raw mono 16-bit microphone PCM.
 * @param frame_count              Number of samples.
 * @param first_frame_timestamp_us Monotonic timestamp (µs) of the first sample.
 */
CPVA_API cpva_error_t cpva_feed_audio(
    cpva_context_t *ctx,
    const int16_t  *pcm_frames,
    size_t          frame_count,
    int64_t         first_frame_timestamp_us
);

/* -------------------------------------------------------------------------
 * 3A / AEC runtime control
 * ---------------------------------------------------------------------- */

/**
 * Enable or bypass the 3A front-end (HPF + NS + AEC) on the capture path.
 *
 * When disabled, cpva_feed_audio() passes raw microphone PCM through to the
 * detection engines without calling audio3a_process_capture().
 * Default after cpva_init(): enabled (1).
 *
 * @param enabled  Non-zero to run 3A; zero for bypass (raw passthrough).
 */
CPVA_API cpva_error_t cpva_set_3a_enabled(cpva_context_t *ctx, int enabled);

/**
 * Update the render-to-capture delay estimate at runtime.
 * Forwards to audio3a_set_delay_ms() internally.
 *
 * @param delay_ms  New delay in milliseconds.
 */
CPVA_API cpva_error_t cpva_set_aec_delay_ms(cpva_context_t *ctx, int delay_ms);

/* -------------------------------------------------------------------------
 * Queries
 * ---------------------------------------------------------------------- */

/**
 * Snapshot of the most recently processed 10 ms capture frame.
 * Updated on every cpva_feed_audio() frame pop; useful for demo metering.
 */
typedef struct {
    float processed_rms;
    int32_t processed_peak;
    int a3a_enabled;
} cpva_capture_stats_t;

/**
 * Read levels after the latest 3A step (or raw passthrough when 3A is off).
 */
CPVA_API cpva_error_t cpva_get_capture_stats(
    cpva_context_t *ctx,
    cpva_capture_stats_t *stats
);

/**
 * Optional tap invoked once per processed 10 ms capture frame, after 3A and
 * before detection.  Used by demo hosts to record proc PCM to disk.
 */
typedef void (*cpva_on_processed_frame_fn)(
    const int16_t *pcm,
    size_t         sample_count,
    int64_t        first_sample_timestamp_us,
    void          *user_data
);

/**
 * Register (or clear with fn=NULL) the processed-frame tap.
 */
CPVA_API cpva_error_t cpva_set_processed_frame_listener(
    cpva_context_t              *ctx,
    cpva_on_processed_frame_fn   fn,
    void                        *user_data
);

/**
 * Query the current voice activation mode.
 */
CPVA_API cpva_error_t cpva_get_voice_activation_mode(
    cpva_context_t               *ctx,
    cpva_voice_activation_mode_t *mode
);

/**
 * Retrieve capability info for the CarPlay info message response.
 */
CPVA_API cpva_error_t cpva_get_info(
    cpva_context_t *ctx,
    cpva_info_t    *info
);

/* -------------------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------------- */

/** Return a human-readable string for an error code (never NULL). */
CPVA_API const char *cpva_error_string(cpva_error_t err);

/** Return library version as a static string, e.g. "1.0.0". */
CPVA_API const char *cpva_version_string(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CARPLAY_VOICE_ACTIVATION_H */
