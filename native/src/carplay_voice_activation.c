/**
 * carplay_voice_activation.c
 *
 * CarPlay Voice Activation Library — implementation.
 *
 * Audio pipeline per 10 ms frame:
 *
 *   cpva_feed_speaker_audio() ──► render ring-buf ──► audio3a_process_render()
 *
 *   cpva_feed_audio()  ──► capture ring-buf ──► audio3a_process_capture()
 *                                                       │
 *                                              cleaned 10 ms frame
 *                                                       │
 *                                    ┌──────────────────┴───────────────────┐
 *                          hey_siri_kws engine               TenVad engine
 *                                    │                               │
 *                                    └──────────────┬────────────────┘
 *                                            on_request_siri()
 *
 * VAD state machine (CarPlay spec §3.3.7.2.2):
 *
 *   SILENCE ──(300ms speech)──► ONSET ──(confirmed)──► ACTIVE ──► GAP
 *                                                                   │
 *                                         gap ≥ 400ms ◄────────────┘
 *                                              │
 *                                           SILENCE
 */

#include "carplay_voice_activation.h"
#include "audio3a.h"
#include "ten_vad.h"
#include "hey_siri_kws.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>   /* INT64_MIN, int16_t, int64_t */
#include <limits.h>   /* belt-and-suspenders for INT64_MIN on some NDK versions */

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/* VAD: minimum continuous speech before an SoS trigger is sent (300 ms). */
#define CPVA_VAD_CONFIRM_US   300000LL

/* VAD: maximum non-speech gap between two speech regions to be treated as
 * one continuous region (400 ms). */
#define CPVA_VAD_GAP_MAX_US   400000LL

/* Ring-buffer: maximum frames held (20 × 10 ms = 200 ms of audio). */
#define CPVA_RINGBUF_FRAMES   20

/* Default VAD threshold when caller passes 0.0f. */
#define CPVA_VAD_DEFAULT_THRESHOLD  0.5f

/* KWS defaults */
#define CPVA_KWS_DEFAULT_THRESHOLD          0.70f
#define CPVA_KWS_DEFAULT_ONSET_OFFSET_MS    400     /* 'S' start relative to detect */
#define CPVA_KWS_DEFAULT_COOLDOWN_MS        1500    /* min gap between triggers */

/* -------------------------------------------------------------------------
 * Internal: simple sample accumulation buffer
 * ---------------------------------------------------------------------- */

typedef struct {
    int16_t *data;
    size_t   capacity;           /* max samples */
    size_t   count;              /* samples currently stored */
    int64_t  head_timestamp_us;  /* monotonic µs of the oldest stored sample */
} cpva_sample_buf_t;

static cpva_error_t sample_buf_init(cpva_sample_buf_t *b, size_t capacity)
{
    b->data = (int16_t *)calloc(capacity, sizeof(int16_t));
    if (!b->data) return CPVA_ERR_INTERNAL;
    b->capacity          = capacity;
    b->count             = 0;
    b->head_timestamp_us = 0;
    return CPVA_OK;
}

static void sample_buf_free(cpva_sample_buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->capacity = b->count = 0;
}

static void sample_buf_push(cpva_sample_buf_t *b,
                            const int16_t     *src,
                            size_t             n,
                            int64_t            first_sample_ts_us)
{
    if (b->count == 0) {
        b->head_timestamp_us = first_sample_ts_us;
    }
    size_t space = b->capacity - b->count;
    if (n > space) n = space;
    memcpy(b->data + b->count, src, n * sizeof(int16_t));
    b->count += n;
}

/**
 * Pop exactly frame_size samples into dst.
 * Returns 1 on success, 0 if there are not enough samples yet.
 * Advances head_timestamp_us by one frame duration after the pop.
 */
