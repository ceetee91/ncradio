#include "audio.h"
#ifdef HAVE_EQ
#include "eq.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <alloca.h>
#ifdef HAVE_UDEV
#include <libudev.h>
#endif

/* ── backend-specific headers ────────────────────────────────────────── */
#ifdef HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/utils/ringbuffer.h>
#else
#include <alsa/asoundlib.h>
#include <alloca.h>
#endif

/* ═══════════════════════════════════════════════════════════════════════
   PipeWire backend — capture and playback both via PipeWire
   ═══════════════════════════════════════════════════════════════════════ */
#ifdef HAVE_PIPEWIRE

/* State shared between the audio thread and PipeWire callbacks. */
struct pw_full_audio {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct pw_stream      *cap;    /* PW_DIRECTION_INPUT  — radio source  */
    struct pw_stream      *play;   /* PW_DIRECTION_OUTPUT — speaker sink  */
    struct spa_hook        cap_listener;
    struct spa_hook        play_listener;
    struct spa_ringbuffer  ring;
    uint8_t               *ring_buf;
    uint32_t               ring_bytes;      /* ring buffer size; must be a power of 2 */
    uint32_t               stride;          /* bytes per frame, updated on format negotiation */
    int                    play_connected;  /* 1 once play stream is connected */
    char                   errmsg[128];
    Audio                 *audio;
};

/* ── capture callbacks ───────────────────────────────────────────────── */

static void pw_cap_param_changed(void *data, uint32_t id,
                                 const struct spa_pod *param)
{
    struct pw_full_audio *s = data;
    if (!param || id != SPA_PARAM_Format) return;

    struct spa_audio_info_raw info = {0};
    if (spa_format_audio_raw_parse(param, &info) < 0) return;

    uint32_t rate     = info.rate     > 0 ? info.rate     : 48000;
    uint32_t channels = info.channels > 0 ? info.channels : 2;
    if (channels > 2) channels = 2;

    s->stride          = channels * 2;
    s->audio->rate     = rate;
    s->audio->channels = (int)channels;

#ifdef HAVE_EQ
    if (s->audio->eq) {
        pthread_mutex_lock(&s->audio->eq->lock);
        eq_init(s->audio->eq, (double)rate);
        pthread_mutex_unlock(&s->audio->eq->lock);
    }
#endif

    /* Update capture buffer params to match negotiated stride. */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_size, SPA_POD_Int(4096 * (int)s->stride));
    pw_stream_update_params(s->cap, params, 1);

    /* Connect playback with the source's negotiated rate to avoid a second
       resample hop between our ring buffer and the sink. */
    if (!s->play_connected && s->play) {
        struct spa_audio_info_raw fmt = {
            .format   = SPA_AUDIO_FORMAT_S16_LE,
            .rate     = rate,
            .channels = channels,
        };
        fmt.position[0] = SPA_AUDIO_CHANNEL_FL;
        if (channels >= 2) fmt.position[1] = SPA_AUDIO_CHANNEL_FR;

        uint8_t pod_buf[1024];
        struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
        const struct spa_pod *pparams[1] = {
            spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &fmt)
        };
        pw_stream_connect(s->play, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS,
                          pparams, 1);
        s->play_connected = 1;
    }
}

static void pw_cap_process(void *data)
{
    struct pw_full_audio *s = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(s->cap);
    if (!b) return;

    struct spa_data *d = &b->buffer->datas[0];
    uint32_t size = d->chunk->size;
    void    *pcm  = (uint8_t *)d->data + d->chunk->offset;

    if (size > 0) {
        uint32_t index;
        int32_t filled = spa_ringbuffer_get_write_index(&s->ring, &index);
        if ((uint32_t)(s->ring_bytes - filled) >= size) {
            spa_ringbuffer_write_data(&s->ring, s->ring_buf, s->ring_bytes,
                                      index & (s->ring_bytes - 1),
                                      pcm, size);
            spa_ringbuffer_write_update(&s->ring, (int32_t)(index + size));
        }

        pthread_mutex_lock(&s->audio->rec_lock);
        if (s->audio->rec_fn && s->stride > 0) {
            int frames = (int)(size / s->stride);
#ifdef HAVE_EQ
            if (s->audio->rec_eq_apply && s->audio->eq) {
                short *tmp = (short *)alloca(size);
                memcpy(tmp, pcm, size);
                eq_process(s->audio->eq, tmp, frames, s->audio->channels);
                s->audio->rec_fn(s->audio->rec_ctx, tmp, frames,
                                 s->audio->channels);
            } else
#endif
                s->audio->rec_fn(s->audio->rec_ctx, pcm, frames,
                                 s->audio->channels);
        }
        pthread_mutex_unlock(&s->audio->rec_lock);
    }

    pw_stream_queue_buffer(s->cap, b);
}

