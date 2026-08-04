/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-24 F. Duncanh
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

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <string.h>
#include "video_renderer.h"
#include "../av_sync_strategy.h"
#include "shared_texture_bridge.h"
#include "native_window.h"
#ifdef _WIN32
#include <gst/video/videooverlay.h>
#endif

#define SECOND_IN_NSECS 1000000000UL

/*
 * Keep AirPlay's realtime clock private.  UxPlay runs in-process with the
 * NearCast HID GStreamer backend, so changing the GstSystemClock singleton
 * here can invalidate timestamps and base-times of an already-running HID
 * pipeline.
 */
static GstClock *uxplay_realtime_clock_obtain(void) {
    return GST_CLOCK(g_object_new(
        GST_TYPE_SYSTEM_CLOCK,
        "clock-type", GST_CLOCK_TYPE_REALTIME,
        NULL));
}
#define SECOND_IN_MICROSECS 1000000
#define GST_VIDEO_LATE_RESYNC_NS 200000000ULL
#define GST_VIDEO_FUTURE_RESYNC_NS 200000000ULL
#define GST_VIDEO_RESYNC_LEAD_NS 10000000ULL
#ifdef X_DISPLAY_FIX
#include <gst/video/navigation.h>
#include "x_display_fix.h"
static bool fullscreen = false;
static bool alt_keypress = false;
static unsigned char X11_search_attempts;
#endif

static GstClockTime gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
static unsigned short width, height, width_source, height_source;  /* not currently used */
static bool first_packet = false;
static bool sync = false;
static bool auto_videosink = true;
static bool hls_video = false;
#ifdef X_DISPLAY_FIX
static bool use_x11 = false;
#endif
static bool logger_debug = false;
static bool video_terminate = false;
static gint64 hls_requested_start_position = 0;
static gint64 hls_seek_start = 0;
static gint64 hls_seek_end = 0;
static gint64 hls_duration;
static gboolean hls_seek_enabled;
static gboolean hls_playing;
static gboolean hls_buffer_empty;
static gboolean hls_buffer_full;
static guint64 debug_video_packet_count = 0;
static GstClockTime debug_last_pts = GST_CLOCK_TIME_NONE;
static bool gst_video_pts_rebase_pending = true;
static bool gst_video_pts_rebase_active = false;
static GstClockTime gst_video_pts_anchor_in = GST_CLOCK_TIME_NONE;
static GstClockTime gst_video_pts_anchor_out = GST_CLOCK_TIME_NONE;
static video_renderer_sync_snapshot_t sync_snapshot;
static guint64 video_rebase_count = 0;

static void video_renderer_note_rebase(video_renderer_rebase_reason_t reason,
                                       GstClockTime old_pts,
                                       GstClockTime new_pts) {
    video_rebase_count++;
    sync_snapshot.rebase_count = video_rebase_count;
    sync_snapshot.last_rebase_reason = reason;
    sync_snapshot.last_rebase_old_pts_ns = old_pts;
    sync_snapshot.last_rebase_new_pts_ns = new_pts;
    sync_snapshot.last_rebase_correction_ns = old_pts >= new_pts
        ? old_pts - new_pts
        : new_pts - old_pts;
}


typedef enum {
  //GST_PLAY_FLAG_VIDEO         = (1 << 0),
  //GST_PLAY_FLAG_AUDIO         = (1 << 1),
  //GST_PLAY_FLAG_TEXT          = (1 << 2),
  //GST_PLAY_FLAG_VIS           = (1 << 3),
  //GST_PLAY_FLAG_SOFT_VOLUME   = (1 << 4),
  //GST_PLAY_FLAG_NATIVE_AUDIO  = (1 << 5),
  //GST_PLAY_FLAG_NATIVE_VIDEO  = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD      = (1 << 7),
  GST_PLAY_FLAG_BUFFERING     = (1 << 8),
  //GST_PLAY_FLAG_DEINTERLACE   = (1 << 9),
  //GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10),
  //GST_PLAY_FLAG_FORCE_FILTERS = (1 << 11),
  //GST_PLAY_FLAG_FORCE_SW_DECODERS = (1 << 12),
} GstPlayFlags;

static void video_renderer_reset_sync_state() {
    gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
    debug_video_packet_count = 0;
    debug_last_pts = GST_CLOCK_TIME_NONE;
    gst_video_pts_rebase_pending = true;
    gst_video_pts_rebase_active = false;
    gst_video_pts_anchor_in = GST_CLOCK_TIME_NONE;
    gst_video_pts_anchor_out = GST_CLOCK_TIME_NONE;
    memset(&sync_snapshot, 0, sizeof(sync_snapshot));
    sync_snapshot.sync_strategy = UXPLAY_AV_SYNC_STRATEGY;
    video_rebase_count = 0;
}

#define NCODECS  2   /* renderers for h264 and h265 */

struct video_renderer_s {
    GstElement *appsrc, *pipeline;
    GstAppSink *shared_texture_sink;
    GstBus *bus;
    const char *codec;
    bool autovideo;
    int id;
    gboolean terminate;
    gint64 duration;
    gint buffering_level;
#ifdef  X_DISPLAY_FIX
    bool use_x11;
    const char * server_name;
    X11_Window_t * gst_window;
#endif
};

static video_renderer_t *renderer = NULL;
static video_renderer_t *renderer_type[NCODECS] = {0};
static int n_renderers = NCODECS;
static GMutex renderer_mutex;
static bool renderers_started = false;
static shared_texture_bridge_t *shared_texture_bridge = NULL;
#ifdef _WIN32
static bool use_native_window = false;
#endif
static char h264[] = "h264";
static char h265[] = "h265";
static char hls[] = "hls";

static GstFlowReturn on_shared_texture_sample(GstAppSink *sink, gpointer user_data) {
    (void) user_data;
    if (!shared_texture_bridge) {
        GstSample *sample = gst_app_sink_pull_sample(sink);
        if (sample) {
            gst_sample_unref(sample);
        }
        return GST_FLOW_OK;
    }
    return shared_texture_bridge_on_new_sample(shared_texture_bridge, sink);
}

