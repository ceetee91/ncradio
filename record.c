#include "record.h"
#ifdef HAVE_LAME

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

Record *record_open(const char *path,
                    int in_rate, int in_channels,
                    int out_rate, int out_channels, int bitrate,
                    char *errmsg, int errmsg_size)
{
    Record *r = calloc(1, sizeof(*r));
    if (!r) {
        if (errmsg) snprintf(errmsg, errmsg_size, "out of memory");
        return NULL;
    }

    r->in_channels = in_channels;

    r->lame = lame_init();
    if (!r->lame) {
        if (errmsg) snprintf(errmsg, errmsg_size, "lame_init failed");
        free(r);
        return NULL;
    }

    lame_set_in_samplerate(r->lame, in_rate);
    lame_set_num_channels(r->lame, in_channels);
    if (out_rate > 0 && out_rate != in_rate)
        lame_set_out_samplerate(r->lame, out_rate);
    lame_set_brate(r->lame, bitrate);

    /* Output mode: force mono if either side requests it */
    if (out_channels == 1 || in_channels == 1)
        lame_set_mode(r->lame, MONO);
    else
        lame_set_mode(r->lame, JOINT_STEREO);

    lame_set_quality(r->lame, 5);
    lame_set_errorf(r->lame, NULL);
    lame_set_debugf(r->lame, NULL);
    lame_set_msgf(r->lame, NULL);

    if (lame_init_params(r->lame) < 0) {
        if (errmsg) snprintf(errmsg, errmsg_size, "lame_init_params failed");
        lame_close(r->lame);
        free(r);
        return NULL;
    }

    r->fp = fopen(path, "wb");
    if (!r->fp) {
        if (errmsg) snprintf(errmsg, errmsg_size, "cannot open '%s'", path);
        lame_close(r->lame);
        free(r);
        return NULL;
    }

    return r;
}

void record_feed(Record *r, const short *pcm, int frames)
{
    int n;
    if (r->in_channels == 2)
        n = lame_encode_buffer_interleaved(r->lame, (short *)pcm, frames,
                                           r->mp3buf, RECORD_MP3_BUF);
    else
        n = lame_encode_buffer(r->lame, pcm, pcm, frames,
                               r->mp3buf, RECORD_MP3_BUF);
    if (n > 0)
        fwrite(r->mp3buf, 1, (size_t)n, r->fp);
}

void record_close(Record *r)
{
    if (!r) return;
    int n = lame_encode_flush(r->lame, r->mp3buf, RECORD_MP3_BUF);
    if (n > 0)
        fwrite(r->mp3buf, 1, (size_t)n, r->fp);
    lame_close(r->lame);
    fclose(r->fp);
    free(r);
}

#endif /* HAVE_LAME */