static const struct pw_stream_events pw_cap_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = pw_cap_param_changed,
    .process       = pw_cap_process,
};

/* ── playback callbacks ──────────────────────────────────────────────── */

static void pw_play_param_changed(void *data, uint32_t id,
                                  const struct spa_pod *param)
{
    struct pw_full_audio *s = data;
    if (!param || id != SPA_PARAM_Format) return;
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_size, SPA_POD_Int(4096 * (int)s->stride));
    pw_stream_update_params(s->play, params, 1);
}

static void pw_play_process(void *data)
{
    struct pw_full_audio *s = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(s->play);
    if (!b) return;

    struct spa_data *d = &b->buffer->datas[0];
    uint32_t req = b->requested > 0
                   ? (uint32_t)(b->requested * s->stride)
                   : d->maxsize;
    if (req > d->maxsize) req = d->maxsize;

    uint32_t index;
    int32_t avail = spa_ringbuffer_get_read_index(&s->ring, &index);
    uint32_t fill = 0;
    if (avail > 0)
        fill = (uint32_t)avail < req ? (uint32_t)avail : req;

    if (fill > 0) {
        spa_ringbuffer_read_data(&s->ring, s->ring_buf, s->ring_bytes,
                                 index & (s->ring_bytes - 1),
                                 d->data, fill);
        spa_ringbuffer_read_update(&s->ring, (int32_t)(index + fill));
#ifdef HAVE_EQ
        if (s->audio->eq)
            eq_process(s->audio->eq, (short *)d->data,
                       (int)(fill / s->stride), s->audio->channels);
#endif
    }
    if (fill < req)
        memset((uint8_t *)d->data + fill, 0, req - fill);

    d->chunk->offset = 0;
    d->chunk->stride = (int32_t)s->stride;
    d->chunk->size   = req;
    pw_stream_queue_buffer(s->play, b);
}

static const struct pw_stream_events pw_play_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = pw_play_param_changed,
    .process       = pw_play_process,
};

/* ── audio transfer thread (PipeWire) ────────────────────────────────── */

static void *audio_fn(void *arg)
{
    Audio *a = arg;
    struct pw_full_audio s;
    memset(&s, 0, sizeof(s));
    s.audio = a;

    /* Initial stride for S16_LE stereo; updated in pw_cap_param_changed once
       the source's native rate and channel count are negotiated. */
    s.stride    = 2 * 2;
    a->channels = 2;

    {
        uint32_t kb = a->buffer_frames > 0 ? a->buffer_frames : 1024;
        if (kb > 2048) kb = 2048;
        if (kb < 128)  kb = 128;
        uint32_t p = 128;
        while (p * 2 <= kb) p *= 2;
        s.ring_bytes = p * 1024;
    }
    s.ring_buf = malloc(s.ring_bytes);
    if (!s.ring_buf) {
        snprintf(a->errmsg, sizeof(a->errmsg), "out of memory");
        goto done;
    }
    spa_ringbuffer_init(&s.ring);
    pw_init(NULL, NULL);

    s.loop = pw_thread_loop_new("ncradio", NULL);
    if (!s.loop) {
        snprintf(a->errmsg, sizeof(a->errmsg), "pw_thread_loop_new failed");
        goto done;
    }
    s.context = pw_context_new(pw_thread_loop_get_loop(s.loop), NULL, 0);
    if (!s.context) {
        snprintf(a->errmsg, sizeof(a->errmsg), "pw_context_new failed");
        goto done;
    }
    if (pw_thread_loop_start(s.loop) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg), "pw_thread_loop_start failed");
        goto done;
    }

    pw_thread_loop_lock(s.loop);

    s.core = pw_context_connect(s.context, NULL, 0);
    if (!s.core) {
        snprintf(a->errmsg, sizeof(a->errmsg),
                 "cannot connect to PipeWire daemon");
        goto fail_locked;
    }

    /* Capture stream (radio source → us).
       Offer S16_LE with a rate range so PipeWire picks the source's native
       sample rate, avoiding a resample on the capture side. */
    {
        struct pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE,     "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE,     "Music",
            NULL);
        if (a->device[0])
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, a->device);

        s.cap = pw_stream_new(s.core, "ncradio-cap", props);
        if (!s.cap) {
            snprintf(a->errmsg, sizeof(a->errmsg),
                     "pw_stream_new (capture) failed");
            goto fail_locked;
        }
        pw_stream_add_listener(s.cap, &s.cap_listener, &pw_cap_events, &s);

        uint8_t pod_buf[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
        const struct spa_pod *params[1];
        params[0] = spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
            SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
            SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(48000, 8000, 384000),
            SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(2, 1, 2));
        pw_stream_connect(s.cap, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS,
                          params, 1);
    }

    /* Playback stream (us → speaker sink).
       Created here; connected from pw_cap_param_changed once the capture
       format is negotiated so both streams use the same sample rate. */
    {
        struct pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE,     "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE,     "Music",
            NULL);
        const char *tgt = a->play_device[0] ? a->play_device : NULL;
        if (tgt)
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, tgt);

        s.play = pw_stream_new(s.core, "ncradio", props);
        if (!s.play) {
            snprintf(a->errmsg, sizeof(a->errmsg),
                     "pw_stream_new (playback) failed");
            goto fail_locked;
        }
        pw_stream_add_listener(s.play, &s.play_listener, &pw_play_events, &s);
        /* pw_stream_connect is deferred to pw_cap_param_changed */
    }

    pw_thread_loop_unlock(s.loop);

    /* Park this thread until audio_stop() clears a->running. */
    {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 }; /* 10 ms */
        while (a->running && !s.errmsg[0])
            nanosleep(&ts, NULL);
        if (s.errmsg[0])
            snprintf(a->errmsg, sizeof(a->errmsg), "%s", s.errmsg);
    }
    goto cleanup;

