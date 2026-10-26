/* Pure, clock-injected core of the pumpkin FSM (host-testable).
 *
 * The core owns the state and all decisions. It never calls FreeRTOS,
 * esp_timer, led_fx or config_store: time comes from the injected
 * fsm_clock_t, hardware effects are requested through fsm_output_t, and
 * persistence of command-driven config fields is requested through
 * fsm_output_t.persist (the IDF layer performs the config_store writes).
 * The IDF layer feeds inputs through its single queue and forwards the
 * outputs; see components/pumpkin_fsm/fsm.c.
 *
 * Timing model: at most one armed deadline at a time (TRIGGERED duration
 * or COOLDOWN). fsm_core_process() recomputes it on every input;
 * fsm_core_poll() compares it against the injected clock and performs the
 * expired-timeout transition. On target, an esp_timer one-shot posts an
 * FSM_IN_POLL wake-up to the FSM queue; on host, tests advance the fake
 * clock and call fsm_core_poll() directly.
 */
#ifndef FSM_CORE_H
#define FSM_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include "pumpkin_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* == CONFIG_ANIM_NAME_LEN; kept independent so the core does not include
 * config_store.h. */
#define FSM_ANIM_NAME_LEN 16

/* The subset of the persisted configuration the FSM consumes. The IDF
 * layer translates pumpkin_config_t into this at boot and on every
 * CONFIG_EVENT_CHANGED. */
typedef struct {
    uint16_t trigger_duration_s;
    uint16_t cooldown_s;
    bool auto_mode;
    char idle_anim[FSM_ANIM_NAME_LEN];
    char trigger_anim[FSM_ANIM_NAME_LEN];
    uint8_t idle_brightness_pct; /* IDLE/COOLDOWN look */
    uint8_t brightness_pct;      /* TRIGGERED look */
    uint8_t max_brightness_pct;  /* hard cap for every emitted look */
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
    bool power_on;               /* boot policy: false -> start in OFF */
} fsm_config_t;

/* Monotonic clock injection: esp_timer_get_time wrapper on target, fake
 * clock on host. */
typedef struct {
    void *ctx;
    int64_t (*now_us)(void *ctx);
} fsm_clock_t;

/* Inputs; one queue item each. FSM_IN_POLL asks the core to check its
 * deadline against the clock (timer wake-up on target). */
typedef enum {
    FSM_IN_PRESENCE_ON = 0,
    FSM_IN_PRESENCE_OFF,
    FSM_IN_POLL,
    FSM_IN_CMD_MODE,       /* u.mode */
    FSM_IN_CMD_LIGHT,      /* u.light */
    FSM_IN_CMD_EFFECT,     /* u.anim */
    FSM_IN_CMD_SCARE,
    FSM_IN_CONFIG_CHANGED, /* u.cfg: full translated configuration */
} fsm_input_type_t;

typedef struct {
    fsm_input_type_t type;
    /* Out: command acceptance. ESP_OK accepted/queued, ESP_ERR_INVALID_STATE
     * rejected by the current state (e.g. scare in OFF). Ignored elsewhere. */
    esp_err_t result;
    union {
        fsm_mode_t mode;
        struct {
            uint8_t r, g, b;
            uint8_t brightness_pct;
        } light;
        char anim[FSM_ANIM_NAME_LEN];
        fsm_config_t cfg;
    } u;
} fsm_input_t;

/* Config params the core asks the IDF layer to persist, with core-side
 * ids (the IDF layer maps them onto config_param_id_t). */
typedef enum {
    FSM_PERSIST_IDLE_ANIM = 0,
    FSM_PERSIST_TRIGGER_ANIM,
    FSM_PERSIST_IDLE_BRIGHTNESS,
    FSM_PERSIST_BRIGHTNESS,
} fsm_persist_param_t;

/* Side effects requested by the core; the IDF layer forwards them in
 * order. Only the do_* and persist fields flagged true are applied. */
typedef struct {
    bool state_changed;
    fsm_state_t prev_state;
    fsm_state_t new_state;

    bool do_effect; char effect[FSM_ANIM_NAME_LEN];
    bool do_color;  uint8_t r, g, b;
    bool do_brightness; uint8_t brightness_0_255; /* capped by max pct */
    bool do_power;  bool power_on;

    /* Rearm the single timer for timer_ms (0 = cancel). */
    bool timer_valid;
    uint32_t timer_ms;

    /* Command-driven config persistence (AUTO light/effect commands). */
    struct {
        bool anim;   fsm_persist_param_t anim_param;
        char anim_name[FSM_ANIM_NAME_LEN]; /* not "anim": bool anim above */
        bool bright; fsm_persist_param_t bright_param;
        uint8_t bright_pct;
        bool color;  uint8_t r, g, b;
    } persist;
} fsm_output_t;

/* Internal state; fields are private to the core. */
typedef struct {
    fsm_state_t state;
    fsm_state_t return_state; /* where scare/trigger returns (IDLE/MANUAL) */
    fsm_config_t cfg;
    fsm_clock_t clock;
    int64_t deadline_us;  /* valid when has_deadline */
    bool has_deadline;
    /* Manual look, shaped by light/effect commands in MANUAL. Entering
     * MANUAL seeds it with the look currently on display. */
    struct {
        char anim[FSM_ANIM_NAME_LEN];
        uint8_t r, g, b;
        uint8_t brightness_pct;
    } manual;
} fsm_core_t;

/* Reset the core: state per cfg.power_on (OFF or IDLE + idle look on the
 * next fsm_core_boot output), no deadline. Does not emit outputs. */
void fsm_core_init(fsm_core_t *f, const fsm_config_t *cfg,
                   const fsm_clock_t *clock);

/* Emit the boot presentation (power rail, initial look). Call once after
 * fsm_core_init. */
void fsm_core_boot(fsm_core_t *f, fsm_output_t *out);

/* Process one input (events, commands, config changes); sets in->result
 * for command inputs. */
void fsm_core_process(fsm_core_t *f, fsm_input_t *in, fsm_output_t *out);

/* Check the armed deadline against the clock; performs the TIMEOUT
 * transition when expired. Safe (no-op) any time. */
void fsm_core_poll(fsm_core_t *f, fsm_output_t *out);

#ifdef __cplusplus
}
#endif
#endif /* FSM_CORE_H */
