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

#include <math.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include "audio_renderer.h"
#define SECOND_IN_NSECS 1000000000UL

#define NFORMATS 2     /* set to 4 to enable AAC_LD and PCM:  allowed, but  never seen in real-world use */

static GstClockTime gst_audio_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
const char * format[NFORMATS];

static const gchar *avdec_aac = "avdec_aac";
static const gchar *avdec_alac = "avdec_alac";
static gboolean aac = FALSE;
static gboolean alac = FALSE;
static gboolean render_audio = FALSE;
static gboolean async = FALSE;
static gboolean vsync = FALSE;
static gboolean sync = FALSE;
static double desired_volume = 1.0;
static audio_renderer_event_callback_t event_callback = NULL;
static void *event_userdata = NULL;
static GMutex renderer_mutex;
static audio_renderer_sync_snapshot_t sync_snapshot;
static guint64 pipeline_generation = 0;

typedef struct audio_renderer_s {
    GstElement *appsrc; 
    GstElement *pipeline;
    GstElement *volume;
    GstBus *bus;
    gint expected_eos;
    unsigned char ct;
} audio_renderer_t ;
static audio_renderer_t *renderer_type[NFORMATS];
static audio_renderer_t *renderer = NULL;

/* GStreamer Caps strings for Airplay-defined audio compression types (ct) */

/* ct = 1; linear PCM (uncompressed): 44100/16/2, S16LE */
static const char lpcm_caps[]="audio/x-raw,rate=(int)44100,channels=(int)2,format=S16LE,layout=interleaved";

/* ct = 2; codec_data is ALAC magic cookie:  44100/16/2 spf = 352 */    
static const char alac_caps[] = "audio/x-alac,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)"
                           "00000024""616c6163""00000000""00000160""0010280a""0e0200ff""00000000""00000000""0000ac44";

/* ct = 4; codec_data from MPEG v4 ISO 14996-3 Section 1.6.2.1:  AAC-LC 44100/2 spf = 1024 */
static const char aac_lc_caps[] ="audio/mpeg,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)1210";

/* ct = 8; codec_data from MPEG v4 ISO 14996-3 Section 1.6.2.1: AAC_ELD 44100/2  spf = 480 */
static const char aac_eld_caps[] ="audio/mpeg,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)f8e85000";

static void notify_event(audio_renderer_event_type_t event_type, audio_renderer_t *source,
                         int detail, double rms_db, double peak_db, const char *message) {
    if (event_callback) {
        event_callback(event_userdata, event_type, source ? source->ct : 0,
                       detail, rms_db, peak_db, message);
    }
}

static double first_level_value(const GstStructure *structure, const char *field) {
    const GValue *values = gst_structure_get_value(structure, field);
    if (!values || gst_value_list_get_size(values) == 0) {
        return -INFINITY;
    }
    const GValue *first = gst_value_list_get_value(values, 0);
    return G_VALUE_HOLDS_DOUBLE(first) ? g_value_get_double(first) : -INFINITY;
}

static audio_renderer_t *renderer_from_bus(GstBus *bus) {
    for (int i = 0; i < NFORMATS; i++) {
        if (renderer_type[i] && renderer_type[i]->bus == bus) {
            return renderer_type[i];
        }
    }
    return NULL;
}

