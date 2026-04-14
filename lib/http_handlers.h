/**
 * Copyright (c) 2024 fduncanh
 * All Rights Reserved.
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
 */

/* this file is part of raop.c and should not be included in any other file */

#include "airplay_video.h"
#include "fcup_request.h"
#include <ctype.h>

static void
http_handler_server_info(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                         char **response_data, int *response_datalen)  {

    assert(conn->raop->dnssd);
    int hw_addr_raw_len = 0;
    const char *hw_addr_raw = dnssd_get_hw_addr(conn->raop->dnssd, &hw_addr_raw_len);

    char *hw_addr = calloc(1, 3 * hw_addr_raw_len);
    //int hw_addr_len =
    utils_hwaddr_airplay(hw_addr, 3 * hw_addr_raw_len, hw_addr_raw, hw_addr_raw_len);

    plist_t r_node = plist_new_dict();

    /* first 12 AirPlay features bits (R to L): 0x27F = 0010 0111 1111
     * Only bits 0-6 and bit 9  are set:
     * 0. video supported
     * 1. photo supported
     * 2. video protected wirh FairPlay DRM
     * 3. volume control supported for video
     * 4. HLS supported
     * 5. slideshow supported
     * 6. (unknown)
     * 9. audio supported.
     */
    plist_t features_node = plist_new_uint(0x27F); 
    plist_dict_set_item(r_node, "features", features_node);

    plist_t mac_address_node = plist_new_string(hw_addr);
    plist_dict_set_item(r_node, "macAddress", mac_address_node);

    plist_t model_node = plist_new_string(GLOBAL_MODEL);
    plist_dict_set_item(r_node, "model", model_node);

    plist_t os_build_node = plist_new_string("12B435");
    plist_dict_set_item(r_node, "osBuildVersion", os_build_node);

    plist_t protovers_node = plist_new_string("1.0");
    plist_dict_set_item(r_node, "protovers", protovers_node);

    plist_t source_version_node = plist_new_string(GLOBAL_VERSION);
    plist_dict_set_item(r_node, "srcvers", source_version_node);

    plist_t vv_node = plist_new_uint(strtol(AIRPLAY_VV, NULL, 10));
    plist_dict_set_item(r_node, "vv", vv_node);

    plist_t device_id_node = plist_new_string(hw_addr);
    plist_dict_set_item(r_node, "deviceid", device_id_node);

    plist_to_xml(r_node, response_data, (uint32_t *) response_datalen);

    //assert(*response_datalen == strlen(*response_data));

    /* last character (at *response_data[response_datalen - 1]) is  0x0a = '\n'
     * (*response_data[response_datalen] is '\0').
     * apsdk removes the last "\n" by overwriting it with '\0', and reducing response_datalen by 1. 
     * TODO: check if this is necessary  */
    
    plist_free(r_node);
    http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
    free(hw_addr);
    
    /* initialize the airplay video service */
    const char *session_id = http_request_get_header(request, "X-Apple-Session-ID");

    airplay_video_service_init(conn->raop, conn->raop->port, session_id);

}

static void
http_handler_scrub(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                   char **response_data, int *response_datalen) {
    const char *url = http_request_get_url(request);
    const char *data = strstr(url, "?");
    float scrub_position = 0.0f;
    if (data) {
        data++;
        const char *position = strstr(data, "=") + 1;
        char *end;
        double value = strtod(position, &end);
        if (end && end != position) {
            scrub_position = (float) value;
            logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_scrub: got position = %.6f",
                       scrub_position);	  
        }
    }
    logger_log(conn->raop->logger, LOGGER_DEBUG, "**********************SCRUB %f ***********************",scrub_position);
    conn->raop->callbacks.on_video_scrub(conn->raop->callbacks.cls, scrub_position);
}

static void
http_handler_rate(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                  char **response_data, int *response_datalen) {

    const char *url = http_request_get_url(request);
    const char *data = strstr(url, "?");
    float rate_value = 0.0f;
    if (data) {
        data++;
        const char *rate = strstr(data, "=") + 1;
        char *end;
        float value = strtof(rate, &end);
        if (end && end != rate) {
            rate_value =  value;
            logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_rate: got rate = %.6f", rate_value);	  
	}
    }
    conn->raop->callbacks.on_video_rate(conn->raop->callbacks.cls, rate_value);
}

static void
http_handler_stop(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                  char **response_data, int *response_datalen) {
    logger_log(conn->raop->logger, LOGGER_INFO, "client HTTP request POST stop");

    conn->raop->callbacks.on_video_stop(conn->raop->callbacks.cls);
}

static int
http_pin_parse_digits(const char *digits, unsigned short *pin_out) {
    if (!digits || !pin_out) {
        return -1;
    }
    unsigned short value = 0;
    int count = 0;
    while (digits[count] >= '0' && digits[count] <= '9') {
        if (count >= 4) {
            return -1;
        }
        value = (unsigned short) (value * 10 + (unsigned short) (digits[count] - '0'));
        count++;
    }
    if (count != 4) {
        return -1;
    }
    *pin_out = value;
    return 0;
}

static int
http_pin_parse_from_query(const char *url, unsigned short *pin_out) {
    if (!url) {
        return -1;
    }
    const char *query = strchr(url, '?');
    if (!query) {
        return -1;
    }
    query++;
    const char *cursor = query;
    while (*cursor) {
        while (*cursor == '&') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        const char *key_start = cursor;
        while (*cursor && *cursor != '=' && *cursor != '&') {
            cursor++;
        }
        if (!*cursor || *cursor == '&') {
            if (*cursor == '&') {
                continue;
            }
            break;
        }
        size_t key_len = cursor - key_start;
        cursor++; /* skip '=' */
        const char *value_start = cursor;
        while (*cursor && *cursor != '&') {
            cursor++;
        }
        bool key_match = (key_len == 3 && !strncmp(key_start, "pin", 3)) ||
                         (key_len == 5 && !strncmp(key_start, "value", 5));
        if (key_match && !http_pin_parse_digits(value_start, pin_out)) {
            return 0;
        }
        if (*cursor == '&') {
            cursor++;
        }
    }
    return -1;
}

