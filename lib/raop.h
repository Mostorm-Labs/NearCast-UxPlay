/**
 *  Copyright (C) 2012-2015  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *===================================================================
 * modified by fduncanh 2021-23
 */

#ifndef RAOP_H
#define RAOP_H

#include "dnssd.h"
#include "stream.h"
#include "raop_ntp.h"
#include "airplay_video.h"

#if defined (WIN32) && defined(DLL_EXPORT)
# define RAOP_API __declspec(dllexport)
#else
# define RAOP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct raop_s raop_t;

typedef void (*raop_log_callback_t)(void *cls, int level, const char *msg);


typedef struct playback_info_s {
  //char * uuid;
    uint32_t stallcount;
    double duration;
    double position;
    float rate;
    bool ready_to_play;
    bool playback_buffer_empty;
    bool playback_buffer_full;
    bool playback_likely_to_keep_up;
    int num_loaded_time_ranges;
    int num_seekable_time_ranges;
    void *loadedTimeRanges;
    void *seekableTimeRanges;
} playback_info_t;
  
typedef enum video_codec_e {
    VIDEO_CODEC_UNKNOWN,
    VIDEO_CODEC_H264,
    VIDEO_CODEC_H265
} video_codec_t;

#define RAOP_SESSION_SERVICE_API_VERSION 2
/* Borrowed SETUP metadata. Names/device IDs are untrusted claims; only the
 * public key below is proven by a completed pair-verify on this connection. */
typedef struct raop_session_info_s {
    const char *device_id;
    const char *model;
    const char *name;
    bool peer_verified;
    unsigned char peer_public_key[32];
    bool password_authenticated;
} raop_session_info_t;

#define RAOP_CONTROL_DIAGNOSTICS_API_VERSION 1
/* Optional observation only: no input strings, addresses, keys or payload.
 * UNKNOWN paths/methods stay OTHER, never copied from a client. Request means
 * parsed ingress; response means constructed response, not confirmed delivery. */
typedef enum { RAOP_CONTROL_OPENED = 1, RAOP_CONTROL_REQUEST, RAOP_CONTROL_RESPONSE, RAOP_CONTROL_CLOSED } raop_control_stage_t;
typedef enum {
    RAOP_METHOD_NONE, RAOP_METHOD_GET, RAOP_METHOD_POST, RAOP_METHOD_OPTIONS,
    RAOP_METHOD_SETUP, RAOP_METHOD_RECORD, RAOP_METHOD_TEARDOWN, RAOP_METHOD_FLUSH,
    RAOP_METHOD_GET_PARAMETER, RAOP_METHOD_SET_PARAMETER, RAOP_METHOD_PUT, RAOP_METHOD_OTHER
} raop_control_method_t;
typedef enum {
    RAOP_PATH_NONE, RAOP_PATH_INFO, RAOP_PATH_PAIR_SETUP, RAOP_PATH_PAIR_VERIFY,
    RAOP_PATH_FP_SETUP, RAOP_PATH_PAIR_PIN_START, RAOP_PATH_PAIR_SETUP_PIN,
    RAOP_PATH_FEEDBACK, RAOP_PATH_STREAM, RAOP_PATH_SERVER_INFO, RAOP_PATH_FP_SETUP2, RAOP_PATH_OTHER
} raop_control_path_t;
typedef enum { RAOP_PROTOCOL_NONE, RAOP_PROTOCOL_RTSP_1_0, RAOP_PROTOCOL_HTTP_1_1, RAOP_PROTOCOL_OTHER } raop_control_protocol_t;
typedef struct raop_control_diagnostic_s {
    uint64_t request_number; /* Per transport, 0 for opened. NOT CSeq or peer identity. */
    raop_control_stage_t stage;
    raop_control_method_t method;
    raop_control_path_t path;
    raop_control_protocol_t protocol;
    int status; /* 0 except constructed response. */
    bool cseq_present, authorization_present, peer_verified, password_authenticated, media_active;
} raop_control_diagnostic_t;

struct raop_callbacks_s {
    void* cls;