static gboolean gstreamer_audio_pipeline_bus_callback(GstBus *bus, GstMessage *message, gpointer userdata) {
    (void) userdata;
    audio_renderer_t *source = renderer_from_bus(bus);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &err, &debug);
        logger_log(logger, LOGGER_ERR, "GStreamer error (audio ct=%u): %s: %s%s%s",
                   source ? source->ct : 0,
                   GST_MESSAGE_SRC_NAME(message),
                   err ? err->message : "unknown",
                   debug ? "; " : "",
                   debug ? debug : "");
        notify_event(AUDIO_RENDERER_EVENT_ERROR, source,
                     err ? err->code : 0, -INFINITY, -INFINITY,
                     err ? err->message : "GStreamer audio error");
        g_clear_error(&err);
        g_free(debug);
        break;
    }
    case GST_MESSAGE_EOS:
        if (source && g_atomic_int_get(&source->expected_eos)) {
            logger_log(logger, LOGGER_DEBUG, "GStreamer expected End-Of-Stream (audio ct=%u)", source->ct);
        } else {
            logger_log(logger, LOGGER_WARNING, "GStreamer unexpected End-Of-Stream (audio ct=%u)",
                       source ? source->ct : 0);
            notify_event(AUDIO_RENDERER_EVENT_UNEXPECTED_EOS, source, 0,
                         -INFINITY, -INFINITY, "unexpected audio EOS");
        }
        break;
    case GST_MESSAGE_ELEMENT: {
        const GstStructure *structure = gst_message_get_structure(message);
        if (structure && gst_structure_has_name(structure, "level")) {
            notify_event(AUDIO_RENDERER_EVENT_OUTPUT, source, 0,
                         first_level_value(structure, "rms"),
                         first_level_value(structure, "peak"), NULL);
        }
        break;
    }
    default:
        break;
    }
    return TRUE;
}

static gboolean check_plugins (void)
{
    gboolean ret;
    GstRegistry *registry;
    const gchar *needed[] = { "app", "libav", "playback", "autodetect", "videoparsersbad",  NULL};
    const gchar *gst[] = {"plugins-base", "libav", "plugins-base", "plugins-good", "plugins-bad", NULL};
    registry = gst_registry_get ();
    ret = TRUE;
    for (int i = 0; i < g_strv_length ((gchar **) needed); i++) {
        GstPlugin *plugin;
        plugin = gst_registry_find_plugin (registry, needed[i]);
        if (!plugin) {
            g_print ("Required gstreamer plugin '%s' not found\n"
                     "Missing plugin is contained in  '[GStreamer 1.x]-%s'\n",needed[i], gst[i]);
            ret = FALSE;
            continue;
        }
        gst_object_unref (plugin);
        plugin = NULL;
    }
    if (ret == FALSE) {
        g_print ("\nif the plugin is installed, but not found, your gstreamer registry may have been corrupted.\n"
                 "to rebuild it when gstreamer next starts, clear your gstreamer cache with:\n"
                 "\"rm -rf ~/.cache/gstreamer-1.0\"\n\n");
    }
    return ret;
}

static gboolean check_plugin_feature (const gchar *needed_feature)
{
    gboolean ret;
    GstPluginFeature *plugin_feature;
    GstRegistry *registry = gst_registry_get ();
    ret = TRUE;

    plugin_feature = gst_registry_find_feature (registry, needed_feature, GST_TYPE_ELEMENT_FACTORY);
    if (!plugin_feature) {
      g_print ("Required gstreamer libav plugin feature '%s' not found:\n\n"
	       "This may be missing because the FFmpeg package used by GStreamer-1.x-libav is incomplete.\n"
	       "(Some distributions provide an incomplete FFmpeg due to License or Patent issues:\n"
	       "in such cases a complete version for that distribution is usually made available elsewhere)\n",
	       needed_feature);
      ret = FALSE;
    } else {
      gst_object_unref (plugin_feature);
      plugin_feature = NULL;
    }
    if (ret == FALSE) {
        g_print ("\nif the plugin feature is installed, but not found, your gstreamer registry may have been corrupted.\n"
                 "to rebuild it when gstreamer next starts, clear your gstreamer cache with:\n"
                 "\"rm -rf ~/.cache/gstreamer-1.0\"\n\n");
    }
    return ret;
}

bool gstreamer_init(){
    gst_init(NULL,NULL);    
    return (bool) check_plugins ();
}

