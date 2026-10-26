#pragma once
/* Costruzione payload JSON e stati (pura, usa cJSON).
 * Le funzioni che restituiscono char* allocano con malloc: caller fa free(). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ha_registry.h"

typedef struct {
    const char *device_id;    /* es. "a1b2c3" */
    const char *name;         /* es. "Zucca pumpkin-a1b2c3" */
    const char *manufacturer;
    const char *model;
    const char *sw_version;
    const char *origin_name;
    const char *origin_sw;
} ha_device_info_t;

char *ha_payload_discovery(const ha_device_info_t *dev, const char *base_topic,
                           const char *availability_topic);
char *ha_payload_light(bool on, bool has_brightness, int brightness,
                       bool has_color, uint8_t r, uint8_t g, uint8_t b,
                       bool has_effect, const char *effect);
char *ha_payload_event_trigger(int32_t version, int64_t ts_ms, int32_t distance_mm,
                               const char *anim);
const char *ha_payload_onoff(bool on); /* "ON"/"OFF", stringa statica */
char *ha_payload_i32(int32_t v);
char *ha_payload_u32(uint32_t v);
char *ha_payload_f32(float v, int decimals);
char *ha_payload_text(const char *v); /* copia malloc della stringa */