    void  (*audio_process)(void *cls, raop_ntp_t *ntp, audio_decode_struct *data);
    void  (*video_process)(void *cls, raop_ntp_t *ntp, video_decode_struct *data);
    void  (*video_pause)(void *cls);
    void  (*video_resume)(void *cls);
    void  (*conn_feedback) (void *cls);
    void  (*conn_reset) (void *cls, int reason);
    void  (*video_reset) (void *cls);
    void  (*handoff_start) (void *cls);
  
  
    /* Optional but recommended callback functions (probably not optional, check this)*/
    void  (*conn_init)(void *cls);
    void  (*conn_destroy)(void *cls);
    void  (*conn_teardown)(void *cls, bool *teardown_96, bool *teardown_110 );
    void  (*audio_flush)(void *cls);
    void  (*video_flush)(void *cls);
    double (*audio_set_client_volume)(void *cls);
    void  (*audio_set_volume)(void *cls, float volume);
    void  (*audio_set_metadata)(void *cls, const void *buffer, int buflen);
    void  (*audio_set_coverart)(void *cls, const void *buffer, int buflen);
    void  (*audio_remote_control_id)(void *cls, const char *dacp_id, const char *active_remote_header);
    void  (*audio_set_progress)(void *cls, unsigned int start, unsigned int curr, unsigned int end);
    void  (*audio_get_format)(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia, uint64_t *audioFormat);
    bool  (*mirror_audio_get_enabled)(void *cls);
    void  (*mirror_audio_set_enabled)(void *cls, bool enabled);
    void  (*video_report_size)(void *cls, float *width_source, float *height_source, float *width, float *height);
    void  (*report_client_request) (void *cls, char *deviceid, char *model, char *name, bool *admit);
    void  (*display_pin) (void *cls, char * pin);
    void  (*control_pin_required) (void *cls, char * pin);
    void  (*register_client) (void *cls, const char *device_id, const char *pk_str, const char *name);
    bool  (*check_register) (void *cls, const char *pk_str);
    const char*  (*passwd) (void *cls, int *len);
    void  (*export_dacp) (void *cls, const char *active_remote, const char *dacp_id);
    int   (*video_set_codec)(void *cls, video_codec_t codec);
    /* for HLS video player controls */
    void  (*on_video_play) (void *cls, const char *location, const float start_position);
    void  (*on_video_scrub) (void *cls, const float position);
    void  (*on_video_rate) (void *cls, const float rate);
    void  (*on_video_stop) (void *cls);
    void  (*on_video_acquire_playback_info) (void *cls, playback_info_t *playback_video);
    void  (*control_pin_changed) (void *cls, unsigned short pin, const char *source, const char *reason);
    void  (*control_audio_changed) (void *cls, bool enabled);
    /* Opt-in service only. begin reserves a logical session, returning 0 to
     * accept; no media workers are started before acceptance. end follows all
     * child joins and final flush, exactly once per successful begin (including
     * startup rollback). Callbacks must be bounded, nonblocking and no-throw. */
    int   (*session_begin)(void *cls, const raop_session_info_t *info);
    void  (*session_end)(void *cls);
    /* Optional paired callbacks, session service only. Called after completed
     * pair-verify, before admission. request returns a borrowed 4-digit password
     * valid until result/next request/release (NULL refuses, never disables
     * authentication). A nonzero challenge_id identifies this password
     * generation; changing it invalidates the previous transport nonce even
     * if the random digits repeat. Nonces/Digest remain per transport.
     * result revalidates an attempted Authorization, including failed Digest;
     * it may reject a successful Digest but must never promote a failed one.
     * No secret belongs in diagnostics. Callbacks must be bounded/no-throw. */
    const char *(*session_password)(void *cls, const raop_session_info_t *info, uint64_t *challenge_id);
    bool (*session_password_result)(void *cls, bool digest_authenticated);
    /* Opt-in bounded/no-throw metadata callback; no response mutation/re-entry.
     * NULL means no observation overhead beyond checking this pointer. */
    void (*control_diagnostic)(void *cls, const raop_control_diagnostic_t *event);
  
};
typedef struct raop_callbacks_s raop_callbacks_t;