fail_locked:
    pw_thread_loop_unlock(s.loop);

cleanup:
    if (s.loop) pw_thread_loop_stop(s.loop);
    if (s.cap)     { pw_stream_destroy(s.cap);      s.cap     = NULL; }
    if (s.play)    { pw_stream_destroy(s.play);     s.play    = NULL; }
    if (s.core)    { pw_core_disconnect(s.core);    s.core    = NULL; }
    if (s.context) { pw_context_destroy(s.context); s.context = NULL; }
    if (s.loop)    { pw_thread_loop_destroy(s.loop); s.loop   = NULL; }

done:
    free(s.ring_buf);
    a->running = 0;
    return NULL;
}

/* ── version ─────────────────────────────────────────────────────────── */

const char *audio_pipewire_version(void)
{
    pw_init(NULL, NULL);
    return pw_get_library_version();
}

/* ── node enumeration helper ─────────────────────────────────────────── */

struct pw_enum_ctx {
    char (*names)[AUDIO_DEV_NAMELEN];
    char (*descs)[AUDIO_DEV_DESCLEN];
    int  *count;
    int   max;
    int   sync_seq;
    const char          *media_class;
    struct pw_main_loop *loop;
    struct spa_hook      core_listener;
    struct spa_hook      reg_listener;
};

static void pw_enum_on_global(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props)
{
    struct pw_enum_ctx *ctx = data;
    (void)id; (void)permissions; (void)version;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    if (*ctx->count >= ctx->max) return;
    const char *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mc || strcmp(mc, ctx->media_class) != 0) return;
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!name) return;
    if (!desc || !desc[0]) desc = name;
    int i = *ctx->count;
    strncpy(ctx->names[i], name, AUDIO_DEV_NAMELEN - 1);
    ctx->names[i][AUDIO_DEV_NAMELEN - 1] = '\0';
    snprintf(ctx->descs[i], AUDIO_DEV_DESCLEN, "%s", desc);
    (*ctx->count)++;
}

static void pw_enum_on_global_remove(void *data, uint32_t id)
{
    (void)data; (void)id;
}

static const struct pw_registry_events pw_enum_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global        = pw_enum_on_global,
    .global_remove = pw_enum_on_global_remove,
};

static void pw_enum_on_core_done(void *data, uint32_t id, int seq)
{
    struct pw_enum_ctx *ctx = data;
    if ((int)id == PW_ID_CORE && seq == ctx->sync_seq)
        pw_main_loop_quit(ctx->loop);
}

static const struct pw_core_events pw_enum_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = pw_enum_on_core_done,
};

/* Enumerate PipeWire nodes of a given media class, appending to names/descs
   starting at the current value of *count. */
