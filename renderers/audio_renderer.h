/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifndef AUDIO_RENDERER_H
#define AUDIO_RENDERER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "../lib/logger.h"

typedef enum audio_renderer_event_type_e {
    AUDIO_RENDERER_EVENT_ERROR = 1,
    AUDIO_RENDERER_EVENT_UNEXPECTED_EOS = 2,
    AUDIO_RENDERER_EVENT_OUTPUT = 3,
    AUDIO_RENDERER_EVENT_START_FAILURE = 4,
    AUDIO_RENDERER_EVENT_PUSH_FAILURE = 5
} audio_renderer_event_type_t;

typedef void (*audio_renderer_event_callback_t)(
    void *userdata,
    audio_renderer_event_type_t event_type,
    unsigned char compression_type,
    int detail,
    double rms_db,
    double peak_db,
    const char *message);

typedef struct audio_renderer_sync_snapshot_s {
    bool valid;
    uint64_t sample_monotonic_us;
    uint64_t converted_ntp_ns;
    uint64_t pipeline_base_ns;
    uint64_t running_time_ns;
    uint64_t submitted_pts_ns;
    uint64_t target_clock_ns;
    uint64_t generation;
} audio_renderer_sync_snapshot_t;

bool gstreamer_init();
void audio_renderer_init(logger_t *logger, const char* audiosink, const bool *audio_sync, const bool *video_sync);
void audio_renderer_set_event_callback(audio_renderer_event_callback_t callback, void *userdata);
bool audio_renderer_start(unsigned char* compression_type);
void audio_renderer_stop();
bool audio_renderer_render_buffer(unsigned char* data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time);
bool audio_renderer_get_sync_snapshot(audio_renderer_sync_snapshot_t *snapshot);
void audio_renderer_set_volume(double volume);
void audio_renderer_flush();
void audio_renderer_destroy();
unsigned int audio_renderer_listen(void *loop, int id);

#ifdef __cplusplus
}
#endif

#endif //AUDIO_RENDERER_H