static int
http_pin_parse_from_payload(const char *payload, unsigned short *pin_out) {
    if (!payload) {
        return -1;
    }
    const char *cursor = payload;
    while (*cursor) {
        if (!strncmp(cursor, "pin", 3)) {
            char prev = (cursor == payload) ? '\0' : *(cursor - 1);
            if (!((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') || prev == '_')) {
                const char *digits = cursor + 3;
                while (*digits == ' ' || *digits == '\t' || *digits == '\n' ||
                       *digits == '\r' || *digits == ':' || *digits == '=' ||
                       *digits == '"' || *digits == '\'') {
                    digits++;
                }
                if (!http_pin_parse_digits(digits, pin_out)) {
                    return 0;
                }
            }
        }
        cursor++;
    }
    cursor = payload;
    while (*cursor) {
        if (*cursor >= '0' && *cursor <= '9') {
            if (!http_pin_parse_digits(cursor, pin_out)) {
                return 0;
            }
            while (*cursor >= '0' && *cursor <= '9') {
                cursor++;
            }
            continue;
        }
        cursor++;
    }
    return -1;
}

static void
http_pin_send_error(const char *protocol, http_response_t *response, int code, const char *reason,
                    const char *message, char **response_data, int *response_datalen) {
    const char *proto = (protocol ? protocol : "HTTP/1.1");
    http_response_init(response, proto, code, reason);
    char body[160];
    int len = snprintf(body, sizeof(body),
                       "{\"status\":\"error\",\"message\":\"%s\"}",
                       (message ? message : "unknown error"));
    char *payload = malloc(len + 1);
    if (!payload) {
        *response_data = NULL;
        *response_datalen = 0;
        return;
    }
    memcpy(payload, body, len + 1);
    *response_data = payload;
    *response_datalen = len;
    http_response_add_header(response, "Content-Type", "application/json");
    http_response_add_header(response, "Cache-Control", "no-store");
    http_response_add_header(response, "Access-Control-Allow-Origin", "*");
}

static void
http_pin_send_status(raop_conn_t *conn, const char *status, const char *message, bool expose_pin,
                     http_response_t *response, char **response_data, int *response_datalen) {
    unsigned short stored_pin = conn->raop->pin;
    const char *mode = "disabled";
    if (conn->raop->use_pin) {
        mode = (stored_pin > 0 ? "static" : "random");
    } else if (stored_pin > 0) {
        mode = "oneshot";
    }
    char pin_field[32];
    if (expose_pin && conn->raop->use_pin && stored_pin) {
        snprintf(pin_field, sizeof(pin_field), "\"pin\":\"%04u\"", stored_pin % 10000);
    } else {
        strncpy(pin_field, "\"pin\":null", sizeof(pin_field));
    }
    pin_field[sizeof(pin_field) - 1] = '\0';
    char body[256];
    int len = snprintf(body, sizeof(body),
                       "{\"status\":\"%s\",\"message\":\"%s\",\"usePin\":%s,\"mode\":\"%s\",%s}",
                       (status ? status : "ok"),
                       (message ? message : ""),
                       (conn->raop->use_pin ? "true" : "false"),
                       mode,
                       pin_field);
    char *payload = malloc(len + 1);
    if (!payload) {
        *response_data = NULL;
        *response_datalen = 0;
        return;
    }
    memcpy(payload, body, len + 1);
    *response_data = payload;
    *response_datalen = len;
    http_response_add_header(response, "Content-Type", "application/json");
    http_response_add_header(response, "Cache-Control", "no-store");
    http_response_add_header(response, "Access-Control-Allow-Origin", "*");
}

static void
http_handler_pin_control(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                         char **response_data, int *response_datalen) {
    const char *method = http_request_get_method(request);
    const char *protocol = http_request_get_protocol(request);
    if (!method) {
        http_pin_send_error(protocol, response, 400, "Bad Request", "missing HTTP method",
                            response_data, response_datalen);
        return;
    }

    if (!strcmp(method, "OPTIONS")) {
        http_response_init(response, protocol ? protocol : "HTTP/1.1", 204, "No Content");
        http_response_add_header(response, "Access-Control-Allow-Origin", "*");
        http_response_add_header(response, "Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
        http_response_add_header(response, "Access-Control-Allow-Headers", "Content-Type");
        *response_data = NULL;
        *response_datalen = 0;
        return;
    }

    if (!strcmp(method, "GET")) {
        bool reveal_pin = (conn->raop->use_pin && conn->raop->pin);
        http_pin_send_status(conn, "ok", "pin status", reveal_pin, response, response_data,
                             response_datalen);
        return;
    }

    bool is_update = (!strcmp(method, "POST") || !strcmp(method, "PUT"));
    if (!is_update) {
        http_pin_send_error(protocol, response, 405, "Method Not Allowed",
                            "method not allowed", response_data, response_datalen);
        http_response_add_header(response, "Allow", "GET, POST, PUT, OPTIONS");
        return;
    }

    unsigned short new_pin = 0;
    int parse_result = http_pin_parse_from_query(http_request_get_url(request), &new_pin);
    int datalen = 0;
    const char *request_data = http_request_get_data(request, &datalen);
    char *payload = NULL;
    if (parse_result != 0 && request_data && datalen > 0) {
        payload = calloc(1, datalen + 1);
        if (payload) {
            memcpy(payload, request_data, datalen);
            parse_result = http_pin_parse_from_payload(payload, &new_pin);
        }
    }
    if (payload) {
        free(payload);
    }
    if (parse_result != 0) {
        http_pin_send_error(protocol, response, 400, "Bad Request",
                            "pin parameter missing or invalid", response_data, response_datalen);
        return;
    }

    conn->raop->pin = (unsigned short) (10000 + new_pin);
    conn->raop->use_pin = true;
    char pin_str[6];
    snprintf(pin_str, sizeof(pin_str), "%04u", new_pin);
    if (conn->raop->callbacks.display_pin) {
        conn->raop->callbacks.display_pin(conn->raop->callbacks.cls, pin_str);
    }
    logger_log(conn->raop->logger, LOGGER_INFO, "HTTP control updated pin to %s", pin_str);
    http_pin_send_status(conn, "ok", "pin updated", true, response, response_data, response_datalen);
}

static int
http_audio_parse_bool_token(const char *value, bool *enabled_out) {
    if (!value || !enabled_out) {
        return -1;
    }
    while (*value == ' ' || *value == '\t' || *value == '\n' || *value == '\r' ||
           *value == ':' || *value == '=' || *value == '"' || *value == '\'') {
        value++;
    }
    char token[16];
    int len = 0;
    while (value[len]) {
        unsigned char ch = (unsigned char) value[len];
        if (isalnum(ch) || ch == '_' || ch == '-') {
            if (len >= (int) sizeof(token) - 1) {
                return -1;
            }
            token[len] = (char) tolower(ch);
            len++;
            continue;
        }
        break;
    }
    if (len == 0) {
        return -1;
    }
    token[len] = '\0';
    if (!strcmp(token, "1") || !strcmp(token, "true") || !strcmp(token, "on") ||
        !strcmp(token, "yes") || !strcmp(token, "enable") || !strcmp(token, "enabled")) {
        *enabled_out = true;
        return 0;
    }
    if (!strcmp(token, "0") || !strcmp(token, "false") || !strcmp(token, "off") ||
        !strcmp(token, "no") || !strcmp(token, "disable") || !strcmp(token, "disabled")) {
        *enabled_out = false;
        return 0;
    }
    return -1;
}

static int
http_audio_parse_from_query(const char *url, bool *enabled_out) {
    if (!url || !enabled_out) {
        return -1;
    }
    const char *query = strchr(url, '?');
    if (!query) {
        return -1;
    }
    query++;
    const char *cursor = query;
    while (*cursor) {
        while (*cursor == '&') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        const char *key_start = cursor;
        while (*cursor && *cursor != '=' && *cursor != '&') {
            cursor++;
        }
        if (!*cursor || *cursor == '&') {
            if (*cursor == '&') {
                continue;
            }
            break;
        }
        size_t key_len = cursor - key_start;
        cursor++; /* skip '=' */
        const char *value_start = cursor;
        while (*cursor && *cursor != '&') {
            cursor++;
        }
        bool key_match = (key_len == 5 && !strncmp(key_start, "audio", 5)) ||
                         (key_len == 7 && !strncmp(key_start, "enabled", 7)) ||
                         (key_len == 5 && !strncmp(key_start, "value", 5)) ||
                         (key_len == 5 && !strncmp(key_start, "state", 5)) ||
                         (key_len == 2 && !strncmp(key_start, "on", 2));
        if (key_match && !http_audio_parse_bool_token(value_start, enabled_out)) {
            return 0;
        }
        if (*cursor == '&') {
            cursor++;
        }
    }
    return -1;
}

static int
http_audio_parse_from_payload_key(const char *payload, const char *key, bool *enabled_out) {
    if (!payload || !key || !enabled_out) {
        return -1;
    }
    size_t key_len = strlen(key);
    const char *cursor = payload;
    while ((cursor = strstr(cursor, key)) != NULL) {
        char prev = (cursor == payload) ? '\0' : *(cursor - 1);
        if (isalnum((unsigned char) prev) || prev == '_') {
            cursor += key_len;
            continue;
        }
        const char *value_start = cursor + key_len;
        while (*value_start == ' ' || *value_start == '\t' || *value_start == '\n' ||
               *value_start == '\r' || *value_start == ':' || *value_start == '=' ||
               *value_start == '"' || *value_start == '\'') {
            value_start++;
        }
        if (!http_audio_parse_bool_token(value_start, enabled_out)) {
            return 0;
        }
        cursor += key_len;
    }
    return -1;
}

static int
http_audio_parse_from_payload(const char *payload, bool *enabled_out) {
    if (!payload || !enabled_out) {
        return -1;
    }
    const char *keys[] = {"enabled", "audio", "state", "value", "on"};
    int key_count = (int) (sizeof(keys) / sizeof(keys[0]));
    for (int i = 0; i < key_count; i++) {
        if (!http_audio_parse_from_payload_key(payload, keys[i], enabled_out)) {
            return 0;
        }
    }
    const char *cursor = payload;
    while (*cursor) {
        unsigned char ch = (unsigned char) *cursor;
        char prev = (cursor == payload) ? '\0' : *(cursor - 1);
        bool token_start = (cursor == payload) || !(isalnum((unsigned char) prev) || prev == '_');
        if (token_start && (isalpha(ch) || ch == '0' || ch == '1')) {
            if (!http_audio_parse_bool_token(cursor, enabled_out)) {
                return 0;
            }
        }
        cursor++;
    }
    return -1;
}

static bool
http_mirror_audio_get_enabled(raop_conn_t *conn) {
    bool enabled = conn->raop->mirror_audio_enabled;
    if (conn->raop->callbacks.mirror_audio_get_enabled) {
        enabled = conn->raop->callbacks.mirror_audio_get_enabled(conn->raop->callbacks.cls);
    }
    return enabled;
}

static void
http_audio_send_error(const char *protocol, http_response_t *response, int code, const char *reason,
                      const char *message, char **response_data, int *response_datalen) {
    const char *proto = (protocol ? protocol : "HTTP/1.1");
    http_response_init(response, proto, code, reason);
    char body[180];
    int len = snprintf(body, sizeof(body),
                       "{\"status\":\"error\",\"message\":\"%s\"}",
                       (message ? message : "unknown error"));
    char *payload = malloc(len + 1);
    if (!payload) {
        *response_data = NULL;
        *response_datalen = 0;
        return;
    }
    memcpy(payload, body, len + 1);
    *response_data = payload;
    *response_datalen = len;
    http_response_add_header(response, "Content-Type", "application/json");
    http_response_add_header(response, "Cache-Control", "no-store");
    http_response_add_header(response, "Access-Control-Allow-Origin", "*");
}

static void
http_audio_send_status(raop_conn_t *conn, const char *status, const char *message,
                       http_response_t *response, char **response_data, int *response_datalen) {
    bool enabled = http_mirror_audio_get_enabled(conn);
    char body[220];
    int len = snprintf(body, sizeof(body),
                       "{\"status\":\"%s\",\"message\":\"%s\",\"mirrorAudio\":%s}",
                       (status ? status : "ok"),
                       (message ? message : ""),
                       (enabled ? "true" : "false"));
    char *payload = malloc(len + 1);
    if (!payload) {
        *response_data = NULL;
        *response_datalen = 0;
        return;
    }
    memcpy(payload, body, len + 1);
    *response_data = payload;
    *response_datalen = len;
    http_response_add_header(response, "Content-Type", "application/json");
    http_response_add_header(response, "Cache-Control", "no-store");
    http_response_add_header(response, "Access-Control-Allow-Origin", "*");
}

static void
http_handler_mirror_audio_control(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                                  char **response_data, int *response_datalen) {
    const char *method = http_request_get_method(request);
    const char *protocol = http_request_get_protocol(request);
    if (!method) {
        http_audio_send_error(protocol, response, 400, "Bad Request", "missing HTTP method",
                              response_data, response_datalen);
        return;
    }

    if (!strcmp(method, "OPTIONS")) {
        http_response_init(response, protocol ? protocol : "HTTP/1.1", 204, "No Content");
        http_response_add_header(response, "Access-Control-Allow-Origin", "*");
        http_response_add_header(response, "Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
        http_response_add_header(response, "Access-Control-Allow-Headers", "Content-Type");
        *response_data = NULL;
        *response_datalen = 0;
        return;
    }

    if (!strcmp(method, "GET")) {
        http_audio_send_status(conn, "ok", "mirror audio status", response, response_data,
                               response_datalen);
        return;
    }

    bool is_update = (!strcmp(method, "POST") || !strcmp(method, "PUT"));
    if (!is_update) {
        http_audio_send_error(protocol, response, 405, "Method Not Allowed",
                              "method not allowed", response_data, response_datalen);
        http_response_add_header(response, "Allow", "GET, POST, PUT, OPTIONS");
        return;
    }

    bool enabled = false;
    int parse_result = http_audio_parse_from_query(http_request_get_url(request), &enabled);
    int datalen = 0;
    const char *request_data = http_request_get_data(request, &datalen);
    char *payload = NULL;
    if (parse_result != 0 && request_data && datalen > 0) {
        payload = calloc(1, datalen + 1);
        if (payload) {
            memcpy(payload, request_data, datalen);
            parse_result = http_audio_parse_from_payload(payload, &enabled);
        }
    }
    if (payload) {
        free(payload);
    }
    if (parse_result != 0) {
        http_audio_send_error(protocol, response, 400, "Bad Request",
                              "mirror audio parameter missing or invalid", response_data, response_datalen);
        return;
    }

    conn->raop->mirror_audio_enabled = enabled;
    if (conn->raop->callbacks.mirror_audio_set_enabled) {
        conn->raop->callbacks.mirror_audio_set_enabled(conn->raop->callbacks.cls, enabled);
    }
    bool applied = http_mirror_audio_get_enabled(conn);
    logger_log(conn->raop->logger, LOGGER_INFO, "HTTP control mirror audio set to %s",
               (applied ? "on" : "off"));
    http_audio_send_status(conn, "ok",
                           (applied ? "mirror audio enabled" : "mirror audio disabled"),
                           response, response_data, response_datalen);
}

/* handles PUT /setProperty http requests from Client to Server */

static void
http_handler_set_property(raop_conn_t *conn,
                          http_request_t *request, http_response_t *response,
			  char **response_data, int *response_datalen) {

    const char *url = http_request_get_url(request);
    const char *property = url + strlen("/setProperty?");
    logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_set_property: %s", property);

    /*  actionAtItemEnd:  values:  
                  0: advance (advance to next item, if there is one)
                  1: pause   (pause playing)
                  2: none    (do nothing)             

        reverseEndTime   (only used when rate < 0) time at which reverse playback ends
        forwardEndTime   (only used when rate > 0) time at which reverse playback ends
    */

    if (!strcmp(property, "reverseEndTime") ||
        !strcmp(property, "forwardEndTime") ||
        !strcmp(property, "actionAtItemEnd")) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "property %s is known but unhandled", property);

        plist_t errResponse = plist_new_dict();
        plist_t errCode = plist_new_uint(0);
        plist_dict_set_item(errResponse, "errorCode", errCode);
        plist_to_xml(errResponse, response_data, (uint32_t *) response_datalen);
        plist_free(errResponse);
        http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
    } else {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "property %s is unknown, unhandled", property);      
        http_response_add_header(response, "Content-Length", "0");
    }
}

/* handles GET /getProperty http requests from Client to Server.  (not implemented) */

static void
http_handler_get_property(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                          char **response_data, int *response_datalen) {
    const char *url = http_request_get_url(request);
    const char *property = url + strlen("getProperty?");
    logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_get_property: %s (unhandled)", property);
}

/* this request (for a variant FairPlay decryption)  cannot be handled  by UxPlay */
static void
http_handler_fpsetup2(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen) {
    logger_log(conn->raop->logger, LOGGER_WARNING, "client HTTP request POST fp-setup2 is unhandled");
    http_response_add_header(response, "Content-Type", "application/x-apple-binary-plist");
    int req_datalen;
    const unsigned char *req_data = (unsigned char *) http_request_get_data(request, &req_datalen);
    logger_log(conn->raop->logger, LOGGER_ERR, "only FairPlay version 0x03 is implemented, version is 0x%2.2x",
               req_data[4]);
    http_response_init(response, "HTTP/1.1", 421, "Misdirected Request");
}

// called by http_handler_playback_info while preparing response to a GET /playback_info request from the client.

typedef struct time_range_s {
  double start;
  double duration;
} time_range_t;

void time_range_to_plist(void *time_ranges, const int n_time_ranges,
		         plist_t time_ranges_node) {
    time_range_t *tr = (time_range_t *) time_ranges;
    for (int i = 0 ; i < n_time_ranges; i++) {
        plist_t time_range_node = plist_new_dict();
        plist_t duration_node = plist_new_real(tr[i].duration);
        plist_dict_set_item(time_range_node, "duration", duration_node);
        plist_t start_node = plist_new_real(tr[i].start);
        plist_dict_set_item(time_range_node, "start", start_node);
        plist_array_append_item(time_ranges_node, time_range_node);
    }
}

// called by http_handler_playback_info while preparing response to a GET /playback_info request from the client.

int create_playback_info_plist_xml(playback_info_t *playback_info, char **plist_xml) {

    plist_t res_root_node = plist_new_dict();

    plist_t duration_node = plist_new_real(playback_info->duration);
    plist_dict_set_item(res_root_node, "duration", duration_node);

    plist_t position_node = plist_new_real(playback_info->position);
    plist_dict_set_item(res_root_node, "position", position_node);

    plist_t rate_node = plist_new_real(playback_info->rate);
    plist_dict_set_item(res_root_node, "rate", rate_node);

    /* should these be int or bool? */
    plist_t ready_to_play_node = plist_new_uint(playback_info->ready_to_play);
    plist_dict_set_item(res_root_node, "readyToPlay", ready_to_play_node);

    plist_t playback_buffer_empty_node = plist_new_uint(playback_info->playback_buffer_empty);
    plist_dict_set_item(res_root_node, "playbackBufferEmpty", playback_buffer_empty_node);

    plist_t playback_buffer_full_node = plist_new_uint(playback_info->playback_buffer_full);
    plist_dict_set_item(res_root_node, "playbackBufferFull", playback_buffer_full_node);

    plist_t playback_likely_to_keep_up_node = plist_new_uint(playback_info->playback_likely_to_keep_up);
    plist_dict_set_item(res_root_node, "playbackLikelyToKeepUp", playback_likely_to_keep_up_node);

    plist_t loaded_time_ranges_node = plist_new_array();
    time_range_to_plist(playback_info->loadedTimeRanges, playback_info->num_loaded_time_ranges,
			loaded_time_ranges_node);
    plist_dict_set_item(res_root_node, "loadedTimeRanges", loaded_time_ranges_node);

    plist_t seekable_time_ranges_node = plist_new_array();
    time_range_to_plist(playback_info->seekableTimeRanges, playback_info->num_seekable_time_ranges,
			seekable_time_ranges_node);
    plist_dict_set_item(res_root_node, "seekableTimeRanges", seekable_time_ranges_node);

    int len;
    plist_to_xml(res_root_node, plist_xml, (uint32_t *) &len);
    /* plist_xml is null-terminated, last character is '/n' */

    plist_free(res_root_node);

    return len;
}


/* this handles requests from the Client  for "Playback information" while the Media is playing on the 
   Media Player.  (The Server gets this information by monitoring the Media Player). The Client could use 
   the information to e.g. update  the slider it shows with progress to the player (0%-100%). 
   It does not affect playing of the Media*/

static void
http_handler_playback_info(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                           char **response_data, int *response_datalen)
{
    logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_playback_info");
    //const char *session_id = http_request_get_header(request, "X-Apple-Session-ID");
    playback_info_t playback_info;

    playback_info.stallcount = 0;
    //playback_info.playback_buffer_empty = false;   // maybe  need to get this from playbin 
    //playback_info.playback_buffer_full = true;
    //ayback_info.ready_to_play = true; // ???;
    //ayback_info.playback_likely_to_keep_up = true;

    conn->raop->callbacks.on_video_acquire_playback_info(conn->raop->callbacks.cls, &playback_info);
    if (playback_info.duration == -1.0) {
        /* video has finished, reset */
	logger_log(conn->raop->logger, LOGGER_DEBUG, "playback_info not available (finishing)");
	//httpd_remove_known_connections(conn->raop->httpd);
	http_response_set_disconnect(response,1);
	conn->raop->callbacks.video_reset(conn->raop->callbacks.cls);
	return;
    } else if (playback_info.position == -1.0) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "playback_info not available");
        return;
    }      

    playback_info.num_loaded_time_ranges = 1; 
    time_range_t time_ranges_loaded[1];
    time_ranges_loaded[0].start = playback_info.position;
    time_ranges_loaded[0].duration = playback_info.duration - playback_info.position;
    playback_info.loadedTimeRanges = (void *) &time_ranges_loaded;

    playback_info.num_seekable_time_ranges = 1;
    time_range_t time_ranges_seekable[1];
    time_ranges_seekable[0].start = 0.0;
    time_ranges_seekable[0].duration = playback_info.position;
    playback_info.seekableTimeRanges = (void *) &time_ranges_seekable;

    *response_datalen =  create_playback_info_plist_xml(&playback_info, response_data);
    http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
}