static void pw_run_enum(const char *media_class,
                        char names[][AUDIO_DEV_NAMELEN],
                        char descs[][AUDIO_DEV_DESCLEN],
                        int *count, int max)
{
    pw_init(NULL, NULL);

    struct pw_main_loop *loop = pw_main_loop_new(NULL);
    if (!loop) return;

    struct pw_context *ctx = pw_context_new(pw_main_loop_get_loop(loop),
                                            NULL, 0);
    if (!ctx) { pw_main_loop_destroy(loop); return; }

    struct pw_core *core = pw_context_connect(ctx, NULL, 0);
    if (!core) { pw_context_destroy(ctx); pw_main_loop_destroy(loop); return; }

    struct pw_registry *registry =
        pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    struct pw_enum_ctx ectx = {
        .names       = names,
        .descs       = descs,
        .count       = count,
        .max         = max,
        .media_class = media_class,
        .loop        = loop,
    };
    pw_registry_add_listener(registry, &ectx.reg_listener,
                             &pw_enum_registry_events, &ectx);
    pw_core_add_listener(core, &ectx.core_listener,
                         &pw_enum_core_events, &ectx);
    ectx.sync_seq = pw_core_sync(core, PW_ID_CORE, 0);
    pw_main_loop_run(loop);

    spa_hook_remove(&ectx.reg_listener);
    spa_hook_remove(&ectx.core_listener);
    pw_proxy_destroy((struct pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(ctx);
    pw_main_loop_destroy(loop);
}

/* ── capture device enumeration (PipeWire sources) ───────────────────── */

void audio_enum_devices(char names[][AUDIO_DEV_NAMELEN],
                        char descs[][AUDIO_DEV_DESCLEN],
                        int *count, int max)
{
    *count = 0;
    pw_run_enum("Audio/Source", names, descs, count, max);
}

/* ── playback device enumeration (PipeWire sinks) ────────────────────── */

void audio_enum_play_devices(char names[][AUDIO_DEV_NAMELEN],
                             char descs[][AUDIO_DEV_DESCLEN],
                             int *count, int max)
{
    *count = 0;
    if (max < 1) return;

    /* Index 0 is always the default (empty name → PipeWire auto-routes) */
    names[0][0] = '\0';
    strncpy(descs[0], "(default)", AUDIO_DEV_DESCLEN - 1);
    descs[0][AUDIO_DEV_DESCLEN - 1] = '\0';
    *count = 1;

    /* Append enumerated sinks starting at index 1 */
    pw_run_enum("Audio/Sink", names, descs, count, max);
}

/* ── PipeWire source detection for autodetect ────────────────────────── */

struct pw_detect_ctx {
    struct pw_main_loop *loop;
    struct spa_hook      core_listener;
    struct spa_hook      reg_listener;
    int                  sync_seq;
    const char          *card_id;    /* ALSA card id/number to match */
    int                  is_numeric; /* card_id is a decimal number   */
    char                 out[AUDIO_DEV_NAMELEN];
    int                  found;
};

static void pw_detect_global(void *data, uint32_t id, uint32_t permissions,
                              const char *type, uint32_t version,
                              const struct spa_dict *props)
{
    struct pw_detect_ctx *ctx = data;
    (void)id; (void)permissions; (void)version;
    if (ctx->found) return;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    const char *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mc || strcmp(mc, "Audio/Source") != 0) return;

    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!node_name) return;

    /* Primary match: ALSA card ID property */
    const char *alsa_id = spa_dict_lookup(props, "api.alsa.card.id");
    if (alsa_id && strcmp(alsa_id, ctx->card_id) == 0) {
        strncpy(ctx->out, node_name, AUDIO_DEV_NAMELEN - 1);
        ctx->out[AUDIO_DEV_NAMELEN - 1] = '\0';
        ctx->found = 1;
        return;
    }

    /* Secondary match: ALSA card number property (when card_id is numeric) */
    if (ctx->is_numeric) {
        const char *alsa_card = spa_dict_lookup(props, "api.alsa.card");
        if (alsa_card && strcmp(alsa_card, ctx->card_id) == 0) {
            strncpy(ctx->out, node_name, AUDIO_DEV_NAMELEN - 1);
            ctx->out[AUDIO_DEV_NAMELEN - 1] = '\0';
            ctx->found = 1;
            return;
        }
    }

    /* Fallback: card id appears as a substring of the node name */
    if (strstr(node_name, ctx->card_id)) {
        strncpy(ctx->out, node_name, AUDIO_DEV_NAMELEN - 1);
        ctx->out[AUDIO_DEV_NAMELEN - 1] = '\0';
        ctx->found = 1;
    }
}

static void pw_detect_global_remove(void *data, uint32_t id)
{
    (void)data; (void)id;
}

static const struct pw_registry_events pw_detect_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global        = pw_detect_global,
    .global_remove = pw_detect_global_remove,
};

static void pw_detect_core_done(void *data, uint32_t id, int seq)
{
    struct pw_detect_ctx *ctx = data;
    if ((int)id == PW_ID_CORE && seq == ctx->sync_seq)
        pw_main_loop_quit(ctx->loop);
}

static const struct pw_core_events pw_detect_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = pw_detect_core_done,
};