static int sample_buf_pop_frame(cpva_sample_buf_t *b,
                                int16_t           *dst,
                                size_t             frame_size,
                                uint32_t           sample_rate_hz)
{
    if (b->count < frame_size) return 0;

    memcpy(dst, b->data, frame_size * sizeof(int16_t));

    size_t remaining = b->count - frame_size;
    if (remaining > 0) {
        memmove(b->data, b->data + frame_size, remaining * sizeof(int16_t));
    }
    b->count = remaining;

    b->head_timestamp_us +=
        (int64_t)frame_size * 1000000LL / (int64_t)sample_rate_hz;

    return 1;
}

/* -------------------------------------------------------------------------
 * VAD state aliases (public enum re-used internally)
 * ---------------------------------------------------------------------- */

#define CPVA_VAD_ST_SILENCE  CPVA_VAD_STATE_SILENCE
#define CPVA_VAD_ST_ONSET    CPVA_VAD_STATE_ONSET
#define CPVA_VAD_ST_ACTIVE   CPVA_VAD_STATE_ACTIVE
#define CPVA_VAD_ST_GAP      CPVA_VAD_STATE_GAP

/* -------------------------------------------------------------------------
 * Internal context
 * ---------------------------------------------------------------------- */

struct cpva_context {
    /* Audio format */
    cpva_audio_format_t          format;
    size_t                       frame_size;      /* samples per 10 ms 3A frame */

    /* 3A front-end (HPF + NS + AEC) */
    Audio3aHandle                audio3a;

    /* Ring buffers for 3A */
    cpva_sample_buf_t            capture_buf;
    cpva_sample_buf_t            render_buf;
    int16_t                     *processed_frame; /* scratch, size = frame_size */

    /* hey_siri_kws engine */
    KwsEngine                   *kws_engine;
    float                        kws_threshold;
    int64_t                      kws_onset_offset_us;  /* siriTriggerTimestamp offset */
    int64_t                      kws_cooldown_us;      /* min µs between triggers */
    int64_t                      kws_last_trigger_ts_us;
    cpva_sample_buf_t            kws_buf;          /* accumulate 3A output → KWS */
    int16_t                      kws_frame_buf[KWS_FRAME_SAMPLES]; /* 320 samples */

    /* TenVad engine */
    ten_vad_handle_t             vad_handle;
    size_t                       vad_hop_size;
    cpva_sample_buf_t            vad_buf;         /* accumulates 3A output → TenVad */
    int16_t                     *vad_frame_buf;   /* scratch, size = vad_hop_size */

    /* VAD state machine */
    cpva_vad_state_t             vad_state;
    int64_t                      vad_onset_ts_us;      /* SoS timestamp (t1) */
    int64_t                      vad_speech_accum_us;  /* speech duration in ONSET */
    int64_t                      vad_gap_start_ts_us;  /* when current gap began */

    /* CarPlay session state */
    cpva_voice_activation_mode_t mode;
    cpva_app_state_speech_t      app_state_speech;
    int                          auxiliary_audio_stream_active;

    /* Callbacks */
    cpva_callbacks_t             callbacks;
    void                        *user_data;

    /* Language */
    cpva_language_tag_t          active_language;
    cpva_language_tag_t          supported_languages[32];
    uint8_t                      supported_language_count;
};

/* -------------------------------------------------------------------------
 * Version
 * ---------------------------------------------------------------------- */

const char *cpva_version_string(void) { return "1.0.0"; }

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static int _cpva_is_request_siri_inhibited(const cpva_context_t *ctx)
{
    if (ctx->app_state_speech == CPVA_APP_STATE_SPEECH_RECOGNIZING) return 1;
    if (ctx->auxiliary_audio_stream_active)                          return 1;
    return 0;
}

/**
 * Hard-reset VAD state without firing any callbacks.
 * Used internally when the session layer changes mode or the device tears
 * down the auxiliary stream — the host already knows about those events
 * from the higher-level notifications, so we skip the VAD callback here.
 */