/* this handles the POST /reverse request from Client to Server on a AirPlay http channel to "Upgrade" 
   to "PTTH/1.0" Reverse HTTP protocol proposed in 2009 Internet-Draft 

          https://datatracker.ietf.org/doc/id/draft-lentczner-rhttp-00.txt .  

   After the Upgrade the channel becomes a reverse http "AirPlay (reversed)" channel for
   http requests from Server to Client.
  */

static void
http_handler_reverse(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                     char **response_data, int *response_datalen) {

    /* get http socket for send */
    int socket_fd = httpd_get_connection_socket (conn->raop->httpd, (void *) conn);
    if (socket_fd < 0) {
        logger_log(conn->raop->logger, LOGGER_ERR, "fcup_request failed to retrieve socket_fd from httpd");
        /* shut down connection? */
    }
    
    const char *purpose = http_request_get_header(request, "X-Apple-Purpose");
    const char *connection = http_request_get_header(request, "Connection");
    const char *upgrade = http_request_get_header(request, "Upgrade");
    logger_log(conn->raop->logger, LOGGER_INFO, "client requested reverse connection: %s; purpose: %s  \"%s\"",
               connection, upgrade, purpose);

    httpd_set_connection_type(conn->raop->httpd, (void *) conn, CONNECTION_TYPE_PTTH);
    int type_PTTH = httpd_count_connection_type(conn->raop->httpd, CONNECTION_TYPE_PTTH);

    if (type_PTTH == 1) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "will use socket %d for %s connections", socket_fd, purpose);
        http_response_init(response, "HTTP/1.1", 101, "Switching Protocols");
	http_response_add_header(response, "Connection", "Upgrade");
	http_response_add_header(response, "Upgrade", "PTTH/1.0");

    } else {
        logger_log(conn->raop->logger, LOGGER_ERR, "multiple TPPH connections (%d) are forbidden", type_PTTH );
    }    
}

