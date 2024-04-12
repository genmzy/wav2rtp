/*
 * $Id$
 *
 * Copyright (c) 2007, R.Imankulov
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 *  3. Neither the name of the R.Imankulov nor the names of its contributors may
 *  be used to endorse or promote products derived from this software without
 *  specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <sndfile.h>
#include <string.h>
#include "error_types.h"
#include "rtpapi.h"
#include "codecapi.h"
#include "wavfile_filter.h"
#include "options.h"
#include "misc.h"
#include "rtpmap.h"

typedef struct __rtp_packet_wrap {
    int left;
    wr_rtp_packet_t rtp_packet;
    int seq;
    int ts;
    struct timeval low_ts_start;
    struct timeval low_ts_end;
    int frames_count;
    short *buf;
    int bufsize;
    int buf_offset;
} wr_rtp_packet_wrap_t;

typedef struct __global_wrap {
    wr_encoder_t *codec;
    wr_rtp_filter_t *filter;
    SF_INFO *finfo;
    int rtp_in_frame;
} global_wrap_t;

static int get_format_payload_type(int format);
static wr_errorcode_t wr_rtp_packet_wrap_init(int left, wr_rtp_packet_wrap_t *wrap, global_wrap_t *global);
static wr_errorcode_t wr_rtp_packet_assign_buf(wr_rtp_packet_wrap_t *wrap, short *buf, int bufsize);
static void wr_rtp_packet_wrap_handle(wr_rtp_packet_wrap_t *wrap, global_wrap_t *global);
static wr_errorcode_t wr_rtp_packet_fill_short(wr_rtp_packet_wrap_t *wrap, short s);

wr_errorcode_t wr_wavfile_filter_start(wr_rtp_filter_t *filter)
{
    SNDFILE *file = NULL;
    SF_INFO finfo = { 0 };
    global_wrap_t global = { 0 };
    wr_rtp_packet_wrap_t lwrap = { 0 }, rwrap = { 0 };

    global.finfo = &finfo;
    global.filter = filter;

    if (list_empty(wr_options.codec_list)) {
        global.codec = get_encoder_by_pt(get_format_payload_type(finfo.format));
        if (global.codec && !(global.codec->init)(global.codec)) {
            wr_set_error("Cannot initialize codec");
            return WR_FATAL;
        }
    } else {
        list_iterator_start(wr_options.codec_list);
        if (list_iterator_hasnext(wr_options.codec_list)){
            global.codec = (wr_encoder_t *)list_iterator_next(wr_options.codec_list);
        }
    }
    if (!global.codec) {
        wr_set_error("no codec is found");
        return WR_FATAL;
    } else {
        printf("global codecs : %s\n", global.codec->name);
    }

    global.rtp_in_frame = iniparser_getpositiveint(wr_options.output_options, "global:rtp_in_frame", 1);
    file = sf_open(wr_options.filename, SFM_READ, &finfo);
    if (!file) {
        wr_set_error("cannot open or render sound file");
        return WR_FATAL;
    }
    if (finfo.channels > 2 || finfo.channels < 1) {
        wr_set_error("only support 1 or 2 channels to convert do filter");
        return WR_FATAL;
    }
    printf("channels: %d\n", finfo.channels);
    if (finfo.samplerate != 8000) {
        wr_set_error("this tool works only with .wav files in 8kHz, rerecord "
                     "your signal or resample it (with sox, for example; like "
                     "'sox input.wav -r8000 resampled.wav')\n" );
        return WR_FATAL;
    }

    wr_rtp_packet_wrap_init(1, &lwrap, &global);
    wr_rtp_packet_wrap_init(0, &rwrap, &global);

    for ( ; global.codec; ) {
        int bufin_size = (global.codec->get_input_buffer_size)(global.codec->state);
        short bufin[bufin_size * finfo.channels];
        memset(bufin, 0, bufin_size * finfo.channels);

        short lbuf[bufin_size], rbuf[bufin_size];
        wr_rtp_packet_assign_buf(&lwrap, lbuf, bufin_size);
        wr_rtp_packet_assign_buf(&rwrap, rbuf, bufin_size);

        int n = sf_readf_short(file, bufin, bufin_size);
        if (n == 0) {
            wr_rtp_packet_wrap_handle(&lwrap, &global);
            wr_rtp_packet_wrap_handle(&rwrap, &global);
            sf_seek(file, 0, SEEK_SET);
            if (list_iterator_hasnext(wr_options.codec_list)) {
                global.codec = (wr_encoder_t *)list_iterator_next(wr_options.codec_list);
            } else {
                global.codec = NULL;
                list_iterator_stop(wr_options.codec_list);
            }
            continue;
        }
        wr_rtp_packet_wrap_t *wrap;
        for (int k = 0, left = 1; k < n*finfo.channels; ++k) {
            wrap = left ? &lwrap : &rwrap;
            wr_rtp_packet_fill_short(wrap, *(bufin+k));
            left = (finfo.channels == 2 && left) ? 0 : 1;
        }
        wr_rtp_packet_wrap_handle(&lwrap, &global);
        wr_rtp_packet_wrap_handle(&rwrap, &global);
    }

    return WR_OK;
}

static int get_format_payload_type(int format)
{
    switch (format & 0xff) {
        case SF_FORMAT_ULAW: return 0;
        case SF_FORMAT_ALAW: return 8;
        case SF_FORMAT_GSM610: return 3;
    }
    return -1;
}

static wr_errorcode_t wr_rtp_packet_wrap_init(int left, wr_rtp_packet_wrap_t *wrap, global_wrap_t *global)
{
    wrap->left = left;
    gettimeofday(&wrap->low_ts_start, NULL);
    timeval_copy(&wrap->low_ts_end, &wrap->low_ts_start);
    wr_rtp_packet_init(&wrap->rtp_packet, global->codec->payload_type, wrap->seq, 1, wrap->ts, wrap->low_ts_start);
    wr_rtp_filter_notify_observers(global->filter, TRANSMISSION_START, &wrap->rtp_packet, 1);
    return WR_OK;
}

static wr_errorcode_t wr_rtp_packet_assign_buf(wr_rtp_packet_wrap_t *wrap, short *buf, int bufsize)
{
    wrap->buf = buf;
    memset(wrap->buf, 0, bufsize);
    wrap->buf_offset = 0;
    wrap->bufsize = bufsize;
    return WR_OK;
}

static wr_errorcode_t wr_rtp_packet_fill_short(wr_rtp_packet_wrap_t *wrap, short s)
{
    if (wrap->buf_offset >= wrap->bufsize) {
        return WR_WARN;
    }
    *(wrap->buf + wrap->buf_offset) = s;
    wrap->buf_offset++;
    return WR_OK;
}

static void wr_rtp_packet_wrap_handle(wr_rtp_packet_wrap_t *wrap, global_wrap_t *global)
{
    if (!wrap->buf_offset) { /* EOF */
        if (wrap->frames_count) { /* send existed packet and do notify */
            wr_rtp_filter_notify_observers(global->filter, NEW_PACKET, &wrap->rtp_packet, wrap->left);
            wr_rtp_packet_destroy(&wrap->rtp_packet);
            wrap->frames_count = 0;
            wrap->seq++;
            timeval_copy(&wrap->low_ts_start, &wrap->low_ts_end);
            wr_rtp_packet_init(&wrap->rtp_packet, global->codec->payload_type, wrap->seq, 0, wrap->ts, wrap->low_ts_start);
        }
        return;
    }

    int bufout_size = (global->codec->get_output_buffer_size)(global->codec->state);
    char bufout[bufout_size];
    memset(bufout, 0, bufout_size);
    (*global->codec->encode)(global->codec->state, wrap->buf, bufout);

    wr_rtp_packet_add_frame(&wrap->rtp_packet, (unsigned char *)bufout, bufout_size,  1000 * wrap->buf_offset / global->finfo->samplerate);
    timeval_increment(&wrap->low_ts_end, 1e6 * wrap->buf_offset / global->finfo->samplerate);
    wrap->ts += wrap->buf_offset;
    wrap->frames_count++;
    if (wrap->frames_count == global->rtp_in_frame) {
        wr_rtp_filter_notify_observers(global->filter, NEW_PACKET, &wrap->rtp_packet, wrap->left);
        wr_rtp_packet_destroy(&wrap->rtp_packet);
        wrap->frames_count = 0;
        wrap->seq++;
        timeval_copy(&wrap->low_ts_start, &wrap->low_ts_end);
        wr_rtp_packet_init(&wrap->rtp_packet, global->codec->payload_type, wrap->seq, 0, wrap->ts, wrap->low_ts_start);
    }
}