static void _cpva_vad_reset(cpva_context_t *ctx)
{
    ctx->vad_state           = CPVA_VAD_ST_SILENCE;
    ctx->vad_onset_ts_us     = 0;
    ctx->vad_speech_accum_us = 0;
    ctx->vad_gap_start_ts_us = 0;
    ctx->vad_buf.count             = 0;
    ctx->vad_buf.head_timestamp_us = 0;
}

/* Emit a VAD state-change notification to the host. */
static void _cpva_notify_vad_state(cpva_context_t   *ctx,
                                   cpva_vad_state_t  new_state,
                                   int64_t           ts_us,
                                   float             probability)
{
    ctx->vad_state = new_state;

    if (ctx->callbacks.on_vad_state_changed) {
        ctx->callbacks.on_vad_state_changed(new_state, ts_us, probability,
                                            ctx->user_data);
    }
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

cpva_error_t cpva_init(
    cpva_context_t            **out_ctx,
    const cpva_audio_format_t  *format,
    const cpva_3a_config_t     *a3_config,
    const cpva_kws_config_t    *kws_config,
    const cpva_vad_config_t    *vad_config,
    const cpva_callbacks_t     *callbacks,
    void                       *user_data)
{
    if (!out_ctx || !format || !callbacks || !callbacks->on_request_siri) {
        return CPVA_ERR_INVALID_ARG;
    }
    if (format->channels != 1 || format->bits_per_sample != 16) {
        return CPVA_ERR_INVALID_ARG;
    }

    cpva_context_t *ctx = (cpva_context_t *)calloc(1, sizeof(cpva_context_t));
    if (!ctx) return CPVA_ERR_INTERNAL;

    ctx->format     = *format;
    ctx->callbacks  = *callbacks;
    ctx->user_data  = user_data;
    ctx->frame_size = format->sample_rate_hz / 100;   /* 10 ms */

    /* ----- 3A front-end ------------------------------------------------ */
    {
        Audio3aConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.sample_rate_hz = (int)format->sample_rate_hz;

        if (a3_config) {
            cfg.stream_delay_ms     = a3_config->stream_delay_ms > 0
                                          ? a3_config->stream_delay_ms : 100;
            cfg.ns_level            = (Audio3aNsLevel)a3_config->ns_level;
            cfg.aec_active_on_start = a3_config->aec_active_on_start;
        } else {
            cfg.stream_delay_ms     = 100;
            cfg.ns_level            = AUDIO3A_NS_VERY_HIGH;
            cfg.aec_active_on_start = 0;
        }

        ctx->audio3a = audio3a_create(&cfg);
        if (!ctx->audio3a) goto err_audio3a;
    }

    /* ----- 3A ring buffers --------------------------------------------- */
    {
        size_t cap = ctx->frame_size * CPVA_RINGBUF_FRAMES;
        if (sample_buf_init(&ctx->capture_buf, cap) != CPVA_OK) goto err_cap;
        if (sample_buf_init(&ctx->render_buf,  cap) != CPVA_OK) goto err_rnd;
        ctx->processed_frame = (int16_t *)malloc(ctx->frame_size * sizeof(int16_t));
        if (!ctx->processed_frame) goto err_pf;
    }

    /* ----- hey_siri_kws engine ----------------------------------------- */
    {
        ctx->kws_engine = kws_create();
        if (!ctx->kws_engine) goto err_kws;

        ctx->kws_threshold =
            (kws_config && kws_config->threshold > 0.0f)
                ? kws_config->threshold : CPVA_KWS_DEFAULT_THRESHOLD;

        int onset_ms = (kws_config && kws_config->keyword_to_siri_onset_ms > 0)
                           ? kws_config->keyword_to_siri_onset_ms
                           : CPVA_KWS_DEFAULT_ONSET_OFFSET_MS;
        ctx->kws_onset_offset_us = (int64_t)onset_ms * 1000LL;

        int cooldown_ms = (kws_config && kws_config->cooldown_ms > 0)
                              ? kws_config->cooldown_ms
                              : CPVA_KWS_DEFAULT_COOLDOWN_MS;
        ctx->kws_cooldown_us = (int64_t)cooldown_ms * 1000LL;

        ctx->kws_last_trigger_ts_us = INT64_MIN;

        /* KWS accumulation buffer: collects 3A 10ms frames until a full
         * KWS_FRAME_SAMPLES (320 @ 16kHz = 20ms) chunk is ready. */
        size_t kws_cap = KWS_FRAME_SAMPLES * CPVA_RINGBUF_FRAMES;
        if (sample_buf_init(&ctx->kws_buf, kws_cap) != CPVA_OK) goto err_kws_buf;
    }

    /* ----- TenVad engine ----------------------------------------------- */
    {
        size_t hop  = (vad_config && vad_config->hop_size > 0)
                          ? vad_config->hop_size
                          : ctx->frame_size;
        float  thr  = (vad_config && vad_config->threshold > 0.0f)
                          ? vad_config->threshold
                          : CPVA_VAD_DEFAULT_THRESHOLD;

        ctx->vad_hop_size = hop;

        if (ten_vad_create(&ctx->vad_handle, hop, thr) != 0) goto err_vad;

        size_t vad_cap = hop * CPVA_RINGBUF_FRAMES;
        if (sample_buf_init(&ctx->vad_buf, vad_cap) != CPVA_OK) goto err_vbuf;

        ctx->vad_frame_buf = (int16_t *)malloc(hop * sizeof(int16_t));
        if (!ctx->vad_frame_buf) goto err_vfb;
    }

    /* ----- Default session state --------------------------------------- */
    ctx->mode                          = CPVA_MODE_DEACTIVATED;
    ctx->app_state_speech              = CPVA_APP_STATE_SPEECH_IDLE;
    ctx->auxiliary_audio_stream_active = 0;
    _cpva_vad_reset(ctx);

    /* TODO: populate from keyword model at runtime */
    strncpy(ctx->active_language.tag,       "en-US", CPVA_LANGUAGE_TAG_MAX_LEN - 1);
    strncpy(ctx->supported_languages[0].tag,"en-US", CPVA_LANGUAGE_TAG_MAX_LEN - 1);
    ctx->supported_language_count = 1;

    /* TODO: initialise KWS engine here */

    *out_ctx = ctx;
    return CPVA_OK;

    /* ---- error unwind ------------------------------------------------- */
err_vfb:
    sample_buf_free(&ctx->vad_buf);
err_vbuf:
    ten_vad_destroy(&ctx->vad_handle);
err_vad:
    sample_buf_free(&ctx->kws_buf);
err_kws_buf:
    kws_destroy(ctx->kws_engine);
err_kws:
    free(ctx->processed_frame);
err_pf:
    sample_buf_free(&ctx->render_buf);
err_rnd:
    sample_buf_free(&ctx->capture_buf);
err_cap:
    audio3a_destroy(ctx->audio3a);
err_audio3a:
    free(ctx);
    return CPVA_ERR_INTERNAL;
}

void cpva_destroy(cpva_context_t *ctx)
{
    if (!ctx) return;
    kws_destroy(ctx->kws_engine);
    sample_buf_free(&ctx->kws_buf);
    ten_vad_destroy(&ctx->vad_handle);
    sample_buf_free(&ctx->vad_buf);
    free(ctx->vad_frame_buf);
    audio3a_destroy(ctx->audio3a);
    sample_buf_free(&ctx->capture_buf);
    sample_buf_free(&ctx->render_buf);
    free(ctx->processed_frame);
    free(ctx);
}

/* -------------------------------------------------------------------------
 * Device → Accessory: configuration (SET_PARAMETER)
 * ---------------------------------------------------------------------- */

cpva_error_t cpva_set_voice_activation_mode(
    cpva_context_t               *ctx,
    cpva_voice_activation_mode_t  mode)
{
    if (!ctx) return CPVA_ERR_NOT_INITIALIZED;

    ctx->mode = mode;

    switch (mode) {
    case CPVA_MODE_DEACTIVATED:
        _cpva_vad_reset(ctx);
        /* TODO: stop KWS engine */
        break;

    case CPVA_MODE_KEYWORD_DETECTION:
        _cpva_vad_reset(ctx);
        /* Flush KWS accumulation buffer and reset cooldown on mode entry. */
        ctx->kws_buf.count             = 0;
        ctx->kws_buf.head_timestamp_us = 0;
        ctx->kws_last_trigger_ts_us    = INT64_MIN;
        kws_reset_mfcc(ctx->kws_engine);
        break;

    case CPVA_MODE_VOICE_ACTIVITY_DETECTION:
        _cpva_vad_reset(ctx);
        break;

    default:
        return CPVA_ERR_INVALID_ARG;
    }

    return CPVA_OK;
}

cpva_error_t cpva_set_voice_model_language(
    cpva_context_t            *ctx,
    const cpva_language_tag_t *language)
{
    if (!ctx || !language) return CPVA_ERR_INVALID_ARG;
    /* TODO: verify language is present in the keyword model. */
    /* TODO: reload / hot-swap the keyword model to the new language. */
    strncpy(ctx->active_language.tag, language->tag, CPVA_LANGUAGE_TAG_MAX_LEN - 1);
    ctx->active_language.tag[CPVA_LANGUAGE_TAG_MAX_LEN - 1] = '\0';
    return CPVA_OK;
}

/* -------------------------------------------------------------------------
 * Device → Accessory: session-state notifications
 * ---------------------------------------------------------------------- */

cpva_error_t cpva_notify_app_state_speech(
    cpva_context_t         *ctx,
    cpva_app_state_speech_t state)
{
    if (!ctx) return CPVA_ERR_NOT_INITIALIZED;

    cpva_app_state_speech_t prev = ctx->app_state_speech;
    ctx->app_state_speech = state;

    /* When Siri finishes (Recognizing → Idle), reset VAD so we don't
     * immediately re-trigger on lingering speech activity. */
    if (prev == CPVA_APP_STATE_SPEECH_RECOGNIZING &&
        state == CPVA_APP_STATE_SPEECH_IDLE) {
        _cpva_vad_reset(ctx);
    }

    return CPVA_OK;
}

cpva_error_t cpva_notify_auxiliary_audio_stream_active(
    cpva_context_t *ctx,
    int             active)
{
    if (!ctx) return CPVA_ERR_NOT_INITIALIZED;

    int was_active = ctx->auxiliary_audio_stream_active;
    ctx->auxiliary_audio_stream_active = (active != 0) ? 1 : 0;

    /* Second pass rejected (stream torn down) → reset VAD so the engine
     * requires a fresh speech onset before sending another requestSiri. */
    if (was_active && !ctx->auxiliary_audio_stream_active) {
        _cpva_vad_reset(ctx);
    }

    return CPVA_OK;
}

/* -------------------------------------------------------------------------
 * Speaker (far-end) audio — AEC reference
 * ---------------------------------------------------------------------- */

cpva_error_t cpva_notify_speaker_active(cpva_context_t *ctx, int active)
{
    if (!ctx) return CPVA_ERR_NOT_INITIALIZED;
    audio3a_set_aec_active(ctx->audio3a, active ? 1 : 0);
    return CPVA_OK;
}

cpva_error_t cpva_feed_speaker_audio(
    cpva_context_t *ctx,
    const int16_t  *pcm_frames,
    size_t          frame_count,
    int64_t         first_frame_timestamp_us)
{
    if (!ctx)                            return CPVA_ERR_NOT_INITIALIZED;
    if (!pcm_frames || frame_count == 0) return CPVA_ERR_INVALID_ARG;

    sample_buf_push(&ctx->render_buf, pcm_frames, frame_count,
                    first_frame_timestamp_us);

    int16_t tmp[480]; /* max 10 ms @ 48 kHz */
    while (sample_buf_pop_frame(&ctx->render_buf, tmp,
                                ctx->frame_size, ctx->format.sample_rate_hz)) {
        audio3a_process_render(ctx->audio3a, tmp, (int)ctx->frame_size);
    }

    return CPVA_OK;
}

/* -------------------------------------------------------------------------
 * TenVad state machine — one hop frame
 * ---------------------------------------------------------------------- */

static void _cpva_vad_process_hop(cpva_context_t *ctx,
                                  const int16_t  *hop_frame,
                                  int64_t         hop_ts_us)
{
    float prob  = 0.0f;
    int   flag  = 0;
    int64_t hop_dur_us =
        (int64_t)ctx->vad_hop_size * 1000000LL / (int64_t)ctx->format.sample_rate_hz;

    if (ten_vad_process(ctx->vad_handle, hop_frame, ctx->vad_hop_size,
                        &prob, &flag) != 0) {
        return;
    }

    switch (ctx->vad_state) {

    case CPVA_VAD_ST_SILENCE:
        if (flag) {
            /* Speech onset — start 300 ms confirmation window. */
            ctx->vad_onset_ts_us     = hop_ts_us;
            ctx->vad_speech_accum_us = hop_dur_us;
            _cpva_notify_vad_state(ctx, CPVA_VAD_ST_ONSET, hop_ts_us, prob);
            /* on_start_auxiliary_audio_stream fired inside notify. */
        }
        break;

    case CPVA_VAD_ST_ONSET:
        if (flag) {
            ctx->vad_speech_accum_us += hop_dur_us;
            if (ctx->vad_speech_accum_us >= CPVA_VAD_CONFIRM_US) {
                /*
                 * 300 ms of continuous speech confirmed.
                 * Transition to ACTIVE first so the host knows the state,
                 * then fire requestSiri with the START timestamp (t1).
                 */
                _cpva_notify_vad_state(ctx, CPVA_VAD_ST_ACTIVE, hop_ts_us, prob);

                if (!_cpva_is_request_siri_inhibited(ctx)) {
                    ctx->callbacks.on_request_siri(
                        CPVA_SIRI_ACTION_VOICE_ACTIVATION,
                        ctx->vad_onset_ts_us,   /* t1: start of speech */
                        ctx->user_data);
                }
            }
        } else {
            /* Non-speech before 300 ms — false alarm, return to silence. */
            ctx->vad_speech_accum_us = 0;
            _cpva_notify_vad_state(ctx, CPVA_VAD_ST_SILENCE, hop_ts_us, prob);
            /* on_stop_auxiliary_audio_stream fired inside notify. */
        }
        break;

    case CPVA_VAD_ST_ACTIVE:
        if (!flag) {
            /* Speech paused — enter gap monitoring. */
            ctx->vad_gap_start_ts_us = hop_ts_us;
            _cpva_notify_vad_state(ctx, CPVA_VAD_ST_GAP, hop_ts_us, prob);
        }
        break;

    case CPVA_VAD_ST_GAP:
        if (flag) {
            /* Speech resumed within gap tolerance — same region, no re-trigger. */
            _cpva_notify_vad_state(ctx, CPVA_VAD_ST_ACTIVE, hop_ts_us, prob);
        } else {
            int64_t gap_us = hop_ts_us - ctx->vad_gap_start_ts_us;
            if (gap_us >= CPVA_VAD_GAP_MAX_US) {
                /* Gap ≥ 400 ms — speech region truly ended. */
                _cpva_notify_vad_state(ctx, CPVA_VAD_ST_SILENCE, hop_ts_us, prob);
                /* on_stop_auxiliary_audio_stream fired inside notify. */
            }
        }
        break;
    }
}

/* -------------------------------------------------------------------------
 * Microphone (near-end) audio — main processing path
 * ---------------------------------------------------------------------- */

/**
 * Process one 3A-cleaned 10 ms frame through the active detection engine.
 * The VAD path feeds processed audio into the TenVad accumulation buffer
 * and runs the state machine whenever a full hop is ready.
 */
static void _cpva_run_detection(cpva_context_t *ctx,
                                const int16_t  *frame,
                                int64_t         frame_ts_us)
{
    if (ctx->mode == CPVA_MODE_DEACTIVATED) return;

    /*
     * NOTE: we intentionally do NOT early-return on inhibited state here.
     * The detection engines must keep running during second-pass processing
     * so that buffered audio is correctly consumed and state remains valid.
     * The inhibition check is applied only at the point of firing on_request_siri.
     */

    if (ctx->mode == CPVA_MODE_KEYWORD_DETECTION) {
        /*
         * Accumulate 3A-cleaned frames into the KWS buffer.
         * Process a full KWS_FRAME_SAMPLES (320 @ 16kHz = 20ms) chunk
         * whenever one becomes available.
         *
         * Spec requirements (§3.3.7.2.1):
         *   • Detection latency from end-of-keyword ≤ 420 ms.
         *   • siriTriggerTimestamp = timestamp of FIRST phoneme ('S').
         *   • Report on_request_siri within 50 ms of detection.
         *   • FRR ≤ 1%;  FAR ≤ 2 per hour.
         */
        sample_buf_push(&ctx->kws_buf, frame, ctx->frame_size, frame_ts_us);

        while (sample_buf_pop_frame(&ctx->kws_buf, ctx->kws_frame_buf,
                                    KWS_FRAME_SAMPLES,
                                    ctx->format.sample_rate_hz)) {

            /* Timestamp of the first sample of this 20ms KWS frame. */
            int64_t kws_ts_us =
                ctx->kws_buf.head_timestamp_us
                - (int64_t)KWS_FRAME_SAMPLES * 1000000LL
                  / (int64_t)ctx->format.sample_rate_hz;

            /* Feed PCM into the streaming KWS model. */
            kws_feed_pcm_i16(ctx->kws_engine, ctx->kws_frame_buf);

            float score     = kws_get_label_score(ctx->kws_engine,
                                                  KWS_LABEL_HEY_SIRI);
            int   triggered = 0;

            if (score >= ctx->kws_threshold) {
                /* Enforce cooldown to avoid double-fires on the same utterance. */
                int64_t elapsed = kws_ts_us - ctx->kws_last_trigger_ts_us;
                if (elapsed >= ctx->kws_cooldown_us &&
                    !_cpva_is_request_siri_inhibited(ctx)) {

                    /*
                     * siriTriggerTimestamp = when the 'S' in "Siri" was spoken.
                     * We back-compute it by subtracting the estimated onset offset
                     * (time from 'S' start to end-of-detection).
                     */
                    int64_t siri_start_ts_us = kws_ts_us - ctx->kws_onset_offset_us;

                    ctx->callbacks.on_request_siri(
                        CPVA_SIRI_ACTION_VOICE_ACTIVATION,
                        siri_start_ts_us,
                        ctx->user_data);

                    ctx->kws_last_trigger_ts_us = kws_ts_us;
                    triggered = 1;
                }
            }

            /* Per-frame score notification (optional callback). */
            if (ctx->callbacks.on_kws_score) {
                ctx->callbacks.on_kws_score(score, triggered, kws_ts_us,
                                            ctx->user_data);
            }
        }
        return;
    }

    if (ctx->mode == CPVA_MODE_VOICE_ACTIVITY_DETECTION) {
        /*
         * Accumulate 3A-cleaned frames into the VAD buffer.
         * Process a full TenVad hop whenever one is available.
         */
        sample_buf_push(&ctx->vad_buf, frame, ctx->frame_size, frame_ts_us);

        while (sample_buf_pop_frame(&ctx->vad_buf, ctx->vad_frame_buf,
                                    ctx->vad_hop_size,
                                    ctx->format.sample_rate_hz)) {
            /*
             * Compute the timestamp of the first sample of this hop.
             * pop_frame already advanced head_timestamp_us to the next hop,
             * so subtract one hop duration to get this hop's start time.
             */
            int64_t hop_ts_us =
                ctx->vad_buf.head_timestamp_us
                - (int64_t)ctx->vad_hop_size * 1000000LL
                  / (int64_t)ctx->format.sample_rate_hz;

            _cpva_vad_process_hop(ctx, ctx->vad_frame_buf, hop_ts_us);
        }
    }
}

cpva_error_t cpva_feed_audio(
    cpva_context_t *ctx,
    const int16_t  *pcm_frames,
    size_t          frame_count,
    int64_t         first_frame_timestamp_us)
{
    if (!ctx)                            return CPVA_ERR_NOT_INITIALIZED;
    if (!pcm_frames || frame_count == 0) return CPVA_ERR_INVALID_ARG;

    sample_buf_push(&ctx->capture_buf, pcm_frames, frame_count,
                    first_frame_timestamp_us);

    while (sample_buf_pop_frame(&ctx->capture_buf,
                                ctx->processed_frame,
                                ctx->frame_size,
                                ctx->format.sample_rate_hz)) {

        /* Timestamp of the first sample in this 10 ms frame. */
        int64_t frame_ts_us =
            ctx->capture_buf.head_timestamp_us
            - (int64_t)ctx->frame_size * 1000000LL
              / (int64_t)ctx->format.sample_rate_hz;

        /* Step 1: 3A front-end (HPF + NS + AEC, in-place). */
        audio3a_process_capture(ctx->audio3a,
                                ctx->processed_frame,
                                ctx->processed_frame,
                                (int)ctx->frame_size);

        /* Step 2: detection (KWS stub or TenVad). */
        _cpva_run_detection(ctx, ctx->processed_frame, frame_ts_us);
    }

    return CPVA_OK;
}

/* -------------------------------------------------------------------------
 * AEC runtime control
 * ---------------------------------------------------------------------- */

cpva_error_t cpva_set_aec_delay_ms(cpva_context_t *ctx, int delay_ms)
{
    if (!ctx) return CPVA_ERR_NOT_INITIALIZED;
    audio3a_set_delay_ms(ctx->audio3a, delay_ms);
    return CPVA_OK;
}

/* -------------------------------------------------------------------------
 * Queries
 * ---------------------------------------------------------------------- */

cpva_error_t cpva_get_voice_activation_mode(
    cpva_context_t               *ctx,
    cpva_voice_activation_mode_t *mode)
{
    if (!ctx || !mode) return CPVA_ERR_INVALID_ARG;
    *mode = ctx->mode;
    return CPVA_OK;
}

cpva_error_t cpva_get_info(cpva_context_t *ctx, cpva_info_t *info)
{
    if (!ctx || !info) return CPVA_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    info->active_language          = ctx->active_language;
    info->supported_language_count = ctx->supported_language_count;
    memcpy(info->supported_languages,
           ctx->supported_languages,
           sizeof(cpva_language_tag_t) * ctx->supported_language_count);
    return CPVA_OK;
}

/* -------------------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------------- */

const char *cpva_error_string(cpva_error_t err)
{
    switch (err) {
    case CPVA_OK:                   return "OK";
    case CPVA_ERR_INVALID_ARG:      return "Invalid argument";
    case CPVA_ERR_INVALID_STATE:    return "Invalid state";
    case CPVA_ERR_NOT_INITIALIZED:  return "Not initialized";
    case CPVA_ERR_UNSUPPORTED_LANG: return "Unsupported language";
    case CPVA_ERR_INTERNAL:         return "Internal error";
    default:                        return "Unknown error";
    }
}
