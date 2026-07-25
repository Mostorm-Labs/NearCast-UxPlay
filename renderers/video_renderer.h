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

/* 
 * H264 renderer using gstreamer
*/

#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "../lib/logger.h"

typedef enum videoflip_e {
    NONE,
    LEFT,
    RIGHT,
    INVERT,
    VFLIP,
    HFLIP,
} videoflip_t;

typedef struct video_renderer_s video_renderer_t;

typedef enum video_renderer_rebase_reason_e {
    VIDEO_RENDERER_REBASE_NONE = 0,
    VIDEO_RENDERER_REBASE_INITIAL = 1,
    VIDEO_RENDERER_REBASE_LATE = 2,
    VIDEO_RENDERER_REBASE_FUTURE = 3
} video_renderer_rebase_reason_t;

typedef struct video_renderer_sync_snapshot_s {
    bool valid;
    uint64_t sample_monotonic_us;
    uint64_t converted_ntp_ns;
    uint64_t pipeline_base_ns;
    uint64_t running_time_ns;
    uint64_t source_pts_ns;
    uint64_t submitted_pts_ns;
    uint64_t target_clock_ns;
    uint64_t rebase_count;
    uint64_t last_rebase_old_pts_ns;
    uint64_t last_rebase_new_pts_ns;
    uint64_t last_rebase_correction_ns;
    video_renderer_rebase_reason_t last_rebase_reason;
    int sync_strategy;
} video_renderer_sync_snapshot_t;

void video_renderer_init (logger_t *logger, const char *server_name, videoflip_t videoflip[2], const char *parser,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options,
                          bool initial_fullscreen, bool video_sync, bool h265_support, guint playbin_version,
                          const char *uri, uint32_t shared_texture_target_pid);
void video_renderer_start ();
void video_renderer_stop ();
void video_renderer_pause ();
void video_renderer_seek(float position);
void video_renderer_set_start(float position);
void video_renderer_resume ();
bool video_renderer_is_paused();
uint64_t  video_renderer_render_buffer (unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time);
bool video_renderer_get_sync_snapshot(video_renderer_sync_snapshot_t *snapshot);
void video_renderer_flush ();
unsigned int video_renderer_listen(void *loop, int id);
void video_renderer_destroy ();
void video_renderer_size(float *width_source, float *height_source, float *width, float *height);
bool waiting_for_x11_window();
bool video_get_playback_info(double *duration, double *position, float *rate, bool *buffer_empty, bool *buffer_full);
int video_renderer_choose_codec(bool is_h265);
unsigned int video_renderer_listen(void *loop, int id);
unsigned int video_reset_callback(void *loop);
#ifdef __cplusplus
}
#endif

#endif //VIDEO_RENDERER_H