#ifdef _WIN32
static void bind_native_window_to_sink(video_renderer_t *renderer_instance, const char *videosink) {
    if (!use_native_window || !renderer_instance || !renderer_instance->pipeline || !videosink) {
        return;
    }

    uintptr_t video_handle = native_window_get_video_handle();
    if (!video_handle) {
        logger_log(logger, LOGGER_WARNING, "native window video handle is not available");
        return;
    }

    gchar *sink_name = g_strdup_printf("%s_%s", videosink, renderer_instance->codec);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(renderer_instance->pipeline), sink_name);
    if (!sink) {
        logger_log(logger, LOGGER_WARNING, "native window could not find video sink \"%s\"", sink_name);
        g_free(sink_name);
        return;
    }

    if (GST_IS_VIDEO_OVERLAY(sink)) {
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), (guintptr) video_handle);
        logger_log(logger, LOGGER_INFO, "bound \"%s\" to native video window handle %p", sink_name, (void *) video_handle);
    } else {
        logger_log(logger, LOGGER_WARNING, "video sink \"%s\" does not implement GstVideoOverlay", sink_name);
    }

    gst_object_unref(sink);
    g_free(sink_name);
}
#endif

static void append_videoflip (GString *launch, const videoflip_t *flip, const videoflip_t *rot) {
    /* videoflip image transform */
    switch (*flip) {
    case INVERT:
        switch (*rot)  {
        case LEFT:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
	    break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        default:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_180 ! ");
	    break;
        }
        break;
    case HFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_HORIZ ! ");
            break;
        }
        break;
    case VFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_VERT ! ");
	  break;
	}
        break;
    default:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
            break;
        default:
            break;
        }
        break;
    }
}

/* apple uses colorimetry that is detected as  1:3:7:1           * //previously 1:3:5:1 was seen
 * (not recognized by v4l2 plugin in Gstreamer  < 1.20.4)        *
 * See .../gst-libs/gst/video/video-color.h in gst-plugins-base  *
 * range = 1   -> GST_VIDEO_COLOR_RANGE_0_255      ("full RGB")  * 
 * matrix = 3  -> GST_VIDEO_COLOR_MATRIX_BT709                   *
 * transfer = 7 -> GST_VIDEO_TRANSFER_SRGB                       * // previously GST_VIDEO_TRANSFER_BT709
 * primaries = 1 -> GST_VIDEO_COLOR_PRIMARIES_BT709              *
 * closest used by  GStreamer < 1.20.4 is BT709, 2:3:5:1 with    * // now use sRGB = 1:1:7:1    
 * range = 2 -> GST_VIDEO_COLOR_RANGE_16_235 ("limited RGB")     */  

static const char h264_caps[]="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[]="video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height) {
    width_source = (unsigned short) *f_width_source;
    height_source = (unsigned short) *f_height_source;
    width = (unsigned short) *f_width;
    height = (unsigned short) *f_height;
    logger_log(logger, LOGGER_DEBUG, "begin video stream wxh = %dx%d; source %dx%d", width, height, width_source, height_source);
}

GstElement *make_video_sink(const char *videosink, const char *videosink_options) {
  /* used to build a videosink for playbin, using the user-specified string "videosink" */ 
    GstElement *video_sink = gst_element_factory_make(videosink, "videosink");
    if (!video_sink) {
        return NULL;
    }

    /* process the video_sink_options */
    size_t len = strlen(videosink_options);
    if (!len) {
        return video_sink;
    }

    char *options  = (char *) malloc(len + 1);
    strncpy(options, videosink_options, len + 1);

    /* remove any extension begining with "!" */
    char *end = strchr(options, '!');
    if (end) {   
      *end = '\0';
    }

    /* add any fullscreen options "property=pval" included in string videosink_options*/    
    /* OK to use strtok_r in Windows with MSYS2 (POSIX); use strtok_s for MSVC */
    char *token;
    char *text = options;

    while((token = strtok_r(text, " ", &text))) {
	char *pval = strchr(token, '=');
        if (pval) {
            *pval = '\0';
            pval++;
            const gchar *property_name = (const gchar *) token;
            const gchar *value = (const gchar *) pval;
	    g_print("playbin_videosink property: \"%s\" \"%s\"\n", property_name, value);
	    gst_util_set_object_arg(G_OBJECT (video_sink), property_name, value);
        }
    }
    free(options);
    return video_sink;
}

void  video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options, 
                          bool initial_fullscreen, bool video_sync, bool h265_support, guint playbin_version,
                          const char *uri, uint32_t shared_texture_target_pid) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    bool shared_texture_export_only = false;
    hls_video = (uri != NULL);
    if (shared_texture_bridge) {
        shared_texture_bridge_destroy(shared_texture_bridge);
        shared_texture_bridge = NULL;
    }
    /* videosink choices that are auto */
    auto_videosink = (strstr(videosink, "autovideosink") || strstr(videosink, "fpsdisplaysink"));

    logger = render_logger;
    logger_debug = (logger_get_level(logger) >= LOGGER_DEBUG);
    video_terminate = false;
    hls_seek_enabled = FALSE;
    hls_playing = FALSE;
    hls_seek_start = -1;
    hls_seek_end = -1;
    hls_duration = -1;
    hls_buffer_empty = TRUE;
    hls_buffer_empty = FALSE;
    

    /* this call to g_set_application_name makes server_name appear in the  X11 display window title bar, */
    /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname,server_name))  g_set_application_name(server_name);
    appname = NULL;

    /* the renderer for hls video will only be built if a HLS uri is provided in 
     * the call to video_renderer_init, in which case the h264 and 265 mirror-mode 
     * renderers will not be built.   This is because it appears that we cannot  
     * put playbin into GST_STATE_READY before knowing the uri (?), so cannot use a
     * unified renderer structure with h264, h265 and hls  */  
    if (hls_video) {
        n_renderers = 1;
    } else {
        n_renderers = h265_support ? 2 : 1;
    }

    if (shared_texture_target_pid != 0) {
        if (hls_video) {
            logger_log(logger, LOGGER_WARNING, "shared texture export is only supported for mirror-mode video pipelines");
        } else {
            shared_texture_bridge = shared_texture_bridge_create(logger, shared_texture_target_pid);
            if (shared_texture_bridge) {
                shared_texture_export_only = true;
                logger_log(logger, LOGGER_INFO, "shared texture export enabled for target pid=%" G_GUINT32_FORMAT,
                           shared_texture_target_pid);
                logger_log(logger, LOGGER_INFO,
                           "shared texture export is running in export-only mode; local videosink display is disabled");
            }
        }
    }

#ifdef _WIN32
    use_native_window = native_window_should_embed(videosink, videosink_options, hls_video,
                                                  shared_texture_target_pid);
    if (use_native_window &&
        !native_window_create(logger, server_name, initial_fullscreen)) {
        use_native_window = false;
    }