/* Find a PipeWire Audio/Source node that corresponds to the given ALSA
   card identifier (ID string or card number). Returns 1 on success. */
static int pw_detect_source(const char *card_id, char *out, int out_size)
{
    if (out_size > 0) out[0] = '\0';
    pw_init(NULL, NULL);

    struct pw_main_loop *loop = pw_main_loop_new(NULL);
    if (!loop) return 0;
    struct pw_context *ctx =
        pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
    if (!ctx) { pw_main_loop_destroy(loop); return 0; }
    struct pw_core *core = pw_context_connect(ctx, NULL, 0);
    if (!core) { pw_context_destroy(ctx); pw_main_loop_destroy(loop); return 0; }

    struct pw_registry *registry =
        pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    struct pw_detect_ctx dctx = {
        .loop       = loop,
        .card_id    = card_id,
        .is_numeric = (card_id[0] >= '0' && card_id[0] <= '9'),
        .found      = 0,
    };
    pw_registry_add_listener(registry, &dctx.reg_listener,
                             &pw_detect_registry_events, &dctx);
    pw_core_add_listener(core, &dctx.core_listener,
                         &pw_detect_core_events, &dctx);
    dctx.sync_seq = pw_core_sync(core, PW_ID_CORE, 0);
    pw_main_loop_run(loop);

    spa_hook_remove(&dctx.reg_listener);
    spa_hook_remove(&dctx.core_listener);
    pw_proxy_destroy((struct pw_proxy *)registry);
    pw_core_disconnect(core);
    pw_context_destroy(ctx);
    pw_main_loop_destroy(loop);

    if (dctx.found) {
        strncpy(out, dctx.out, (size_t)(out_size - 1));
        out[out_size - 1] = '\0';
        return 1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   ALSA backend — capture and playback both via libasound
   ═══════════════════════════════════════════════════════════════════════ */
#else /* !HAVE_PIPEWIRE */

/* Candidate capture rates tried in preference order (highest first). */
static const unsigned int RATES[] = { 96000, 48000, 44100, 32000, 22050, 16000, 0 };

/* ── audio transfer thread (ALSA) ────────────────────────────────────── */

static void *audio_fn(void *arg)
{
    Audio *a = arg;
    snd_pcm_t *cap  = NULL;
    snd_pcm_t *play = NULL;
    void      *buf  = NULL;

    snd_pcm_uframes_t period = a->buffer_frames > 0 ? a->buffer_frames : 4096;
    const char *play_dev = a->play_device[0] ? a->play_device : "default";

    if (snd_pcm_open(&cap, a->device, SND_PCM_STREAM_CAPTURE, 0) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg),
                 "cannot open '%s'", a->device);
        goto done;
    }

    snd_pcm_hw_params_t *cap_hw;
    snd_pcm_hw_params_alloca(&cap_hw);
    snd_pcm_hw_params_any(cap, cap_hw);

    unsigned int rate = 44100;
    for (int i = 0; RATES[i]; i++) {
        if (snd_pcm_hw_params_test_rate(cap, cap_hw, RATES[i], 0) == 0) {
            rate = RATES[i];
            break;
        }
    }

    snd_pcm_hw_params_any(cap, cap_hw);
    snd_pcm_hw_params_set_access(cap, cap_hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(cap, cap_hw, SND_PCM_FORMAT_S16_LE);

    int channels = 2;
    if (snd_pcm_hw_params_set_channels(cap, cap_hw, 2) < 0) {
        channels = 1;
        snd_pcm_hw_params_set_channels(cap, cap_hw, 1);
    }

    snd_pcm_hw_params_set_rate(cap, cap_hw, rate, 0);
    snd_pcm_hw_params_set_period_size_near(cap, cap_hw, &period, 0);

    if (snd_pcm_hw_params(cap, cap_hw) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg),
                 "cannot configure capture device");
        goto done;
    }
    snd_pcm_hw_params_get_period_size(cap_hw, &period, NULL);
    snd_pcm_hw_params_get_rate(cap_hw, &rate, NULL);

    a->rate     = rate;
    a->channels = channels;

#ifdef HAVE_EQ
    if (a->eq) {
        pthread_mutex_lock(&a->eq->lock);
        eq_init(a->eq, (double)rate);
        pthread_mutex_unlock(&a->eq->lock);
    }