/* the POST /action request from Client to Server on the AirPlay http channel follows a POST /event "FCUP Request"
 from Server to Client on the reverse http channel, for a HLS playlist (first the Master Playlist, then the Media Playlists
 listed in the Master Playlist.     The POST /action request contains the playlist requested by the Server in
 the preceding "FCUP Request".   The FCUP Request sequence  continues until all Media Playlists have been obtained by the Server */ 

static void
http_handler_action(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                    char **response_data, int *response_datalen) {

    bool data_is_plist = false;
    plist_t req_root_node = NULL;
    uint64_t uint_val;
    int request_id = 0;
    int fcup_response_statuscode = 0;
    bool logger_debug = (logger_get_level(conn->raop->logger) >= LOGGER_DEBUG);
    

    const char* session_id = http_request_get_header(request, "X-Apple-Session-ID");
    if (!session_id) {
        logger_log(conn->raop->logger, LOGGER_ERR, "Play request had no X-Apple-Session-ID");
        goto post_action_error;
    }    
    const char *apple_session_id = get_apple_session_id(conn->raop->airplay_video);
    if (strcmp(session_id, apple_session_id)){
        logger_log(conn->raop->logger, LOGGER_ERR, "X-Apple-Session-ID has changed:\n  was:\"%s\"\n  now:\"%s\"",
                   apple_session_id, session_id);
        goto post_action_error;
    }

    /* verify that this request contains a binary plist*/
    char *header_str = NULL;
    http_request_get_header_string(request, &header_str);
    logger_log(conn->raop->logger, LOGGER_DEBUG, "request header: %s", header_str);
    data_is_plist = (strstr(header_str,"apple-binary-plist") != NULL);
    free(header_str);
    if (!data_is_plist) {
        logger_log(conn->raop->logger, LOGGER_INFO, "POST /action: did not receive expected plist from client");	
        goto post_action_error;
    }

    /* extract the root_node  plist */
    int request_datalen = 0;
    const char *request_data = http_request_get_data(request, &request_datalen);
    if (request_datalen == 0) {
        logger_log(conn->raop->logger, LOGGER_INFO, "POST /action: did not receive expected plist from client");	
        goto post_action_error;
    }
    plist_from_bin(request_data, request_datalen, &req_root_node);

    /* determine type of data */
    plist_t req_type_node = plist_dict_get_item(req_root_node, "type");
    if (!PLIST_IS_STRING(req_type_node)) {
      goto post_action_error;
    }

    plist_t req_params_node = NULL;
    
    /* three possible types are known */
    char *type = NULL;
    plist_get_string_val(req_type_node, &type);
    logger_log(conn->raop->logger, LOGGER_DEBUG, "action type is %s", type);
    if (strstr(type, "unhandledURLResponse")) {
        goto unhandledURLResponse;      
    } else if (strstr(type, "playlistRemove")) {
        logger_log(conn->raop->logger, LOGGER_INFO, "unhandled action type playlistRemove (stop playback)");
        req_params_node = plist_dict_get_item(req_root_node, "params");    
        if (!req_params_node || !PLIST_IS_DICT (req_params_node)) {
            goto post_action_error;
        }
        plist_t req_params_item_node = plist_dict_get_item(req_params_node, "item");
        if (!req_params_item_node) {
            goto post_action_error;
        } else {
            if (!PLIST_IS_DICT (req_params_item_node)) {
                goto post_action_error;
            }
            plist_t req_params_item_uuid_node = plist_dict_get_item(req_params_item_node, "uuid");
            char* remove_uuid = NULL;
	    plist_get_string_val(req_params_item_uuid_node, &remove_uuid);
            const char *playback_uuid = get_playback_uuid(conn->raop->airplay_video);
	    if (strcmp(remove_uuid, playback_uuid)) {
                logger_log(conn->raop->logger, LOGGER_ERR, "uuid of playlist removal action request did not match current playlist:\n"
                           "   current: %s\n   remove: %s", playback_uuid, remove_uuid);
            } else {
                logger_log(conn->raop->logger, LOGGER_DEBUG, "removal_uuid matches playback_uuid\n");
	    }	  
	    free (remove_uuid);
        }
        logger_log(conn->raop->logger, LOGGER_ERR, "FIXME: playlist removal not yet implemented");
        goto finish;
    } else if (strstr(type, "playlistInsert")) {
        logger_log(conn->raop->logger, LOGGER_INFO, "unhandled action type playlistInsert (add new playback)");
        printf("\n***************FIXME************************\nPlaylist insertion needs more information for it to be implemented:\n"
               "please report following output as an \"Issue\" at http://github.com/FDH2/UxPlay:\n");
        char *header_str = NULL;
        http_request_get_header_string(request, &header_str);
        printf("\n\n%s\n", header_str);
        bool is_plist = (bool) strstr(header_str,"apple-binary-plist");
        free(header_str);
        if (is_plist) {
            int request_datalen;
            const char *request_data = http_request_get_data(request, &request_datalen);
            plist_t req_root_node = NULL;
            plist_from_bin(request_data, request_datalen, &req_root_node);
            char * plist_xml;
            uint32_t plist_len;
            plist_to_xml(req_root_node, &plist_xml, &plist_len);
            plist_xml = utils_strip_data_from_plist_xml(plist_xml);
            printf("%s", plist_xml);	
            free(plist_xml);
            plist_free(req_root_node);
        }
        assert(0);
    } else {
        logger_log(conn->raop->logger, LOGGER_INFO, "unknown action type (unhandled)"); 
        goto finish;
    }

 unhandledURLResponse:;

    req_params_node = plist_dict_get_item(req_root_node, "params");
    if (!PLIST_IS_DICT (req_params_node)) {
        goto post_action_error;
    }
    
    /* handling type "unhandledURLResponse" (case 1)*/
    uint_val = 0;
    int fcup_response_datalen = 0;

    if  (logger_debug) {
        plist_t req_params_fcup_response_statuscode_node = plist_dict_get_item(req_params_node,
                                                                      "FCUP_Response_StatusCode");
        if (req_params_fcup_response_statuscode_node) {
            plist_get_uint_val(req_params_fcup_response_statuscode_node, &uint_val);
            fcup_response_statuscode = (int) uint_val;
            uint_val = 0;
            logger_log(conn->raop->logger, LOGGER_DEBUG, "FCUP_Response_StatusCode = %d",
                       fcup_response_statuscode);
        }

        plist_t req_params_fcup_response_requestid_node = plist_dict_get_item(req_params_node,
                                                                     "FCUP_Response_RequestID");
        if (req_params_fcup_response_requestid_node) {
            plist_get_uint_val(req_params_fcup_response_requestid_node, &uint_val);
            request_id = (int) uint_val;
            uint_val = 0;
            logger_log(conn->raop->logger, LOGGER_DEBUG, "FCUP_Response_RequestID =  %d", request_id);
        }
    }

    plist_t req_params_fcup_response_url_node = plist_dict_get_item(req_params_node, "FCUP_Response_URL");
    if (!PLIST_IS_STRING(req_params_fcup_response_url_node)) {
        goto post_action_error;
    }
    char *fcup_response_url = NULL;
    plist_get_string_val(req_params_fcup_response_url_node, &fcup_response_url);
    if (!fcup_response_url) {
        goto post_action_error;
    }
    logger_log(conn->raop->logger, LOGGER_DEBUG, "FCUP_Response_URL =  %s", fcup_response_url);
	
    plist_t req_params_fcup_response_data_node = plist_dict_get_item(req_params_node, "FCUP_Response_Data");
    if (!PLIST_IS_DATA(req_params_fcup_response_data_node)){
        goto post_action_error;
    }

    uint_val = 0;
    char *fcup_response_data = NULL;    
    plist_get_data_val(req_params_fcup_response_data_node, &fcup_response_data, &uint_val);
    fcup_response_datalen = (int) uint_val;

    if (!fcup_response_data) {
	free (fcup_response_url);
	goto post_action_error;
    } 

    if (logger_debug) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "FCUP_Response datalen =  %d", fcup_response_datalen);
	char *data = malloc(fcup_response_datalen + 1);
	memcpy(data, fcup_response_data, fcup_response_datalen);
	data[fcup_response_datalen] = '\0';
	logger_log(conn->raop->logger, LOGGER_DEBUG, "begin FCUP Response data:\n%s\nend FCUP Response data",data);
	free (data);
    }


    char *ptr = strstr(fcup_response_url, "/master.m3u8");
    if (ptr) {
      	/* this is a master playlist */
        char *uri_prefix = get_uri_prefix(conn->raop->airplay_video);
	char ** media_data_store = NULL;
        int num_uri = 0;

        char *uri_local_prefix = get_uri_local_prefix(conn->raop->airplay_video);
        char *new_master = adjust_master_playlist (fcup_response_data, fcup_response_datalen,  uri_prefix, uri_local_prefix);
        store_master_playlist(conn->raop->airplay_video, new_master);
        create_media_uri_table(uri_prefix, fcup_response_data, fcup_response_datalen, &media_data_store, &num_uri);	
	create_media_data_store(conn->raop->airplay_video, media_data_store, num_uri);  
	num_uri =  get_num_media_uri(conn->raop->airplay_video);
	set_next_media_uri_id(conn->raop->airplay_video, 0);
    } else {
        /* this is a media playlist */
        assert(fcup_response_data);
	char *playlist = (char *) calloc(fcup_response_datalen + 1, sizeof(char));
	memcpy(playlist, fcup_response_data, fcup_response_datalen);
        int uri_num = get_next_media_uri_id(conn->raop->airplay_video);
	--uri_num;    // (next num is current num + 1)
	store_media_playlist(conn->raop->airplay_video, playlist, uri_num);
        float duration = 0.0f;
        int count = analyze_media_playlist(playlist, &duration);
        if (count) {
        logger_log(conn->raop->logger, LOGGER_DEBUG,
                   "\n%s:\nreceived media playlist has %5d chunks, total duration %9.3f secs\n",
                    fcup_response_url, count, duration);
        }
    }

    if (fcup_response_data) {
        free (fcup_response_data);
    }
    if (fcup_response_url) {
        free (fcup_response_url);
    }

    int num_uri = get_num_media_uri(conn->raop->airplay_video);
    int uri_num = get_next_media_uri_id(conn->raop->airplay_video);
    if (uri_num <  num_uri) {
        fcup_request((void *) conn, get_media_uri_by_num(conn->raop->airplay_video, uri_num),
                                                  apple_session_id,
                                                  get_next_FCUP_RequestID(conn->raop->airplay_video));
	set_next_media_uri_id(conn->raop->airplay_video, ++uri_num);
    } else {
        char * uri_local_prefix = get_uri_local_prefix(conn->raop->airplay_video);
        conn->raop->callbacks.on_video_play(conn->raop->callbacks.cls,
					    strcat(uri_local_prefix, "/master.m3u8"),
                                            get_start_position_seconds(conn->raop->airplay_video));
    }

 finish:
    plist_free(req_root_node);
    return;

 post_action_error:;
    http_response_init(response, "HTTP/1.1", 400, "Bad Request");

    if (req_root_node)  {
      plist_free(req_root_node);
    }

}