void audio_renderer_init(logger_t *render_logger, const char* audiosink, const bool* audio_sync, const bool* video_sync) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    GstClock *clock = gst_system_clock_obtain();
    g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);

    logger = render_logger;
    desired_volume = 1.0;
    render_audio = FALSE;
    renderer = NULL;
    gst_audio_pipeline_base_time = GST_CLOCK_TIME_NONE;
    memset(&sync_snapshot, 0, sizeof(sync_snapshot));
    pipeline_generation = 0;
    
    aac = check_plugin_feature (avdec_aac);
    alac = check_plugin_feature (avdec_alac);

    for (int i = 0; i < NFORMATS ; i++) {
        renderer_type[i] = (audio_renderer_t *)  calloc(1,sizeof(audio_renderer_t));
        g_assert(renderer_type[i]);
        GString *launch = g_string_new("appsrc name=audio_source ! ");
        g_string_append(launch, "queue ! ");
        switch (i) {
        case 0:    /* AAC-ELD */
        case 2:    /* AAC-LC */
            if (aac) g_string_append(launch, "avdec_aac ! ");
            break;
        case 1:    /* ALAC */
            if (alac) g_string_append(launch, "avdec_alac ! ");
            break;
        case 3:   /*PCM*/
            break;
        default:
            break;
        }
        g_string_append (launch, "audioconvert ! ");
        g_string_append (launch, "audioresample ! ");    /* wasapisink must resample from 44.1 kHz to 48 kHz */
        g_string_append (launch, "volume name=volume ! level name=audio_level post-messages=true interval=100000000 ! ");
        g_string_append (launch, audiosink);
        switch(i) {
        case 1:  /*ALAC*/
            if (*audio_sync) {
                g_string_append (launch, " sync=true");
                async = TRUE;
            } else {
                g_string_append (launch, " sync=false");
                async = FALSE;
            }
            break;
        default:
	    if (*video_sync) {
                g_string_append (launch, " sync=true");
                vsync = TRUE;
            } else {
                g_string_append (launch, " sync=false");
                vsync = FALSE;
            }
            break;
        }
        renderer_type[i]->pipeline  = gst_parse_launch(launch->str, &error);
	if (error) {
          g_error ("gst_parse_launch error (audio %d):\n %s\n", i+1, error->message);
          g_clear_error (&error);
        }

        g_assert (renderer_type[i]->pipeline);
        gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);

        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);
        renderer_type[i]->appsrc = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "audio_source");
        renderer_type[i]->volume = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "volume");
        g_atomic_int_set(&renderer_type[i]->expected_eos, TRUE);
        switch (i) {
        case 0:
            caps =  gst_caps_from_string(aac_eld_caps);
            renderer_type[i]->ct = 8;
            format[i] = "AAC-ELD 44100/2";
            break;
        case 1:
            caps =  gst_caps_from_string(alac_caps);
            renderer_type[i]->ct = 2;
            format[i] = "ALAC 44100/16/2";
            break;
        case 2:
            caps =  gst_caps_from_string(aac_lc_caps);
            renderer_type[i]->ct = 4;
            format[i] = "AAC-LC 44100/2";
            break;
        case 3:
            caps =  gst_caps_from_string(lpcm_caps);
            renderer_type[i]->ct = 1;
            format[i] = "PCM 44100/16/2 S16LE";
            break;
        default:
            break;
        }
        logger_log(logger, LOGGER_DEBUG, "Audio format %d: %s",i+1,format[i]);
        logger_log(logger, LOGGER_DEBUG, "GStreamer audio pipeline %d: \"%s\"", i+1, launch->str);
        g_string_free(launch, TRUE);
        g_object_set(renderer_type[i]->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
        gst_caps_unref(caps);
    }
    gst_object_unref(clock);
}

void audio_renderer_set_event_callback(audio_renderer_event_callback_t callback, void *userdata) {
    g_mutex_lock(&renderer_mutex);
    event_callback = callback;
    event_userdata = userdata;
    g_mutex_unlock(&renderer_mutex);
}

