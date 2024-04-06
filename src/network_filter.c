/*
 * $Id$
 * 
 * Copyright (c) 2010, R.Imankulov, Yu Jiang
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
#ifdef __linux__

#include "error_types.h"
#include "rtpapi.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <netinet/ether.h> /* ether_aton_r */
#include <arpa/inet.h>

#include <pcap.h>

#include "options.h"
#include "network_filter.h"

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

void wr_sleep(struct timespec *spec) {
    struct timespec rem0 = { 0 }, rem1 = { 0 }, *req = spec, *rem = &rem0;
    int next = 0;
    for (;;) {
        memset(rem, 0, sizeof(struct timespec));
        nanosleep(req, rem);
        if (!rem1.tv_nsec && !rem1.tv_sec) {
            break;
        }
        if (next) {
            req = &rem1;
            rem = &rem0;
            next = 0;
        } else {
            req = &rem0;
            rem = &rem1;
            next = 1;
        }
    }
}

static int cp2buf(uint8_t *buf, int offset, const void *st, int lst)
{
    uint8_t *uint8_st = (uint8_t *)st;
    if (offset < 0) {
        offset = 0;
    }
    uint8_t *start = offset+buf;
    for (int i=0; i < lst; ++i) {
        *(start+i) = *(uint8_st+i);
    }
    return lst+offset;
}

wr_errorcode_t wr_network_filter_notify(wr_rtp_filter_t * filter, wr_event_type_t event, wr_rtp_packet_t * packet)
{
    switch(event) {
        case TRANSMISSION_START:
            {
                if (wr_options.output_format != WR_OUTPUT_NETWORK) {
                    return WR_OK;
                }
                wr_network_filter_context_t *ctx = calloc(1, sizeof(wr_network_filter_context_t));
                if (-1 == (ctx->socket_fd = socket(AF_INET, SOCK_DGRAM, 0))) {
                    wr_network_filter_context_safe_free(ctx);
                    return WR_FATAL;
                }
                // remote address
                ctx->server_addr = calloc(1, sizeof(struct sockaddr_in));
                if (!ctx->server_addr) {
                    wr_network_filter_context_safe_free(ctx);
                    return WR_FATAL;
                }
                if (-1 == wr_parse_url(wr_options.output_filename, ctx->server_addr)) {
                    wr_network_filter_context_safe_free(ctx);
                    return WR_FATAL;
                }
                ctx->sended = 0;
                ctx->spec = calloc(1, sizeof(struct timespec));
                ctx->spec->tv_sec = 0;
                ctx->spec->tv_nsec = 20 /* millisecond */ * 1000 /* microsecond */ * 1000 /* nanosecond */;

                filter->state = (void *)ctx;
            }
            break;

        case NEW_PACKET:
            {
                if (wr_options.output_format != WR_OUTPUT_NETWORK) {
                    return WR_OK;
                }
                wr_network_filter_context_t *ctx = (wr_network_filter_context_t *)(filter->state);
                list_iterator_start(&(packet->data_frames));
                wr_rtp_header_t header;
                wr_rtp_header_init(&header, packet);
                uint8_t buf[1024] = { 0 }; // bigger than ethernet data length okay
                while (list_iterator_hasnext(&(packet->data_frames))) {
                    if (ctx->sended) {
                        wr_sleep(ctx->spec);
                    } else {
                        ctx->sended = 1;
                    }
                    /* merge into rtp frame data and send to list */
                    wr_data_frame_t * frame = list_iterator_next(&(packet->data_frames));
                    int lh = sizeof(wr_rtp_header_t);
                    cp2buf(buf, 0, &header, lh);
                    int lp = cp2buf(buf, lh, frame->data, frame->size);
                    sendto(ctx->socket_fd, buf, lp, 0, (struct sockaddr *)(ctx->server_addr), sizeof(struct sockaddr_in));
                }
                list_iterator_stop(&(packet->data_frames));
            }
            break;

        case TRANSMISSION_END:
            {
                if (wr_options.output_format != WR_OUTPUT_NETWORK) {
                    return WR_OK;
                }
                wr_network_filter_context_safe_free(filter->state);
            }
            break;
    }
    return WR_OK;
}

#endif