#endif

    if (snd_pcm_prepare(cap) < 0 || snd_pcm_start(cap) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg), "cannot start capture device");
        goto done;
    }

    size_t buf_bytes = period * (size_t)channels * 2;
    buf = malloc(buf_bytes);
    if (!buf) {
        snprintf(a->errmsg, sizeof(a->errmsg), "out of memory");
        goto done;
    }

    if (snd_pcm_open(&play, play_dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg),
                 "cannot open playback device '%s'", play_dev);
        goto done;
    }

    snd_pcm_hw_params_t *play_hw;
    snd_pcm_hw_params_alloca(&play_hw);
    snd_pcm_hw_params_any(play, play_hw);
    snd_pcm_hw_params_set_access(play, play_hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(play, play_hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(play, play_hw, (unsigned int)channels);
    snd_pcm_hw_params_set_rate_near(play, play_hw, &rate, 0);

    snd_pcm_uframes_t play_period = period;
    snd_pcm_uframes_t play_buffer = period * 4;
    snd_pcm_hw_params_set_period_size_near(play, play_hw, &play_period, 0);
    snd_pcm_hw_params_set_buffer_size_near(play, play_hw, &play_buffer);

    if (snd_pcm_hw_params(play, play_hw) < 0) {
        snprintf(a->errmsg, sizeof(a->errmsg), "cannot configure playback device");
        goto done;
    }
    snd_pcm_prepare(play);

    while (a->running) {
        snd_pcm_sframes_t n = snd_pcm_readi(cap, buf, period);
        if (!a->running) break;
        if (n < 0) {
            if (snd_pcm_recover(cap, (int)n, 1) < 0) {
                snprintf(a->errmsg, sizeof(a->errmsg), "capture read error");
                break;
            }
            continue;
        }
#ifdef HAVE_EQ
        if (a->eq && !a->rec_eq_apply) {
            /* rec_eq_apply=0: record raw audio before applying EQ */
            pthread_mutex_lock(&a->rec_lock);
            if (a->rec_fn) a->rec_fn(a->rec_ctx, buf, (int)n, a->channels);
            pthread_mutex_unlock(&a->rec_lock);
        }
        if (a->eq)
            eq_process(a->eq, buf, (int)n, a->channels);
#endif
        snd_pcm_sframes_t w = snd_pcm_writei(play, buf, (snd_pcm_uframes_t)n);
        if (w < 0) {
            if (snd_pcm_recover(play, (int)w, 1) < 0)
                snd_pcm_prepare(play);
        }
#ifdef HAVE_EQ
        if (!a->eq || a->rec_eq_apply) {
            /* rec_eq_apply=1 (or no EQ): record after EQ is applied */
#endif
            pthread_mutex_lock(&a->rec_lock);
            if (a->rec_fn) a->rec_fn(a->rec_ctx, buf, (int)n, a->channels);
            pthread_mutex_unlock(&a->rec_lock);
#ifdef HAVE_EQ
        }
#endif
    }

done:
    free(buf);
    if (play) snd_pcm_close(play);
    if (cap)  snd_pcm_close(cap);
    a->running = 0;
    return NULL;
}

/* ── version ─────────────────────────────────────────────────────────── */

const char *audio_alsa_version(void)
{
    return snd_asoundlib_version();
}

/* ── device enumeration (ALSA) ───────────────────────────────────────── */

static void enum_pcm_devices(char names[][AUDIO_DEV_NAMELEN],
                             char descs[][AUDIO_DEV_DESCLEN],
                             int *count, int max,
                             snd_pcm_stream_t stream)
{
    int card = -1;
    while (*count < max && snd_card_next(&card) >= 0 && card >= 0) {
        char ctl_name[16];
        snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);

        snd_ctl_t *ctl;
        if (snd_ctl_open(&ctl, ctl_name, 0) < 0) continue;

        snd_ctl_card_info_t *ci;
        snd_ctl_card_info_alloca(&ci);
        if (snd_ctl_card_info(ctl, ci) < 0) { snd_ctl_close(ctl); continue; }

        const char *card_id   = snd_ctl_card_info_get_id(ci);
        const char *card_name = snd_ctl_card_info_get_name(ci);

        int dev = -1;
        while (*count < max &&
               snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0) {
            snd_pcm_info_t *pi;
            snd_pcm_info_alloca(&pi);
            snd_pcm_info_set_device(pi, (unsigned int)dev);
            snd_pcm_info_set_subdevice(pi, 0);
            snd_pcm_info_set_stream(pi, stream);
            if (snd_ctl_pcm_info(ctl, pi) < 0) continue;

            const char *dev_name = snd_pcm_info_get_name(pi);
            snprintf(names[*count], AUDIO_DEV_NAMELEN,
                     "hw:CARD=%s,DEV=%d", card_id, dev);
            snprintf(descs[*count], AUDIO_DEV_DESCLEN,
                     "%s, %s", card_name, dev_name ? dev_name : "");
            (*count)++;
        }
        snd_ctl_close(ctl);
    }
}