#endif

    g_assert (n_renderers <= NCODECS);
    for (int i = 0; i < n_renderers; i++) {
        g_assert (i < 2);
        renderer_type[i] = (video_renderer_t *) calloc(1, sizeof(video_renderer_t));
        g_assert(renderer_type[i]);
        renderer_type[i]->autovideo = auto_videosink;
        renderer_type[i]->id = i;
        renderer_type[i]->bus = NULL;
        if (hls_video) {
            /* use playbin3 to play HLS video: replace "playbin3" by "playbin" to use playbin2 */
            switch (playbin_version)  {
            case 2:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin", "hls-playbin2");
                break;
            case 3:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin3", "hls-playbin3");
                break;
            default:
                logger_log(logger, LOGGER_ERR, "video_renderer_init: invalid playbin version %u", playbin_version);
                g_assert(0);
            }
            logger_log(logger, LOGGER_INFO, "Will use GStreamer playbin version %u to play HLS streamed video", playbin_version);	    
            g_assert(renderer_type[i]->pipeline);
            renderer_type[i]->appsrc = NULL;
	    renderer_type[i]->codec = hls;
            /* if we are not using autovideosink, build a videossink based on the string "videosink" */
            if(strcmp(videosink, "autovideosink")) {
                GstElement *playbin_videosink = make_video_sink(videosink, videosink_options);  
                if (!playbin_videosink) {
                    logger_log(logger, LOGGER_ERR, "video_renderer_init: failed to create playbin_videosink");
                } else {
                    logger_log(logger, LOGGER_DEBUG, "video_renderer_init: create playbin_videosink at %p", playbin_videosink);
                    g_object_set(G_OBJECT (renderer_type[i]->pipeline), "video-sink", playbin_videosink, NULL);
                }
            }
            gint flags;
            g_object_get(renderer_type[i]->pipeline, "flags", &flags, NULL);
            flags |= GST_PLAY_FLAG_DOWNLOAD;
	    flags |= GST_PLAY_FLAG_BUFFERING;    // set by default in playbin3, but not in playbin2; is it needed?
            g_object_set(renderer_type[i]->pipeline, "flags", flags, NULL);
	    g_object_set (G_OBJECT (renderer_type[i]->pipeline), "uri", uri, NULL);
        } else {
            switch (i) {
            case 0:
                renderer_type[i]->codec = h264;
                caps = gst_caps_from_string(h264_caps);
                break;
            case 1:
                renderer_type[i]->codec = h265;
                caps = gst_caps_from_string(h265_caps);
                break;
            default:
                g_assert(0);
            }
            GString *launch = g_string_new("appsrc name=video_source ! ");
            g_string_append(launch, "queue ! ");
            g_string_append(launch, parser);
            g_string_append(launch, " ! ");
            g_string_append(launch, decoder);
            g_string_append(launch, " ! ");
            append_videoflip(launch, &videoflip[0], &videoflip[1]);
            
            if (shared_texture_export_only) {
                g_string_append(launch, "d3d11convert ! ");
                g_string_append(launch, "video/x-raw(memory:D3D11Memory),format=BGRA ! ");
                g_string_append(launch, "appsink name=shared_texture_sink_");
                g_string_append(launch, renderer_type[i]->codec);
                g_string_append(launch, " sync=false qos=true max-buffers=1 drop=true");
            } else {
                g_string_append(launch, converter);
                g_string_append(launch, " ! ");
                g_string_append(launch, "videoscale ! ");
                g_string_append(launch, videosink);
                g_string_append(launch, " name=");
                g_string_append(launch, videosink);
                g_string_append(launch, "_");
                g_string_append(launch, renderer_type[i]->codec);
                g_string_append(launch, videosink_options);
            }
            if (video_sync) {
                g_string_append(launch, " sync=true");
                sync = true;
            } else {
                g_string_append(launch, " sync=false");
                sync = false;
            }

            if (!strcmp(renderer_type[i]->codec, h264)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h265))){
                    pos +=3;
                    *pos = '4';
                }
            } else if (!strcmp(renderer_type[i]->codec, h265)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h264))){
                    pos +=3;
                    *pos = '5';
                }
            }

            logger_log(logger, LOGGER_DEBUG, "GStreamer video pipeline %d:\n\"%s\"", i + 1, launch->str);
            renderer_type[i]->pipeline = gst_parse_launch(launch->str, &error);
            if (error) {
                logger_log(logger, LOGGER_ERR, "GStreamer gst_parse_launch failed to create video pipeline %d\n"
                           "*** error message from gst_parse_launch was:\n%s\n"
                           "launch string parsed was \n[%s]", i + 1, error->message, launch->str);
		if (strstr(error->message, "no element")) {
                    logger_log(logger, LOGGER_ERR, "This error usually means that a uxplay option was mistyped\n"
                               "           or some requested part of GStreamer is not installed\n");
                }
                g_clear_error (&error);
            }
            g_assert (renderer_type[i]->pipeline);

            GstClock *clock = uxplay_realtime_clock_obtain();
            gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);
            renderer_type[i]->appsrc = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "video_source");
            g_assert(renderer_type[i]->appsrc);
            renderer_type[i]->shared_texture_sink = NULL;

#ifdef _WIN32
            bind_native_window_to_sink(renderer_type[i], videosink);
#endif

            if (shared_texture_bridge) {
                gchar *shared_texture_sink_name =
                    g_strdup_printf("shared_texture_sink_%s", renderer_type[i]->codec);
                GstElement *shared_texture_sink_element =
                    gst_bin_get_by_name(GST_BIN(renderer_type[i]->pipeline), shared_texture_sink_name);
                if (!shared_texture_sink_element || !GST_IS_APP_SINK(shared_texture_sink_element)) {
                    logger_log(logger, LOGGER_ERR,
                               "video_renderer_init: failed to find shared texture appsink \"%s\"",
                               shared_texture_sink_name);
                } else {
                    renderer_type[i]->shared_texture_sink = GST_APP_SINK(shared_texture_sink_element);
                    gst_app_sink_set_emit_signals(renderer_type[i]->shared_texture_sink, TRUE);
                    gst_app_sink_set_drop(renderer_type[i]->shared_texture_sink, TRUE);
                    gst_app_sink_set_max_buffers(renderer_type[i]->shared_texture_sink, 1);
                    g_signal_connect(renderer_type[i]->shared_texture_sink, "new-sample",
                                     G_CALLBACK(on_shared_texture_sample), renderer_type[i]);
                }
                g_free(shared_texture_sink_name);
            }

            g_object_set(renderer_type[i]->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
            g_string_free(launch, TRUE);
            gst_caps_unref(caps);
	    gst_object_unref(clock);
        }	