/* The POST /play request from the Client to Server on the AirPlay http channel contains (among other information)
   the "Content Location" that specifies the HLS Playlists for the video to be streamed, as well as the video 
   "start position in seconds".   Once this request is received by the Sever, the Server sends a POST /event
   "FCUP Request" request to the Client on the reverse http channel, to request the HLS Master Playlist */

static void
http_handler_play(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen) {

    char* playback_location = NULL;
    char* client_proc_name = NULL;
    plist_t req_root_node = NULL;
    float start_position_seconds = 0.0f;
    bool data_is_binary_plist = false;
    bool data_is_text = false;
    bool data_is_octet = false;
    char supported_hls_proc_names[] = "YouTube;";

    logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_play");

    const char* session_id = http_request_get_header(request, "X-Apple-Session-ID");
    if (!session_id) {
        logger_log(conn->raop->logger, LOGGER_ERR, "Play request had no X-Apple-Session-ID");
        goto play_error;
    }
    const char *apple_session_id = get_apple_session_id(conn->raop->airplay_video);
    if (strcmp(session_id, apple_session_id)){
        logger_log(conn->raop->logger, LOGGER_ERR, "X-Apple-Session-ID has changed:\n  was:\"%s\"\n  now:\"%s\"",
                   apple_session_id, session_id);
        goto play_error;
    }

    int request_datalen = -1;    
    const char *request_data = http_request_get_data(request, &request_datalen);

    if (request_datalen > 0) {
        char *header_str = NULL;
        http_request_get_header_string(request, &header_str);
        logger_log(conn->raop->logger, LOGGER_DEBUG, "request header:\n%s", header_str);
	data_is_binary_plist = (strstr(header_str, "x-apple-binary-plist") != NULL);
	data_is_text = (strstr(header_str, "text/parameters") != NULL);
	data_is_octet = (strstr(header_str, "octet-stream") != NULL);
	free (header_str);
    }
    if (!data_is_text && !data_is_octet && !data_is_binary_plist) {
      goto play_error;
    }

    if (data_is_text) {
         logger_log(conn->raop->logger, LOGGER_ERR, "Play request Content is text (unsupported)");
	 goto play_error;
    }

    if (data_is_octet) {
         logger_log(conn->raop->logger, LOGGER_ERR, "Play request Content is octet-stream (unsupported)");
	 goto play_error;
    }

    if (data_is_binary_plist) {
        plist_from_bin(request_data, request_datalen, &req_root_node);

        plist_t req_uuid_node = plist_dict_get_item(req_root_node, "uuid");
        if (!req_uuid_node) {
            goto play_error;
        } else {
            char* playback_uuid = NULL;
            plist_get_string_val(req_uuid_node, &playback_uuid);
	    set_playback_uuid(conn->raop->airplay_video, playback_uuid);
            free (playback_uuid);
	}

        plist_t req_content_location_node = plist_dict_get_item(req_root_node, "Content-Location");
        if (!req_content_location_node) {
            goto play_error;
        } else {
            plist_get_string_val(req_content_location_node, &playback_location);
        }

        plist_t req_client_proc_name_node = plist_dict_get_item(req_root_node, "clientProcName");
        if (!req_client_proc_name_node) {
            goto play_error;
        } else {
            plist_get_string_val(req_client_proc_name_node, &client_proc_name);
            if (!strstr(supported_hls_proc_names, client_proc_name)){
                logger_log(conn->raop->logger, LOGGER_WARNING, "Unsupported HLS streaming format: clientProcName %s not found in supported list: %s",
                           client_proc_name, supported_hls_proc_names);
            }
        }
	
        plist_t req_start_position_seconds_node = plist_dict_get_item(req_root_node, "Start-Position-Seconds");
        if (!req_start_position_seconds_node) {
            logger_log(conn->raop->logger, LOGGER_INFO, "No Start-Position-Seconds in Play request");	    
         } else {
             double start_position = 0.0;
             plist_get_real_val(req_start_position_seconds_node, &start_position);
	     start_position_seconds = (float) start_position;
        }
	set_start_position_seconds(conn->raop->airplay_video, (float) start_position_seconds);
    }

    char *ptr = strstr(playback_location, "/master.m3u8");
    if (!ptr) {
        logger_log(conn->raop->logger, LOGGER_ERR, "Content-Location has unsupported form:\n%s\n", playback_location);	    
	goto play_error;
    }
    int prefix_len =  (int) (ptr - playback_location);
    set_uri_prefix(conn->raop->airplay_video, playback_location, prefix_len);
    set_next_media_uri_id(conn->raop->airplay_video, 0);
    fcup_request((void *) conn, playback_location, apple_session_id, get_next_FCUP_RequestID(conn->raop->airplay_video));

    if (playback_location) {
        free (playback_location);
    }

    if (req_root_node) {
        plist_free(req_root_node);
    }
    return;

 play_error:;
    if (req_root_node) {
        plist_free(req_root_node);
    }
    logger_log(conn->raop->logger, LOGGER_ERR, "Could not find valid Plist Data for POST/play request, Unhandled");
    http_response_init(response, "HTTP/1.1", 400, "Bad Request");
    http_response_set_disconnect(response, 1);
    conn->raop->callbacks.conn_reset(conn->raop->callbacks.cls, 2);
}