void audio_renderer_stop() {
    g_mutex_lock(&renderer_mutex);
    if (renderer) {
        audio_renderer_t *stopping = renderer;
        g_atomic_int_set(&stopping->expected_eos, TRUE);
        gst_app_src_end_of_stream(GST_APP_SRC(stopping->appsrc));
        gst_bus_set_flushing(stopping->bus, TRUE);
        gst_element_set_state(stopping->pipeline, GST_STATE_READY);
        gst_element_set_state(stopping->pipeline, GST_STATE_NULL);
        gst_bus_set_flushing(stopping->bus, FALSE);
        gst_audio_pipeline_base_time = GST_CLOCK_TIME_NONE;
        memset(&sync_snapshot, 0, sizeof(sync_snapshot));
        sync_snapshot.generation = pipeline_generation;
        renderer = NULL;
    }
    render_audio = FALSE;
    g_mutex_unlock(&renderer_mutex);
}

static void get_renderer_type(unsigned char *ct, int *id) {
    render_audio = FALSE;
    *id = -1;
    for (int i = 0; i < NFORMATS; i++) {
        if (renderer_type[i]->ct == *ct) {
	    *id = i;
            break;
        }
    }
    switch (*id) {
    case 2:
    case 0:
        if (aac) {
            render_audio = TRUE;
        } else {
            logger_log(logger, LOGGER_INFO, "*** GStreamer libav plugin feature avdec_aac is missing, cannot decode AAC audio");
        }
        sync = vsync;
        break;
    case 1:
        if (alac) {
            render_audio = TRUE;
        } else {
            logger_log(logger, LOGGER_INFO, "*** GStreamer libav plugin feature avdec_alac is missing, cannot decode ALAC audio");
        }
        sync = async;
        break;
    case 3:
        render_audio = TRUE;
	sync = FALSE;
        break;
    default:
        break;
    }
}

bool audio_renderer_start(unsigned char *ct) {
    int id = -1;
    GstStateChangeReturn state_result = GST_STATE_CHANGE_FAILURE;
    audio_renderer_t *starting = NULL;

    g_mutex_lock(&renderer_mutex);
    get_renderer_type(ct, &id);
    if (id >= 0 && renderer) {
        if(*ct != renderer->ct) {
            audio_renderer_t *stopping = renderer;
            g_atomic_int_set(&stopping->expected_eos, TRUE);
            gst_app_src_end_of_stream(GST_APP_SRC(stopping->appsrc));
            gst_bus_set_flushing(stopping->bus, TRUE);
            gst_element_set_state(stopping->pipeline, GST_STATE_READY);
            gst_element_set_state(stopping->pipeline, GST_STATE_NULL);
            gst_bus_set_flushing(stopping->bus, FALSE);
            logger_log(logger, LOGGER_INFO, "changed audio connection, format %s", format[id]);
            renderer = renderer_type[id];
        } else {
            g_mutex_unlock(&renderer_mutex);
            return render_audio;
        }
    } else if (id >= 0) {
        logger_log(logger, LOGGER_INFO, "start audio connection, format %s", format[id]);
        renderer = renderer_type[id];
    } else {
        logger_log(logger, LOGGER_ERR, "unknown audio compression type ct = %d", *ct);
        g_mutex_unlock(&renderer_mutex);
        return false;
    }

    starting = renderer;
    if (!render_audio || !starting) {
        renderer = NULL;
        g_mutex_unlock(&renderer_mutex);
        return false;
    }

    gst_bus_set_flushing(starting->bus, FALSE);
    g_atomic_int_set(&starting->expected_eos, TRUE);
    g_object_set(starting->volume, "volume", desired_volume, NULL);
    state_result = gst_element_set_state(starting->pipeline, GST_STATE_PLAYING);
    if (state_result == GST_STATE_CHANGE_FAILURE) {
        logger_log(logger, LOGGER_ERR, "GStreamer audio pipeline failed to enter PLAYING (ct=%u)", starting->ct);
        gst_element_set_state(starting->pipeline, GST_STATE_NULL);
        renderer = NULL;
        render_audio = FALSE;
        g_mutex_unlock(&renderer_mutex);
        notify_event(AUDIO_RENDERER_EVENT_START_FAILURE, starting,
                     (int) state_result, -INFINITY, -INFINITY,
                     "audio pipeline failed to enter PLAYING");
        return false;
    }
    gst_audio_pipeline_base_time = gst_element_get_base_time(starting->appsrc);
    pipeline_generation++;
    memset(&sync_snapshot, 0, sizeof(sync_snapshot));
    sync_snapshot.generation = pipeline_generation;
    g_atomic_int_set(&starting->expected_eos, FALSE);
    g_mutex_unlock(&renderer_mutex);
    return true;
}