#ifdef X_DISPLAY_FIX
        use_x11 = (!shared_texture_export_only) &&
                  (strstr(videosink, "xvimagesink") || strstr(videosink, "ximagesink") || auto_videosink);
        fullscreen = initial_fullscreen;
        renderer_type[i]->server_name = server_name;
        renderer_type[i]->gst_window = NULL;
        renderer_type[i]->use_x11 = false;
        X11_search_attempts = 0;
	/* setting char *x11_display_name to NULL means the value is taken from $DISPLAY in the environment 
         * (a uxplay option to specify a different value is possible)  */
	char *x11_display_name = NULL;
        if (use_x11) {
            if (i == 0) {
                renderer_type[0]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[0]->gst_window);
                get_X11_Display(renderer_type[0]->gst_window, x11_display_name);
                if (renderer_type[0]->gst_window->display) {
                    renderer_type[i]->use_x11 = true;
                } else {
                    free(renderer_type[0]->gst_window);
                    renderer_type[0]->gst_window = NULL;
                }
            } else if (renderer_type[0]->use_x11) {
                renderer_type[i]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[i]->gst_window);
                memcpy(renderer_type[i]->gst_window, renderer_type[0]->gst_window, sizeof(X11_Window_t));
                renderer_type[i]->use_x11 = true;
            }
        }
#endif
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_READY);
        GstState state;
        if (gst_element_get_state (renderer_type[i]->pipeline, &state, NULL, 100 * GST_MSECOND)) {
            if (state == GST_STATE_READY) {
                logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer %d", i + 1);
                if (hls_video && i == 0) {
                    renderer = renderer_type[i];
                }
            } else {
                logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer %d", i + 1);
            }
        } else {
            logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer %d", i + 1);
        }
    }
}

void video_renderer_pause() {
    if (!renderer) {
        return;
    }
    logger_log(logger, LOGGER_DEBUG, "video renderer paused");
    gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
}

void video_renderer_resume() {
    if (!renderer) {
        return;
    }
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState state;
    /* wait with timeout 100 msec for pipeline to change state from PAUSED to PLAYING */
    gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
    const gchar *state_name = gst_element_state_get_name(state);
    logger_log(logger, LOGGER_DEBUG, "video renderer resumed: state %s", state_name);
    if (renderer->appsrc) {
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
        gst_video_pts_rebase_pending = true;
        gst_video_pts_rebase_active = false;
        gst_video_pts_anchor_in = GST_CLOCK_TIME_NONE;
        gst_video_pts_anchor_out = GST_CLOCK_TIME_NONE;
    }
}

void video_renderer_start() {
    g_mutex_lock(&renderer_mutex);
    renderers_started = false;
    video_renderer_reset_sync_state();
    GstState state;
    const gchar *state_name;
    if (hls_video) {
        renderer->bus = gst_element_get_bus(renderer->pipeline);
        gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
	gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
	state_name= gst_element_state_get_name(state);
	logger_log(logger, LOGGER_DEBUG, "video renderer_start: state %s", state_name);
        g_mutex_unlock(&renderer_mutex);
        return;
    } 
  /* when not hls, start both h264 and h265 pipelines; will shut down the "wrong" one when we know the codec */
    for (int i = 0; i < n_renderers; i++) {
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);
        gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 0);
        if ((renderer && renderer_type[i] == renderer && state == GST_STATE_PLAYING) || state == GST_STATE_PLAYING) {
            state_name = gst_element_state_get_name(state);
            logger_log(logger, LOGGER_DEBUG, "video renderer_start: renderer %d already %s", i, state_name);
            continue;
        }
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_PAUSED);
        gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 1000 * GST_MSECOND);
        state_name= gst_element_state_get_name(state);
        logger_log(logger, LOGGER_DEBUG, "video renderer_start: renderer %d state %s", i, state_name);
    }
    first_packet = true;
#ifdef X_DISPLAY_FIX
    X11_search_attempts = 0;
#endif
    renderers_started = true;
    g_mutex_unlock(&renderer_mutex);
}

/* used to find any X11 Window used by the playbin (HLS) pipeline after it starts playing. 
*  if use_x11 is true, called every 100 ms after playbin state is READY until the x11 window is found*/
bool waiting_for_x11_window() {
    if (!hls_video) {
        return false;
    }
#ifdef X_DISPLAY_FIX
    if (use_x11 && renderer->gst_window) {
        get_x_window(renderer->gst_window, renderer->server_name);
        if (!renderer->gst_window->window) {
	    return true;    /* window still not found */
        }
    }
    if (fullscreen) {
         set_fullscreen(renderer->gst_window, &fullscreen);
    }
#endif
    return false;
}