/* the HLS handler handles http requests GET /[uri] on the HLS channel from the media player to the Server, asking for
   (adjusted) copies of Playlists: first the Master Playlist  (adjusted to change the uri prefix to
   "http://localhost:[port]/.......m3u8"), then the Media Playlists that the media player wishes to use.  
   If the client supplied Media playlists with the "YT-EXT-CONDENSED-URI" header, these must be adjusted into
   the standard uncondensed form before sending with the response.    The uri in the request is  the uri for the
   Media Playlist, taken from the Master Playlist, with the uri prefix removed.  
*/ 

static void
http_handler_hls(raop_conn_t *conn,  http_request_t *request, http_response_t *response,
                 char **response_data, int *response_datalen) {
    const char *method = http_request_get_method(request);
    assert (!strcmp(method, "GET"));
    const char *url = http_request_get_url(request);    
    const char* upgrade = http_request_get_header(request, "Upgrade");
    if (upgrade) {
        //don't accept Upgrade: h2c request ?
        char *header_str = NULL;
        http_request_get_header_string(request, &header_str);
        logger_log(conn->raop->logger, LOGGER_INFO,
                   "%s\nhls upgrade request declined", header_str); 
        free (header_str);
        return;
    }

    if (!strcmp(url, "/master.m3u8")){
        char * master_playlist  = get_master_playlist(conn->raop->airplay_video);
	if (master_playlist) {
            size_t len = strlen(master_playlist);
            char * data = (char *) malloc(len + 1);
            memcpy(data, master_playlist, len);
            data[len] = '\0';
            *response_data = data;
            *response_datalen = (int ) len;
        } else {
            logger_log(conn->raop->logger, LOGGER_ERR,"requested master playlist %s not found", url); 
            *response_datalen = 0;
        }

    } else {
        char *media_playlist = get_media_playlist(conn->raop->airplay_video, url);
        if (media_playlist) {
            char *data  = adjust_yt_condensed_playlist(media_playlist);
            *response_data = data;
            *response_datalen = strlen(data);
            float duration = 0.0f;
            int chunks = analyze_media_playlist(data, &duration);
            logger_log(conn->raop->logger, LOGGER_INFO,
                       "Requested media_playlist %s has %5d chunks, total duration %9.3f secs", url, chunks, duration); 
        } else {
            logger_log(conn->raop->logger, LOGGER_ERR,"requested media playlist %s not found", url); 
            *response_datalen = 0;
        }
    }

    http_response_add_header(response, "Access-Control-Allow-Headers", "Content-type");
    http_response_add_header(response, "Access-Control-Allow-Origin", "*");
    const char *date;
    date = gmt_time_string();
    http_response_add_header(response, "Date", date);
    if (*response_datalen > 0) {
        http_response_add_header(response, "Content-Type", "application/x-mpegURL; charset=utf-8");
    } else if (*response_datalen == 0) {
        http_response_init(response, "HTTP/1.1", 404, "Not Found");
    }
}