bool audio_renderer_render_buffer(unsigned char* data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time) {
    GstBuffer *buffer;
    bool valid;
    GstFlowReturn flow_result = GST_FLOW_ERROR;
    audio_renderer_t *active = NULL;

    (void) seqnum;
    g_mutex_lock(&renderer_mutex);

    if (!render_audio || renderer == NULL || data == NULL || data_len == NULL || *data_len <= 0) {
        g_mutex_unlock(&renderer_mutex);
        return false;
    }
    active = renderer;

    GstClockTime pts = (GstClockTime) *ntp_time ;    /* now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (sync) {
        if (pts >= gst_audio_pipeline_base_time) {
            pts -= gst_audio_pipeline_base_time;
        } else {
            logger_log(logger, LOGGER_ERR, "*** invalid ntp_time < gst_audio_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_audio_pipeline_base_time) / SECOND_IN_NSECS);
            g_mutex_unlock(&renderer_mutex);
            return false;
        }
    }

    /* all audio received seems to be either ct = 8 (AAC_ELD 44100/2 spf 460 ) AirPlay Mirror protocol *
     * or ct = 2 (ALAC 44100/16/2 spf 352) AirPlay protocol.                                           *
     * first byte data[0] of ALAC frame is 0x20,                                                       *
     * first byte of AAC_ELD is 0x8c, 0x8d or 0x8e: 0x100011(00,01,10) in modern devices               *
     *                   but is 0x80, 0x81 or 0x82: 0x100000(00,01,10) in ios9, ios10 devices          *
     * first byte of AAC_LC should be 0xff (ADTS) (but has never been  seen).                          */
    
    buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
    g_assert(buffer != NULL);
    //g_print("audio latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
    if (sync) {
        GST_BUFFER_PTS(buffer) = pts;
    }
    gst_buffer_fill(buffer, 0, data, *data_len);
    switch (active->ct){
    case 8: /*AAC-ELD*/
        switch (data[0]){
        case 0x8c:
        case 0x8d:
        case 0x8e:
        case 0x80:
        case 0x81:
        case 0x82:
            valid = true;
            break;          
        default:
            valid = false;
            break;
        }
        break;
    case 2: /*ALAC*/
        valid = (data[0] == 0x20);
        break;
    case 4:  /*AAC_LC */
        valid = (data[0] == 0xff );
 	break;
    default:
        valid = true;
        break;
    }
    if (valid) {
        flow_result = gst_app_src_push_buffer(GST_APP_SRC(active->appsrc), buffer);
        if (flow_result == GST_FLOW_OK) {
            GstClockTime running = GST_CLOCK_TIME_NONE;
            GstClock *clock = gst_element_get_clock(active->pipeline);
            if (clock) {
                const GstClockTime now = gst_clock_get_time(clock);
                gst_object_unref(clock);
                if (GST_CLOCK_TIME_IS_VALID(now) &&
                    GST_CLOCK_TIME_IS_VALID(gst_audio_pipeline_base_time) &&
                    now >= gst_audio_pipeline_base_time) {
                    running = now - gst_audio_pipeline_base_time;
                }
            }
            sync_snapshot.valid = TRUE;
            sync_snapshot.sample_monotonic_us = (uint64_t) g_get_monotonic_time();
            sync_snapshot.converted_ntp_ns = *ntp_time;
            sync_snapshot.pipeline_base_ns = gst_audio_pipeline_base_time;
            sync_snapshot.running_time_ns = running;
            sync_snapshot.submitted_pts_ns = sync ? pts : GST_CLOCK_TIME_NONE;
            sync_snapshot.target_clock_ns =
                sync && G_MAXUINT64 - gst_audio_pipeline_base_time >= pts
                    ? gst_audio_pipeline_base_time + pts
                    : GST_CLOCK_TIME_NONE;
            sync_snapshot.generation = pipeline_generation;
        }
        g_mutex_unlock(&renderer_mutex);
        if (flow_result != GST_FLOW_OK) {
            logger_log(logger, LOGGER_ERR,
                       "GStreamer audio appsrc push failed: ct=%u seq=%u flow=%s (%d)",
                       active->ct, seqnum ? *seqnum : 0,
                       gst_flow_get_name(flow_result), (int) flow_result);
            notify_event(AUDIO_RENDERER_EVENT_PUSH_FAILURE, active,
                         (int) flow_result, -INFINITY, -INFINITY,
                         gst_flow_get_name(flow_result));
            return false;
        }
        return true;
    } else {
        logger_log(logger, LOGGER_ERR, "*** ERROR invalid  audio frame (compression_type %d) skipped ", active->ct);
        logger_log(logger, LOGGER_ERR, "***       first byte of invalid frame was  0x%2.2x ", (unsigned int) data[0]);
        gst_buffer_unref(buffer);
        g_mutex_unlock(&renderer_mutex);
        return false;
    }
}