uint64_t video_renderer_render_buffer(unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    g_mutex_lock(&renderer_mutex);
    if (!renderer || !renderer->pipeline || !renderer->appsrc) {
        g_mutex_unlock(&renderer_mutex);
        return 0;
    }

    GstBuffer *buffer;
    GstClockTime pts = (GstClockTime) *ntp_time; /*now in nsecs */
    GstClockTime running = GST_CLOCK_TIME_NONE;
    GstClockTime source_pts = GST_CLOCK_TIME_NONE;
    guint64 packet_index = ++debug_video_packet_count;
    gint nals = nal_count ? *nal_count : -1;
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (sync) {
        if (pts >= gst_video_pipeline_base_time) {
            pts -= gst_video_pipeline_base_time;
        } else {
            // adjust timestamps to be >= gst_video_pipeline_base time
            logger_log(logger, LOGGER_DEBUG, "*** invalid ntp_time < gst_video_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            g_mutex_unlock(&renderer_mutex);
            return  (uint64_t)  gst_video_pipeline_base_time - pts;
        }
        if (renderer && renderer->pipeline && gst_video_pipeline_base_time != GST_CLOCK_TIME_NONE) {
            GstClock *clock = gst_element_get_clock(renderer->pipeline);
            if (clock) {
                GstClockTime now = gst_clock_get_time(clock);
                gst_object_unref(clock);
                if (GST_CLOCK_TIME_IS_VALID(now) && now > gst_video_pipeline_base_time) {
                    running = now - gst_video_pipeline_base_time;
                    source_pts = pts;
#if UXPLAY_AV_SYNC_STRATEGY == UXPLAY_AV_SYNC_LEGACY_REBASE
                    if (gst_video_pts_rebase_pending) {
                        GstClockTime old_pts = pts;
                        gst_video_pts_anchor_in = source_pts;
                        gst_video_pts_anchor_out = running + GST_VIDEO_RESYNC_LEAD_NS;
                        gst_video_pts_rebase_pending = false;
                        gst_video_pts_rebase_active = true;
                        pts = gst_video_pts_anchor_out;
                        video_renderer_note_rebase(
                            VIDEO_RENDERER_REBASE_INITIAL, old_pts, pts);
                    } else if (gst_video_pts_rebase_active &&
                               gst_video_pts_anchor_in != GST_CLOCK_TIME_NONE &&
                               gst_video_pts_anchor_out != GST_CLOCK_TIME_NONE) {
                        if (source_pts >= gst_video_pts_anchor_in) {
                            GstClockTime delta = source_pts - gst_video_pts_anchor_in;
                            if (G_MAXUINT64 - gst_video_pts_anchor_out < delta) {
                                pts = G_MAXUINT64;
                            } else {
                                pts = gst_video_pts_anchor_out + delta;
                            }
                        } else {
                            gst_video_pts_rebase_pending = true;
                            gst_video_pts_rebase_active = false;
                            gst_video_pts_anchor_in = GST_CLOCK_TIME_NONE;
                            gst_video_pts_anchor_out = GST_CLOCK_TIME_NONE;
                        }
                    }
                    if (running > pts && (running - pts) > GST_VIDEO_LATE_RESYNC_NS) {
                        GstClockTime old_pts = pts;
                        GstClockTime late_ns = running - pts;
                        pts = running + GST_VIDEO_RESYNC_LEAD_NS;
                        gst_video_pts_anchor_in = source_pts;
                        gst_video_pts_anchor_out = pts;
                        gst_video_pts_rebase_active = true;
                        video_renderer_note_rebase(
                            VIDEO_RENDERER_REBASE_LATE, old_pts, pts);
                        logger_log(logger, LOGGER_WARNING,
                                   "video resync late packet=%" G_GUINT64_FORMAT " skew=%" G_GUINT64_FORMAT " old_pts=%" G_GUINT64_FORMAT " new_pts=%" G_GUINT64_FORMAT " running=%" G_GUINT64_FORMAT,
                                   packet_index, (guint64) late_ns, (guint64) old_pts, (guint64) pts, (guint64) running);
                    } else if (pts > running && (pts - running) > GST_VIDEO_FUTURE_RESYNC_NS) {
                        GstClockTime old_pts = pts;
                        GstClockTime future_ns = pts - running;
                        pts = running + GST_VIDEO_RESYNC_LEAD_NS;
                        gst_video_pts_anchor_in = source_pts;
                        gst_video_pts_anchor_out = pts;
                        gst_video_pts_rebase_active = true;
                        video_renderer_note_rebase(
                            VIDEO_RENDERER_REBASE_FUTURE, old_pts, pts);
                        logger_log(logger, LOGGER_WARNING,
                                   "video resync future packet=%" G_GUINT64_FORMAT " skew=%" G_GUINT64_FORMAT " old_pts=%" G_GUINT64_FORMAT " new_pts=%" G_GUINT64_FORMAT " running=%" G_GUINT64_FORMAT,
                                   packet_index, (guint64) future_ns, (guint64) old_pts, (guint64) pts, (guint64) running);
                    }
#endif
                }
            }
        }
    }
    g_assert(data_len != 0);
    /* first four bytes of valid  h264  video data are 0x00, 0x00, 0x00, 0x01.    *
     * nal_count is the number of NAL units in the data: short SPS, PPS, SEI NALs *
     * may  precede a VCL NAL. Each NAL starts with 0x00 0x00 0x00 0x01 and is    *
     * byte-aligned: the first byte of invalid data (decryption failed) is 0x01   */
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
    } else {
        const bool show_native_window_after_push = first_packet;
        if (first_packet) {
            logger_log(logger, LOGGER_INFO, "Begin streaming to GStreamer video pipeline");
        }
        // if (sync) {
        //     if (GST_CLOCK_TIME_IS_VALID(debug_last_pts) && pts <= debug_last_pts) {
        //         logger_log(logger, LOGGER_WARNING,
        //                    "video push packet=%" G_GUINT64_FORMAT " non-monotonic pts current=%" G_GUINT64_FORMAT
        //                    " previous=%" G_GUINT64_FORMAT " delta=%" G_GINT64_FORMAT " nsec ntp=%" G_GUINT64_FORMAT,
        //                    packet_index, (guint64) pts, (guint64) debug_last_pts,
        //                    (gint64) (pts - debug_last_pts), (guint64) *ntp_time);
        //     }
        //     debug_last_pts = pts;
        // }
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        g_assert(buffer != NULL);
        //g_print("video latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
        if (sync) {
            GST_BUFFER_PTS(buffer) = pts;
        }
        gst_buffer_fill(buffer, 0, data, *data_len);
        GstFlowReturn flow_ret = gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
        if (flow_ret == GST_FLOW_OK) {
            if (show_native_window_after_push) {
                first_packet = false;
#ifdef _WIN32
                if (use_native_window) {
                    native_window_show();
                }
#endif
            }
            sync_snapshot.valid = TRUE;
            sync_snapshot.sample_monotonic_us = (uint64_t) g_get_monotonic_time();
            sync_snapshot.converted_ntp_ns = *ntp_time;
            sync_snapshot.pipeline_base_ns = gst_video_pipeline_base_time;
            sync_snapshot.running_time_ns = running;
            sync_snapshot.source_pts_ns = source_pts;
            sync_snapshot.submitted_pts_ns = sync ? pts : GST_CLOCK_TIME_NONE;
            sync_snapshot.target_clock_ns =
                sync && G_MAXUINT64 - gst_video_pipeline_base_time >= pts
                    ? gst_video_pipeline_base_time + pts
                    : GST_CLOCK_TIME_NONE;
            sync_snapshot.rebase_count = video_rebase_count;
            sync_snapshot.sync_strategy = UXPLAY_AV_SYNC_STRATEGY;
        }
        // if (flow_ret != GST_FLOW_OK) {
        //     logger_log(logger, LOGGER_ERR,
        //                "video push packet=%" G_GUINT64_FORMAT " failed flow=%s(%d) len=%d nal=%d sync=%d ntp=%" G_GUINT64_FORMAT " pts=%" G_GUINT64_FORMAT,
        //                packet_index, gst_flow_get_name(flow_ret), flow_ret, *data_len, nals, sync, (guint64) *ntp_time, (guint64) pts);
        // } else if (logger_debug && (packet_index <= 3 || !(packet_index % 120))) {
        //     logger_log(logger, LOGGER_DEBUG,
        //                "video push packet=%" G_GUINT64_FORMAT " ok len=%d nal=%d sync=%d ntp=%" G_GUINT64_FORMAT " pts=%" G_GUINT64_FORMAT " base=%" G_GUINT64_FORMAT,
        //                packet_index, *data_len, nals, sync, (guint64) *ntp_time, (guint64) pts, (guint64) gst_video_pipeline_base_time);
        // }
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window && !(renderer->gst_window->window) && renderer->use_x11) {
            X11_search_attempts++;
            logger_log(logger, LOGGER_DEBUG, "Looking for X11 UxPlay Window, attempt %d", (int) X11_search_attempts);
            get_x_window(renderer->gst_window, renderer->server_name);
            if (renderer->gst_window->window) {
                logger_log(logger, LOGGER_INFO, "\n*** X11 Windows: Use key F11 or (left Alt)+Enter to toggle full-screen mode\n");
                if (fullscreen) {
                    set_fullscreen(renderer->gst_window, &fullscreen);
                }
            }
        }
#endif
    }
    g_mutex_unlock(&renderer_mutex);
    return 0;
}

