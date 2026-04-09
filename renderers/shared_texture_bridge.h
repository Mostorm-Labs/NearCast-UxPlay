#ifndef SHARED_TEXTURE_BRIDGE_H
#define SHARED_TEXTURE_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include <gst/app/gstappsink.h>

#include "../lib/logger.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shared_texture_bridge_s shared_texture_bridge_t;

shared_texture_bridge_t *shared_texture_bridge_create(logger_t *logger, uint32_t target_pid);
void shared_texture_bridge_destroy(shared_texture_bridge_t *bridge);
bool shared_texture_bridge_is_active(const shared_texture_bridge_t *bridge);
GstFlowReturn shared_texture_bridge_on_new_sample(shared_texture_bridge_t *bridge, GstAppSink *sink);

#ifdef __cplusplus
}
#endif

#endif