void audio_enum_devices(char names[][AUDIO_DEV_NAMELEN],
                        char descs[][AUDIO_DEV_DESCLEN],
                        int *count, int max)
{
    *count = 0;
    enum_pcm_devices(names, descs, count, max, SND_PCM_STREAM_CAPTURE);
}

void audio_enum_play_devices(char names[][AUDIO_DEV_NAMELEN],
                             char descs[][AUDIO_DEV_DESCLEN],
                             int *count, int max)
{
    *count = 0;
    if (max < 1) return;

    /* Index 0 is always the ALSA "default" device */
    names[0][0] = '\0';
    strncpy(descs[0], "(default)", AUDIO_DEV_DESCLEN - 1);
    descs[0][AUDIO_DEV_DESCLEN - 1] = '\0';
    *count = 1;

    enum_pcm_devices(names, descs, count, max, SND_PCM_STREAM_PLAYBACK);
}

#endif /* HAVE_PIPEWIRE */

/* ═══════════════════════════════════════════════════════════════════════
   Public API (shared by both backends)
   ═══════════════════════════════════════════════════════════════════════ */

int audio_start(Audio *a, const char *device)
{
    audio_stop(a);
    strncpy(a->device, device ? device : "", AUDIO_DEV_NAMELEN - 1);
    a->device[AUDIO_DEV_NAMELEN - 1] = '\0';
    a->errmsg[0] = '\0';
    a->rate      = 0;
    a->channels  = 0;
    pthread_mutex_init(&a->rec_lock, NULL);
    a->running   = 1;
    a->started   = 1;
    if (pthread_create(&a->thread, NULL, audio_fn, a) != 0) {
        a->running = 0;
        a->started = 0;
        snprintf(a->errmsg, sizeof(a->errmsg), "pthread_create failed");
        return -1;
    }
    return 0;
}

void audio_stop(Audio *a)
{
    if (!a->started) return;
    a->running = 0;
    pthread_join(a->thread, NULL);
    a->started = 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   Sysfs / udev detection helpers (shared by both backends)
   These functions use only POSIX filesystem and udev APIs, not ALSA.
   ═══════════════════════════════════════════════════════════════════════ */

static int path_join(char *buf, size_t size, const char *dir, const char *suffix)
{
    size_t d = strlen(dir), s = strlen(suffix);
    if (d + 1 + s + 1 > size) return 0;
    memcpy(buf, dir, d);
    buf[d] = '/';
    memcpy(buf + d + 1, suffix, s + 1);
    return 1;
}

static int detect_card_in_sound_dir(const char *sound_dir, char *out, int out_size)
{
    DIR *d = opendir(sound_dir);
    if (!d) return 0;

    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "card", 4) != 0 || ent->d_name[4] < '0')
            continue;

        int card_num = atoi(ent->d_name + 4);
        char id_path[PATH_MAX];
        char id[64] = "";
        snprintf(id_path, sizeof(id_path), "%s/%s/id", sound_dir, ent->d_name);
        FILE *f = fopen(id_path, "r");
        if (f) {
            if (fgets(id, sizeof(id), f))
                id[strcspn(id, "\n")] = '\0';
            fclose(f);
        }

        if (id[0])
            snprintf(out, (size_t)out_size, "hw:CARD=%s,DEV=0", id);
        else
            snprintf(out, (size_t)out_size, "hw:%d,0", card_num);
        found = 1;
        break;
    }
    closedir(d);
    return found;
}