#define RAOP_CALLBACK_SCOPES_API_VERSION 1
/* Optional transport callback ownership seam, configured before raop_init2.
 * A transport scope is NOT an authenticated sender/media-session identity.
 * create receives a zeroed table; return 0 with a complete callback table
 * (including the mandatory audio_process/video_process callbacks). On failure
 * the factory owns rollback. A successful result is released exactly once,
 * even when its table is invalid. Release runs only after protocol children
 * have stopped/joined and the final connection flush has returned.
 * The factory/cls must outlive raop. Callbacks must not throw or synchronously
 * destroy their owning service. No callback buffer ownership is changed here.
 */
typedef struct raop_callback_factory_s {
    void *cls;
    int (*create)(void *cls, raop_callbacks_t *connection_callbacks);
    void (*release)(void *cls, const raop_callbacks_t *connection_callbacks);
} raop_callback_factory_t;
raop_ntp_t *raop_ntp_init(logger_t *logger, raop_callbacks_t *callbacks, const char *remote,
                          int remote_addr_len, unsigned short timing_rport,
                          timing_protocol_t *time_protocol);

int airplay_video_service_init(raop_t *raop, unsigned short port, const char *session_id);

bool register_airplay_video(raop_t *raop, airplay_video_t *airplay_video);
airplay_video_t *get_airplay_video(raop_t *raop);
airplay_video_t *deregister_airplay_video(raop_t *raop);
uint64_t get_local_time();

RAOP_API raop_t *raop_init(raop_callbacks_t *callbacks);
/* Returns -1 for invalid factories or configuration after init2; NULL clears. */
RAOP_API int raop_set_callback_factory(raop_t *raop, const raop_callback_factory_t *factory);
/* Pre-init, factory required. Requires pair-verify; disables single-client
 * nohold/rejection and HLS. The bounded transport budget (1..52) is NOT sender
 * capacity. Logical admission belongs to session_begin's external owner. */
RAOP_API int raop_enable_session_service(raop_t *raop, unsigned int transport_budget);
#define RAOP_SCOPED_DISCONNECT_API_VERSION 1
typedef bool (*raop_callback_scope_match_t)(void *callback_cls, void *opaque);
/* Session-service only, caller serializes against stop/destroy. Matches runs
 * synchronously under the transport registry lock: no re-entry, waits or throw.
 * It may atomically latch an exact current generation in its own scope. The
 * HTTP owner joins media and releases callbacks after shutdown, never here. */
RAOP_API int raop_request_session_disconnect(raop_t *raop, raop_callback_scope_match_t matches, void *opaque);
RAOP_API int raop_init2(raop_t *raop, int nohold, const char *device_id, const char *keyfile);
RAOP_API void raop_set_log_level(raop_t *raop, int level);
RAOP_API void raop_set_log_callback(raop_t *raop, raop_log_callback_t callback, void *cls);
RAOP_API int raop_set_plist(raop_t *raop, const char *plist_item, const int value);
RAOP_API void raop_set_port(raop_t *raop, unsigned short port);
RAOP_API void raop_set_udp_ports(raop_t *raop, unsigned short port[3]);
RAOP_API void raop_set_tcp_ports(raop_t *raop, unsigned short port[2]);
RAOP_API unsigned short raop_get_port(raop_t *raop);
RAOP_API void *raop_get_callback_cls(raop_t *raop);
RAOP_API int raop_start_httpd(raop_t *raop, unsigned short *port);
RAOP_API int raop_is_running(raop_t *raop);
RAOP_API void raop_stop_httpd(raop_t *raop);
RAOP_API void raop_set_dnssd(raop_t *raop, dnssd_t *dnssd);
RAOP_API void raop_destroy(raop_t *raop);
RAOP_API void raop_remove_known_connections(raop_t * raop);
RAOP_API void raop_destroy_airplay_video(raop_t *raop);
RAOP_API int raop_control_get_pin(raop_t *raop, unsigned short *pin_out, bool *use_pin_out);
RAOP_API int raop_control_set_pin(raop_t *raop, unsigned short pin);
RAOP_API int raop_control_rotate_pin(raop_t *raop);
RAOP_API int raop_rotate_pin(raop_t *raop, const char *reason);
RAOP_API bool raop_control_get_mirror_audio(raop_t *raop);
RAOP_API int raop_control_set_mirror_audio(raop_t *raop, bool enabled);
RAOP_API void raop_control_stop(raop_t *raop);

#ifdef __cplusplus
}
#endif
#endif
