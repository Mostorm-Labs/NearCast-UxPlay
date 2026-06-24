/*
 * Windows native video window support for UxPlay.
 */

#ifndef UXPLAY_NATIVE_WINDOW_H
#define UXPLAY_NATIVE_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include "../lib/logger.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*native_window_action_callback_t)(const char *action, void *userdata);

bool native_window_should_embed(const char *videosink, const char *videosink_options,
                                bool hls_video, uint32_t shared_texture_target_pid);
bool native_window_create(logger_t *logger, const char *title, bool fullscreen);
void native_window_set_action_callback(native_window_action_callback_t callback, void *userdata);
uintptr_t native_window_get_video_handle(void);
void native_window_show(void);
void native_window_set_muted(bool muted);
void native_window_set_pin(const char *pin, bool show);
void native_window_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