static int detect_alsa_in(const char *base, char *out, int out_size)
{
    char sound_dir[PATH_MAX];
    struct stat st;

    if (path_join(sound_dir, sizeof(sound_dir), base, "sound") &&
        stat(sound_dir, &st) == 0 && S_ISDIR(st.st_mode) &&
        detect_card_in_sound_dir(sound_dir, out, out_size))
        return 1;

    DIR *d = opendir(base);
    if (!d) return 0;

    int found = 0;
    struct dirent *ent;
    char child[PATH_MAX];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!path_join(child, sizeof(child), base, ent->d_name)) continue;
        if (!path_join(sound_dir, sizeof(sound_dir), child, "sound")) continue;
        if (stat(sound_dir, &st) == 0 && S_ISDIR(st.st_mode) &&
            detect_card_in_sound_dir(sound_dir, out, out_size)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int resolve_syslink(const char *link, char *out, int out_size)
{
    char saved[PATH_MAX];
    if (!getcwd(saved, sizeof(saved))) return 0;
    if (chdir(link) != 0) return 0;
    int ok = (getcwd(out, (size_t)out_size) != NULL);
    chdir(saved);
    return ok;
}

static int detect_sysfs(const char *radio_dev, char *out, int out_size)
{
    const char *devname = strrchr(radio_dev, '/');
    devname = devname ? devname + 1 : radio_dev;

    char syslink[PATH_MAX];
    snprintf(syslink, sizeof(syslink), "/sys/class/video4linux/%s", devname);

    char path[PATH_MAX];
    if (!resolve_syslink(syslink, path, sizeof(path))) return 0;

    char *p;
    p = strrchr(path, '/'); if (p) *p = '\0';
    p = strrchr(path, '/'); if (p) *p = '\0';

    char base[PATH_MAX];
    strncpy(base, path, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    for (int level = 0; level < 3; level++) {
        if (detect_alsa_in(base, out, out_size)) return 1;
        p = strrchr(base, '/');
        if (!p) break;
        *p = '\0';
    }
    return 0;
}

#ifdef HAVE_UDEV
static int detect_udev(const char *radio_dev, char *out, int out_size)
{
    struct udev *udev = udev_new();
    if (!udev) return 0;

    struct stat st;
    if (stat(radio_dev, &st) != 0) { udev_unref(udev); return 0; }

    struct udev_device *radio_udev =
        udev_device_new_from_devnum(udev, 'c', st.st_rdev);
    if (!radio_udev) { udev_unref(udev); return 0; }

    struct udev_device *usb = udev_device_get_parent_with_subsystem_devtype(
        radio_udev, "usb", "usb_device");
    if (!usb) {
        udev_device_unref(radio_udev);
        udev_unref(udev);
        return 0;
    }
    const char *usb_syspath = udev_device_get_syspath(usb);

    struct udev_enumerate *en = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(en, "sound");
    udev_enumerate_scan_devices(en);

    int found = 0;
    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
        struct udev_device *snd = udev_device_new_from_syspath(
            udev, udev_list_entry_get_name(entry));
        if (!snd) continue;

        const char *sysname = udev_device_get_sysname(snd);
        if (strncmp(sysname, "card", 4) != 0 || sysname[4] < '0') {
            udev_device_unref(snd);
            continue;
        }

        struct udev_device *snd_usb =
            udev_device_get_parent_with_subsystem_devtype(
                snd, "usb", "usb_device");
        if (snd_usb &&
            strcmp(usb_syspath, udev_device_get_syspath(snd_usb)) == 0) {
            const char *id = udev_device_get_sysattr_value(snd, "id");
            if (id && id[0])
                snprintf(out, (size_t)out_size, "hw:CARD=%s,DEV=0", id);
            else
                snprintf(out, (size_t)out_size, "hw:%d,0", atoi(sysname + 4));
            found = 1;
        }

        udev_device_unref(snd);
        if (found) break;
    }

    udev_enumerate_unref(en);
    udev_device_unref(radio_udev);
    udev_unref(udev);
    return found;
}
#endif /* HAVE_UDEV */

/* ── autodetect ──────────────────────────────────────────────────────── */

int audio_autodetect(const char *radio_dev, char *out, int out_size)
{
    if (out_size > 0) out[0] = '\0';

#ifdef HAVE_PIPEWIRE
    /* Step 1: find the ALSA card identifier via sysfs/udev. */
    char alsa_dev[AUDIO_DEV_NAMELEN] = "";
#ifdef HAVE_UDEV
    if (!detect_udev(radio_dev, alsa_dev, sizeof(alsa_dev)))
#endif
        detect_sysfs(radio_dev, alsa_dev, sizeof(alsa_dev));

    if (!alsa_dev[0]) return 0;

    /* Step 2: extract card id/number from the hw: string. */
    char card_id[64] = "";
    if (strncmp(alsa_dev, "hw:CARD=", 8) == 0) {
        const char *p = alsa_dev + 8;
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len < sizeof(card_id)) {
            memcpy(card_id, p, len);
            card_id[len] = '\0';
        }
    } else if (strncmp(alsa_dev, "hw:", 3) == 0) {
        strncpy(card_id, alsa_dev + 3, sizeof(card_id) - 1);
        char *comma = strchr(card_id, ',');
        if (comma) *comma = '\0';
    }

    if (!card_id[0]) return 0;

    /* Step 3: find the matching PipeWire Audio/Source node. */
    return pw_detect_source(card_id, out, out_size);

#else /* ALSA mode: return the hw: string directly */
#ifdef HAVE_UDEV
    if (detect_udev(radio_dev, out, out_size)) return 1;
#endif
    return detect_sysfs(radio_dev, out, out_size);
#endif /* HAVE_PIPEWIRE */
}
