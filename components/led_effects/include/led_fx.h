/* Public API of the led_effects component (pumpkin firmware, Phase 1). */
#ifndef LED_FX_H
#define LED_FX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-point speed format: 256 == 1.0x. */
#define LED_FX_SPEED_ONE 256u

/* LED matrix geometry: 8x8 panel wired as a single serpentine chain.
 * Pixel 0 is the top-left cell; even rows run left->right, odd rows
 * right->left. W*H (64) is the body zone [0, CONFIG_LED_FX_EYES_START);
 * from Fase 8 the chain default CONFIG_LED_FX_NUM_PIXELS is 66: 64 body
 * pixels + 2 WS2812B eyes in tail (see the Fase 8 contract below).
 * HARDWARE ASSUMPTION: verify the actual wiring on the bench. */
#define LED_FX_MATRIX_W 8
#define LED_FX_MATRIX_H 8

/* Rendering statistics, updated by the anim task. */
typedef struct {
    uint32_t frames;        /* rendered frames since init */
    uint32_t dropped;       /* frames whose render+flush exceeded the frame period */
    uint32_t render_us_avg; /* EMA (weight 1/8) of per-frame render+flush time [us] */
    uint32_t render_us_max; /* worst-case per-frame render+flush time [us] */
} led_fx_stats_t;

/**
 * Start the anim task, create the command queue and the LED strip driver.
 * The LED rail stays OFF after init: no light until led_fx_power(true).
 */
esp_err_t led_fx_init(void);

/** Switch the LED power rail (see led_fx_is_powered for the resulting state). */
esp_err_t led_fx_power(bool on);
bool led_fx_is_powered(void);

/** Select effect by name (see led_fx_get_effect_list). Cancels a running oneshot. */
esp_err_t led_fx_set_effect(const char *name);

/** Set the base effect color (linear 0-255 per channel, before gamma/brightness). */
esp_err_t led_fx_set_color(uint8_t r, uint8_t g, uint8_t b);

/** Global brightness 0-255, applied only at the output stage. */
esp_err_t led_fx_set_brightness(uint8_t brightness);

/** Effect speed multiplier in Q8 (LED_FX_SPEED_ONE == normal speed). */
esp_err_t led_fx_set_speed(uint16_t speed_q8);

/** Play a one-shot effect (e.g. "scare") once, then restore the previous effect. */
esp_err_t led_fx_play_oneshot(const char *name);

/** Matrix cell (x, y) -> strip index (serpentine, origin top-left).
 *  Returns -1 when x or y is outside [0, LED_FX_MATRIX_W/H). */
int led_fx_xy_index(int x, int y);

/** Pixel-test hold: the anim task renders a black frame with only cell
 *  (x, y) lit (r/g/b linear 0-255) instead of the current effect, until
 *  led_fx_pixel_test_off. ESP_ERR_INVALID_ARG for out-of-matrix coords. */
esp_err_t led_fx_pixel_test(int x, int y, uint8_t r, uint8_t g, uint8_t b);

/** Release the pixel-test hold; the previous effect resumes with its clock
 *  untouched. ESP_OK also when no hold is active. */
esp_err_t led_fx_pixel_test_off(void);

/** Array of effect names in registry order (static storage); *out_count receives its length. */
const char *const *led_fx_get_effect_list(size_t *out_count);

/** Copy of current rendering statistics. */
void led_fx_get_stats(led_fx_stats_t *out);

/* --- Phase 4: status overlays (network/provisioning feedback) --- */

/* Dim overlay pattern drawn on top of the current effect: components are
 * capped at 48/255 per channel and fused with max() per channel AFTER
 * gamma/brightness, so the overlay stays visible even at low brightness.
 * Never applied during a pixel-test hold; with the rail off no frame is
 * rendered, so the overlay is invisible regardless. */
typedef enum {
    LED_FX_OVERLAY_NONE = 0,   /* no overlay (default, no visual change) */
    LED_FX_OVERLAY_PROV,       /* slow blue pulse, 2 s period */
    LED_FX_OVERLAY_CONNECTING, /* double blue blink, 2 s period */
    LED_FX_OVERLAY_NET_UP,     /* three green flashes in 1.5 s, then auto-NONE */
    LED_FX_OVERLAY_NET_DOWN,   /* slow red pulse, 3 s period */
} led_fx_overlay_t;