bool video_renderer_get_sync_snapshot(video_renderer_sync_snapshot_t *snapshot) {
    if (!snapshot) {
        return false;
    }
    g_mutex_lock(&renderer_mutex);
    *snapshot = sync_snapshot;
    g_mutex_unlock(&renderer_mutex);
    return snapshot->valid;
}

void video_renderer_flush() {
}

void video_renderer_stop() {
    g_mutex_lock(&renderer_mutex);
    renderers_started = false;
    video_renderer_reset_sync_state();
    if (shared_texture_bridge) {
        shared_texture_bridge_set_session_active(shared_texture_bridge, false, "video-renderer-stop");
    }
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_stop");
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        renderer = NULL;
        //gst_element_set_state (renderer->playbin, GST_STATE_NULL);
     }
    g_mutex_unlock(&renderer_mutex);
}

static void video_renderer_destroy_instance(video_renderer_t *renderer) {
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"destroying renderer instance %p", renderer);
        GstState state;
	GstStateChangeReturn ret;
        gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
	logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        if (state != GST_STATE_NULL) {
            if (!hls_video) {
                gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
            }
            ret = gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
	    logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
		       gst_element_state_change_return_get_name(ret));
	    gst_element_get_state(renderer->pipeline, NULL, NULL, 1000 * GST_MSECOND);
        }
        gst_object_unref(renderer->bus);
	if (renderer->appsrc) {
            gst_object_unref (renderer->appsrc);
        }
        if (renderer->shared_texture_sink) {
            gst_object_unref(renderer->shared_texture_sink);
            renderer->shared_texture_sink = NULL;
        }
        gst_object_unref (renderer->pipeline);
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window) {
            free(renderer->gst_window);
            renderer->gst_window = NULL;
        }
#endif
        free (renderer);
        renderer = NULL;
    }
}

void video_renderer_destroy() {
    g_mutex_lock(&renderer_mutex);
    renderers_started = false;
    video_renderer_reset_sync_state();
    renderer = NULL;
    for (int i = 0; i < n_renderers; i++) {
        video_renderer_t *to_destroy = renderer_type[i];
        renderer_type[i] = NULL;
        video_renderer_destroy_instance(to_destroy);
    }
    if (shared_texture_bridge) {
        shared_texture_bridge_destroy(shared_texture_bridge);
        shared_texture_bridge = NULL;
    }
#ifdef _WIN32
    if (use_native_window) {
        native_window_destroy();
        use_native_window = false;
    }
#endif
    g_mutex_unlock(&renderer_mutex);
}

static void get_stream_status_name(GstStreamStatusType type, char *name, size_t len) {
  switch (type) {
  case GST_STREAM_STATUS_TYPE_CREATE:
    strncpy(name, "CREATE", len);
    return;
  case GST_STREAM_STATUS_TYPE_ENTER:
    strncpy(name, "ENTER", len);
    return;
  case GST_STREAM_STATUS_TYPE_LEAVE:
    strncpy(name, "LEAVE", len);
    return;
  case GST_STREAM_STATUS_TYPE_DESTROY:
    strncpy(name, "DESTROY", len);
    return;
  case GST_STREAM_STATUS_TYPE_START:
    strncpy(name, "START", len);
    return;
  case GST_STREAM_STATUS_TYPE_PAUSE:
    strncpy(name, "PAUSE", len);
    return;
  case GST_STREAM_STATUS_TYPE_STOP:
    strncpy(name, "STOP", len);
    return;
  default:
    strncpy(name, "", len);
    return;
  }
}
  
