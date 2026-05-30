#pragma once
#ifdef HAVE_LAME

#include <lame/lame.h>
#include <stdio.h>

/* Worst-case lame output: 1.25 * nsamples + 7200; sized for 4096-frame periods. */
#define RECORD_MP3_BUF 16384

typedef struct {
    lame_global_flags *lame;
    FILE              *fp;
    unsigned char      mp3buf[RECORD_MP3_BUF];
    int                in_channels;  /* channels of PCM data being fed */
} Record;

/*
 * Open a new MP3 recording.
 *   in_rate/in_channels — format of PCM that will be passed to record_feed()
 *   out_rate            — output sample rate (0 = match in_rate)
 *   out_channels        — 1=mono 2=stereo (downmixes if in_channels=2 and out=1)
 *   bitrate             — kbps, e.g. 128
 * Returns NULL on error, filling errmsg.
 */
Record *record_open(const char *path,
                    int in_rate, int in_channels,
                    int out_rate, int out_channels, int bitrate,
                    char *errmsg, int errmsg_size);

/* Feed interleaved S16_LE frames (samples-per-channel = frames). */
void record_feed(Record *r, const short *pcm, int frames);

/* Flush, finalize MP3, close file, free r. */
void record_close(Record *r);

#endif /* HAVE_LAME */
