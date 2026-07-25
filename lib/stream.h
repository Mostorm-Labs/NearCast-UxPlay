/*
 * Copyright (c) 2019 dsafa22, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *=================================================================
 * modified by fduncanh 2022-2023
 */

#ifndef AIRPLAYSERVER_STREAM_H
#define AIRPLAYSERVER_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include "raop_ntp.h"

typedef struct {
    bool is_h265;
    int nal_count;
    unsigned char *data;
    int data_len;
    uint64_t ntp_time_local;
    uint64_t ntp_time_remote;
} video_decode_struct;

typedef struct {
    unsigned char *data;
    unsigned char ct;
    int data_len;
    int sync_status;
    uint64_t ntp_time_local;
    uint64_t ntp_time_remote;
    uint64_t rtp_time;
    unsigned short seqnum;
    uint64_t resend_request_count;
    uint64_t resend_requested_packet_count;
    uint64_t resend_failure_count;
    uint64_t resent_packet_count;
    uint64_t last_resend_request_local_ns;
    uint64_t last_resend_failure_local_ns;
    uint64_t last_resent_packet_local_ns;
    uint64_t rtp_sync_update_count;
    uint64_t last_rtp_sync_update_local_ns;
    int64_t last_rtp_sync_offset_change_ns;
    raop_ntp_diagnostics_t ntp_diagnostics;
} audio_decode_struct;

#endif //AIRPLAYSERVER_STREAM_H