gboolean gstreamer_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {
    GstState old_state, new_state;
    const gchar no_state[] = "";
    const gchar *old_state_name = no_state, *new_state_name = no_state;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STATE_CHANGED) {
        GstState old_state, new_state;
        gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
        old_state_name = gst_element_state_get_name (old_state);
        new_state_name = gst_element_state_get_name (new_state);
    }

    /* identify which pipeline sent the message */ 
    int type = -1;
    for (int i = 0 ; i < n_renderers ; i ++ ) {
        if (renderer_type[i] && renderer_type[i]->bus == bus) {
            type = i;
            break;
        }
    }

    /* if the bus sending the message is not found, the renderer may already have been destroyed */
    if (type == -1) {
        if (logger_debug) {
            g_print("GStreamer(UNKNOWN, now destroyed?) bus message: %s %s %s %s\n",
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }     
        return TRUE;
    }

    if (logger_debug) {
        gchar *name = NULL;
	GstElement *element = NULL;
        gchar type_name[8] = { 0 };
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STREAM_STATUS) {
            GstStreamStatusType type;
            gst_message_parse_stream_status(message, &type, &element);
            name = gst_element_get_name(element);
            get_stream_status_name(type, type_name, 8);
            old_state_name = name;
            new_state_name = type_name;
        }
        gint64 pos = -1;
        if (hls_video) {
            gst_element_query_position (renderer_type[type]->pipeline, GST_FORMAT_TIME, &pos);
        }
        if (GST_CLOCK_TIME_IS_VALID(pos)) {
            g_print("GStreamer %s  bus message %s %s %s %s; position: %" GST_TIME_FORMAT "\n" ,renderer_type[type]->codec,
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name, GST_TIME_ARGS(pos));
        } else {
            g_print("GStreamer %s bus message %s %s %s %s\n", renderer_type[type]->codec,
                    GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }
	if (name) {
	  g_free(name);
	}
    }

    /* monitor hls video position until seek to hls_start_position is achieved */
    if (hls_video && hls_requested_start_position) {
        if (strstr(GST_MESSAGE_SRC_NAME(message), "sink")) {	  
            gint64 pos;
            if (!GST_CLOCK_TIME_IS_VALID(hls_duration)) {
                gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration);
            }
	    gst_element_query_position (renderer_type[type]->pipeline, GST_FORMAT_TIME, &pos);
            //g_print("HLS position %" GST_TIME_FORMAT " requested_start_position %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " %s\n",
            //    GST_TIME_ARGS(pos), GST_TIME_ARGS(hls_requested_start_position), GST_TIME_ARGS(hls_duration),
            //    (hls_seek_enabled ? "seek enabled" : "seek not enabled"));
            if (pos > hls_requested_start_position) {
                hls_requested_start_position = 0;
            }
	    if ( hls_requested_start_position && pos < hls_requested_start_position  && hls_seek_enabled) {
                g_print("***************** seek to hls_requested_start_position %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(hls_requested_start_position));
                if (gst_element_seek_simple (renderer_type[type]->pipeline, GST_FORMAT_TIME,
                                            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, hls_requested_start_position)) {
                    hls_requested_start_position = 0;
                }
	    }
	}
    }

    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_DURATION:
        hls_duration = GST_CLOCK_TIME_NONE;
        break;
    case GST_MESSAGE_BUFFERING:
        if (hls_video) {
            gint percent = -1;
            gst_message_parse_buffering(message, &percent);
            hls_buffer_empty = TRUE;
	    hls_buffer_full = FALSE;
	    if (percent > 0) {
                hls_buffer_empty = FALSE;
                renderer_type[type]->buffering_level = percent;
                logger_log(logger, LOGGER_DEBUG, "Buffering :%d percent done", percent);
                if (percent < 100) {
                    gst_element_set_state (renderer_type[type]->pipeline, GST_STATE_PAUSED);
                } else {
                    hls_buffer_full = TRUE;
                    gst_element_set_state (renderer_type[type]->pipeline, GST_STATE_PLAYING);
                }
            }
        }
	break;      
    case GST_MESSAGE_ERROR: {
        GError *err;
        gchar *debug;
        gboolean flushing;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error: %s %s", GST_MESSAGE_SRC_NAME(message),err->message);
        if (!hls_video && strstr(err->message,"Internal data stream error")) {
            logger_log(logger, LOGGER_INFO,
                     "*** This is a generic GStreamer error that usually means that GStreamer\n"
                     "*** was unable to construct a working video pipeline.\n\n"
                     "*** If you are letting the default autovideosink select the videosink,\n"
                     "*** GStreamer may be trying to use non-functional hardware h264 video decoding.\n"
                     "*** Try using option -avdec to force software decoding or use -vs <videosink>\n"
                     "*** to select a videosink of your choice (see \"man uxplay\").\n\n"
                     "*** Raspberry Pi models 4B and earlier using Video4Linux2 may need \"-bt709\" uxplay option");
        }
	g_error_free (err);
        g_free (debug);
	if (renderer_type[type]->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer_type[type]->appsrc));
	}
        gst_bus_set_flushing(bus, TRUE);
        gst_element_set_state (renderer_type[type]->pipeline, GST_STATE_READY);
        renderer_type[type]->terminate = TRUE;
        g_main_loop_quit( (GMainLoop *) loop);
        break;
    }
    case GST_MESSAGE_EOS:
      /* end-of-stream */
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream");
	if (hls_video) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer_type[type]->pipeline, GST_STATE_READY);
	    renderer_type[type]->terminate = TRUE;
            g_main_loop_quit( (GMainLoop *) loop);
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (hls_video && logger_debug && strstr(GST_MESSAGE_SRC_NAME(message), "hls-playbin")) {
            GstState old_state, new_state;
            gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
            g_print ("****** hls_playbin: Element %s changed state from %s to %s.\n", GST_OBJECT_NAME (message->src),
                     gst_element_state_get_name (old_state),
                     gst_element_state_get_name (new_state));
            if (new_state != GST_STATE_PLAYING) {
                break;
                hls_playing = FALSE;
            }
            hls_playing = TRUE;
            GstQuery *query;
            query = gst_query_new_seeking(GST_FORMAT_TIME);
                if (gst_element_query(renderer->pipeline, query)) {
	        gst_query_parse_seeking (query, NULL, &hls_seek_enabled, &hls_seek_start, &hls_seek_end);
                if (hls_seek_enabled) {
                    g_print ("Seeking is ENABLED from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT "\n",
			     GST_TIME_ARGS (hls_seek_start), GST_TIME_ARGS (hls_seek_end));
                } else {
                    g_print ("Seeking is DISABLED for this stream.\n");
                }
            } else {
                g_printerr ("Seeking query failed.");
            }
            gst_query_unref (query);
        }
        if (renderer_type[type]->autovideo) {
            char *sink = strstr(GST_MESSAGE_SRC_NAME(message), "-actual-sink-");
            if (sink) {
                sink += strlen("-actual-sink-");
		if (strstr(GST_MESSAGE_SRC_NAME(message), renderer_type[type]->codec)) {
                    logger_log(logger, LOGGER_DEBUG, "GStreamer: automatically-selected videosink"
                               " (renderer %d: %s) is \"%ssink\"", renderer_type[type]->id + 1,
                               renderer_type[type]->codec, sink);
#ifdef X_DISPLAY_FIX
                    renderer_type[type]->use_x11 = (strstr(sink, "ximage") || strstr(sink, "xvimage"));
#endif
		    renderer_type[type]->autovideo = false;
                }
            }
        }
        break;
#ifdef  X_DISPLAY_FIX
    case GST_MESSAGE_ELEMENT:
        if (renderer_type[type]->gst_window && renderer_type[type]->gst_window->window) {
            GstNavigationMessageType message_type = gst_navigation_message_get_type (message);
            if (message_type == GST_NAVIGATION_MESSAGE_EVENT) {
                GstEvent *event = NULL;
                if (gst_navigation_message_parse_event (message, &event)) {
                    GstNavigationEventType event_type = gst_navigation_event_get_type (event);
                    const gchar *key;
                    switch (event_type) {
                    case GST_NAVIGATION_EVENT_KEY_PRESS:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if ((strcmp (key, "F11") == 0) || (alt_keypress && strcmp (key, "Return") == 0)) {
                                fullscreen = !(fullscreen);
                                set_fullscreen(renderer_type[type]->gst_window, &fullscreen);
                            } else if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = true;
                            }
                        }
                        break;
                    case GST_NAVIGATION_EVENT_KEY_RELEASE:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = false;
                            }
                        }
                    default:
                        break;
                    }
                }
                if (event) {
                    gst_event_unref (event);
                }
            }
        }
        break;
#endif
    default:
      /* unhandled message */
        break;
    }
    return TRUE;
}