/** Select the overlay pattern. Queued like the other setters: applied by the
 *  anim task from the next frame on. ESP_ERR_INVALID_ARG for values past
 *  LED_FX_OVERLAY_NET_DOWN; re-selecting the active overlay keeps its phase. */
esp_err_t led_fx_overlay_set(led_fx_overlay_t ov);

/** Overlay currently selected (LED_FX_OVERLAY_NONE before any set; NET_UP
 *  reads as set until the anim task auto-clears it after 1.5 s). */
led_fx_overlay_t led_fx_overlay_get(void);

/* --- Phase 6 hook: power estimation (no active limiting yet) --- */

/** Estimate instantaneous LED rail current [mA] from the last rendered frame.
 *  Dalla Fase 8 copre l'intera catena: corpo 8x8 + zona occhi in coda
 *  (66 px con la configurazione di default). */
uint32_t led_fx_estimate_current_ma(void);

/** Global scale factor [percent, 1-100] applied by led_fx_estimate_current_ma. Default 100. */
esp_err_t led_fx_set_current_scale_pct(uint8_t pct);

/* --- Fase 8: zona occhi (2 pixel WS2812B in coda alla catena) --- */
ESP_EVENT_DECLARE_BASE(LED_FX_EVENT);
typedef enum { LED_FX_EVENT_EYES = 0 } led_fx_event_t;
typedef struct {
    char effect[16];      /* nome effetto base attivo (registro occhi) */
    uint8_t r, g, b;      /* colore target corrente (lineare 0-255) */
    uint8_t brightness;   /* luminosita zona occhi 0-255 */
    bool blink_overlay;   /* blink casuale sovrapposto attivo */
    bool oneshot_active;  /* flash/glare in corso */
} led_fx_eyes_look_t;
typedef enum {
    LED_FX_EYES_PLAN_IDLE = 0,
    LED_FX_EYES_PLAN_TRIGGERED,
    LED_FX_EYES_PLAN_COOLDOWN,
    LED_FX_EYES_PLAN_OFF,
} led_fx_eyes_plan_kind_t;
typedef struct {
    led_fx_eyes_plan_kind_t kind;
    uint8_t idle_r, idle_g, idle_b;
    char idle_effect[16];
    uint8_t idle_brightness;              /* 0-255 */
    uint8_t scare_r, scare_g, scare_b;
    uint8_t scare_brightness;             /* 0-255 */
    uint32_t duration_ms;                 /* durata glare = trigger_duration_s*1000 */
} led_fx_eyes_plan_t;
/** Seleziona l'effetto occhi per nome (registro occhi, vedi led_fx_eyes_get_effects). */
esp_err_t led_fx_eyes_set_effect(const char *name);
/** Colore base occhi (target: lerp su CONFIG_LED_FX_EYES_TRANSITION_MS). */
esp_err_t led_fx_eyes_set_color(uint8_t r, uint8_t g, uint8_t b);
/** Luminosita zona occhi 0-255, applicata solo all'uscita. */
esp_err_t led_fx_eyes_set_brightness(uint8_t brightness);
/** One-shot occhi ("flash"/"glare"), poi ritorno al look base. */
esp_err_t led_fx_eyes_play_oneshot(const char *name, uint32_t duration_ms);
/** Gate del piano FSM: con false led_fx_eyes_apply_fsm_plan e' ignorato. */
esp_err_t led_fx_eyes_follow_fsm(bool enable);
/** Blink casuale sovrapposto al look base (piano idle della FSM). */
esp_err_t led_fx_eyes_set_blink_overlay(bool enable);
/** Applica il piano guidato dalla FSM (solo se led_fx_eyes_follow_fsm(true)). */
esp_err_t led_fx_eyes_apply_fsm_plan(const led_fx_eyes_plan_t *plan);
/** Copia dello stato corrente degli occhi (thread-safe). */
esp_err_t led_fx_eyes_get_look(led_fx_eyes_look_t *out);
typedef struct { const char *name; bool oneshot; } led_fx_eyes_effect_info_t;
/** Registro occhi (storage statico); ritorna il numero di effetti. */
size_t led_fx_eyes_get_effects(const led_fx_eyes_effect_info_t **out);

#ifdef __cplusplus
}
#endif
#endif /* LED_FX_H */