bool audio_renderer_get_sync_snapshot(audio_renderer_sync_snapshot_t *snapshot) {
    if (!snapshot) {
        return false;
    }
    g_mutex_lock(&renderer_mutex);
    *snapshot = sync_snapshot;
    g_mutex_unlock(&renderer_mutex);
    return snapshot->valid;
}

void audio_renderer_set_volume(double volume) {
    volume = (volume > 10.0) ? 10.0 : volume;
    volume = (volume < 0.0) ? 0.0 : volume;
    g_mutex_lock(&renderer_mutex);
    desired_volume = volume;
    if (renderer && renderer->volume) {
        g_object_set(renderer->volume, "volume", desired_volume, NULL);
    }
    g_mutex_unlock(&renderer_mutex);
}

void audio_renderer_flush() {
}

void audio_renderer_destroy() {
    audio_renderer_stop();
    audio_renderer_set_event_callback(NULL, NULL);
    for (int i = 0; i < NFORMATS ; i++ ) {
        gst_object_unref (renderer_type[i]->bus);
        renderer_type[i]->bus = NULL;
        gst_object_unref (renderer_type[i]->volume);
	renderer_type[i]->volume = NULL;
        gst_object_unref (renderer_type[i]->appsrc);
        renderer_type[i]->appsrc = NULL;
	gst_object_unref (renderer_type[i]->pipeline);
        renderer_type[i]->pipeline = NULL;
        free(renderer_type[i]);
        renderer_type[i] = NULL;
    }
}

unsigned int audio_renderer_listen(void *loop, int id) {
    (void) loop;
    g_assert(id >= 0 && id < NFORMATS);
    g_assert(renderer_type[id] && renderer_type[id]->bus);
    return (unsigned int) gst_bus_add_watch(renderer_type[id]->bus,
                                            gstreamer_audio_pipeline_bus_callback,
                                            NULL);
}