int video_renderer_choose_codec (bool video_is_h265) {
    g_mutex_lock(&renderer_mutex);
    if (!renderers_started) {
        g_mutex_unlock(&renderer_mutex);
        return -1;
    }
    video_renderer_t *renderer_used = NULL;
    video_renderer_t *renderer_unused = NULL;
    g_assert(!hls_video);
    if (n_renderers == 1) {
        if (video_is_h265) {
            logger_log(logger, LOGGER_ERR, "video is h265 but the -h265 option was not used");
            g_mutex_unlock(&renderer_mutex);
            return -1;
	}
        renderer_used = renderer_type[0];
    } else {
        renderer_used = video_is_h265 ? renderer_type[1] : renderer_type[0];
        renderer_unused = video_is_h265 ? renderer_type[0] : renderer_type[1];
    }
    if (renderer_used == NULL) { 
        g_mutex_unlock(&renderer_mutex);
        return -1;
    } else if (renderer_used == renderer) {
        g_mutex_unlock(&renderer_mutex);
        return 0;
    } else if (renderer) {
        g_mutex_unlock(&renderer_mutex);
        return -1;
    }
    gst_element_set_state (renderer_used->pipeline, GST_STATE_PLAYING);
    GstState old_state, new_state;
    if (gst_element_get_state(renderer_used->pipeline, &old_state, &new_state, 500 * GST_MSECOND) == GST_STATE_CHANGE_FAILURE) {
        logger_log(logger, LOGGER_ERR, "video pipeline failed to go into playing state");
        logger_log(logger, LOGGER_INFO, "retrying video pipeline state transition");
        gst_element_set_state(renderer_used->pipeline, GST_STATE_NULL);
        gst_element_get_state(renderer_used->pipeline, NULL, NULL, 500 * GST_MSECOND);
        gst_element_set_state(renderer_used->pipeline, GST_STATE_PAUSED);
        if (gst_element_get_state(renderer_used->pipeline, &old_state, &new_state, 1000 * GST_MSECOND) == GST_STATE_CHANGE_FAILURE) {
            g_mutex_unlock(&renderer_mutex);
            return -1;
        }
        gst_element_set_state(renderer_used->pipeline, GST_STATE_PLAYING);
        if (gst_element_get_state(renderer_used->pipeline, &old_state, &new_state, 1000 * GST_MSECOND) == GST_STATE_CHANGE_FAILURE) {
            g_mutex_unlock(&renderer_mutex);
            return -1;
        }
    }
    renderer = renderer_used;
    logger_log(logger, LOGGER_DEBUG, "video_pipeline state change from %s to %s\n",
               gst_element_state_get_name (old_state),gst_element_state_get_name (new_state));
    gst_video_pipeline_base_time = gst_element_get_base_time(renderer_used->appsrc);
    gst_video_pts_rebase_pending = true;
    gst_video_pts_rebase_active = false;
    gst_video_pts_anchor_in = GST_CLOCK_TIME_NONE;
    gst_video_pts_anchor_out = GST_CLOCK_TIME_NONE;
    if (renderer == renderer_type[1]) {
        logger_log(logger, LOGGER_INFO, "*** video format is h265 high definition (HD/4K) video %dx%d", width, height);
    }
    if (renderer_unused) {
        for (int i = 0; i < n_renderers; i++) {
            if (renderer_type[i] != renderer_unused) {
                continue;
            }
            renderer_type[i] = NULL;
            video_renderer_destroy_instance(renderer_unused);
        }
    }
    g_mutex_unlock(&renderer_mutex);
    return 0;
}

unsigned int video_reset_callback(void * loop) {
    if (video_terminate) {
        video_terminate = false;
        if (renderer->appsrc) {
	    gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
	gboolean flushing = TRUE;
        gst_bus_set_flushing(renderer->bus, flushing);
 	gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
	g_main_loop_quit( (GMainLoop *) loop);
    }
    return (unsigned  int) TRUE;
}

bool video_get_playback_info(double *duration, double *position, float *rate, bool *buffer_empty, bool *buffer_full) {
    gint64 pos = 0;
    GstState state;
    *duration = 0.0;
    *position = -1.0;
    *rate = 0.0f;
    if (!renderer) {
        return true;
    }

    *buffer_empty = (bool) hls_buffer_empty;
    *buffer_full = (bool) hls_buffer_full;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    *rate = 0.0f;
    switch (state) {
    case GST_STATE_PLAYING:
        *rate = 1.0f;
    default:
        break;
    }

    if (!GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        if (!gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration)) {
            return true;
        }
    }
    *duration = ((double) hls_duration) / GST_SECOND;
    if (*duration) {
        if (gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos) &&
                                        GST_CLOCK_TIME_IS_VALID(pos)) {
            *position = ((double) pos) / GST_SECOND;
        }
    }

    logger_log(logger, LOGGER_DEBUG, "********* video_get_playback_info: position %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " %s *********",
               GST_TIME_ARGS (pos), GST_TIME_ARGS (hls_duration), gst_element_state_get_name(state));

    return true;
}

void video_renderer_set_start(float position) {
    int pos_in_micros = (int) (position * SECOND_IN_MICROSECS);
    hls_requested_start_position = (gint64) (pos_in_micros * GST_USECOND);
    logger_log(logger, LOGGER_DEBUG, "register HLS video start position %f %lld", position,
               hls_requested_start_position);    
}

void video_renderer_seek(float position) {
    int pos_in_micros = (int) (position * SECOND_IN_MICROSECS);
    gint64 seek_position = (gint64) (pos_in_micros * GST_USECOND);
    /* don't seek to within 1  microsecond  of beginning or end of video */
    if (hls_duration < 2000) return;
    seek_position =  seek_position < 1000 ? 1000 : seek_position;
    seek_position =  seek_position > hls_duration  - 1000 ? hls_duration - 1000 : seek_position;
    g_print("SCRUB: seek to %f secs =  %" GST_TIME_FORMAT ", duration = %" GST_TIME_FORMAT "\n", position,
            GST_TIME_ARGS(seek_position),  GST_TIME_ARGS(hls_duration));
    gboolean result = gst_element_seek_simple(renderer->pipeline, GST_FORMAT_TIME,
                                              (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                              seek_position);
    if (result) {
        g_print("seek succeeded\n");
        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);	
    } else {
        g_print("seek failed\n");
    }
}

unsigned int video_renderer_listen(void *loop, int id) {
    g_assert(id >= 0 && id < n_renderers);
    return (unsigned int) gst_bus_add_watch(renderer_type[id]->bus,(GstBusFunc)
                                            gstreamer_pipeline_bus_callback, (gpointer) loop);    
}
