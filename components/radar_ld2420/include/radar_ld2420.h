/* Public API of the radar_ld2420 component (pumpkin firmware, Fase 2).
 *
 * HLK-LD2420 24 GHz presence radar on UART: presence events on the
 * PRESENCE_EVENT base, trigger logic, module configuration with
 * write-only-if-different boot sync, fault recovery and calibration.
 *
 * The physical module was identified as an LD2420 from its pin header
 * (3V3, GND, OT1, RX, OT2) on 2026-10-25; the firmware speaks the LD2420
 * protocol. See docs/wiki/hardware/hlk-ld2420.md for the wiring and the
 * OT1/OT2 role swap across module firmware versions.
 */
#ifndef RADAR_LD2420_H
#define RADAR_LD2420_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
#include "radar_protocol.h"
#include "radar_trigger.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(PRESENCE_EVENT);

/* Event ids posted on PRESENCE_EVENT; event_data is radar_event_data_t. */
typedef enum {
    RADAR_EVENT_PRESENCE_ON,      /* trigger confirmed */
    RADAR_EVENT_PRESENCE_OFF,     /* hold expired or hysteresis exit */
    RADAR_EVENT_DISTANCE_UPDATED, /* detection distance changed */
    RADAR_EVENT_RADAR_FAULT,      /* sensor mute beyond the fault timeout */
    RADAR_EVENT_RADAR_OK,         /* frames flowing again after a fault */
} radar_event_t;

typedef struct {
    bool presence;        /* latched trigger */
    bool degraded;        /* running on the presence pin only (sensor mute) */
    uint16_t distance_mm; /* module-reported distance (cm on the wire), 0 = none */
} radar_event_data_t;

/* Serial output mode the component switched the module to. */
typedef enum {
    RADAR_MODULE_MODE_RUNNING = 0, /* text lines, fw <= 1.5.3 (or requested) */
    RADAR_MODULE_MODE_ENERGY,      /* binary energy frames, fw >= 1.5.4 */
} radar_module_mode_t;

/* Overall snapshot for console/HA layers. */
typedef struct {
    bool presence;
    bool fault;
    radar_module_mode_t mode;
    uint16_t distance_mm;
    bool gates_valid;
    uint16_t gate_energy[RADAR_MAX_GATES];
    char fw_text[16];       /* as reported by the module ("v1.5.34") */
    uint32_t baud;          /* detected UART baud (0 before the probe) */
    int64_t last_frame_us;
    /* diagnostics */
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t bytes_dropped;
    uint32_t recoveries;
    bool calibrating;
    bool degraded;     /* true while presence reporting is degraded to the
                        * OUT pin (fault active) */
    bool raw_presence; /* presence flag of the last decoded frame, before
                        * trigger latching (stale while faulted) */
} radar_state_t;

/* Calibration proposal (never written without an explicit apply). */
typedef struct {
    bool valid;
    uint32_t samples;
    uint16_t peak[RADAR_MAX_GATES];     /* observed empty-room max energy */
    uint32_t move[RADAR_MAX_GATES];     /* proposed trigger thresholds */
    uint32_t still[RADAR_MAX_GATES];    /* proposed keep thresholds */
} radar_calibration_t;

/* Start the UART task, probe the baud rate (Kconfig 0 = auto 115200/256000),
 * read the module firmware version, switch it to energy mode when supported
 * (running mode otherwise) and sync the desired configuration from
 * NVS/Kconfig writing only what differs. Safe no-op when the sensor is
 * absent: the fault/recovery path takes over. */
esp_err_t radar_init(void);

/* Copy of the current state snapshot. */
esp_err_t radar_get_state(radar_state_t *out);

/* Live module configuration read back from the module (one config
 * session): gate limits and both 16-gate threshold banks. */
typedef struct {
    uint8_t min_gate;   /* 0..15 */
    uint8_t max_gate;   /* 0..15 */
    uint16_t timeout_s; /* presence hold, 0..255 s */
    uint32_t move_thresh[RADAR_MAX_GATES];  /* trigger levels, gate 0..15 */
    uint32_t still_thresh[RADAR_MAX_GATES]; /* keep levels, gate 0..15 */
} radar_module_params_t;

esp_err_t radar_read_module_params(radar_module_params_t *out);

/* Runtime trigger distance in mm; persisted in NVS namespace "radar". */
esp_err_t radar_set_trigger_mm(uint16_t trigger_mm);

/* Min/max detection gate (0..15) and module presence hold in seconds
 * (0..255): config session with verified ACK, persisted in NVS. */
esp_err_t radar_set_gate_limits(uint16_t min_gate, uint16_t max_gate,
                                uint16_t hold_s);

/* Write move (trigger) and still (keep) thresholds for one gate 0..15 or
 * RADAR_ALL_GATES; levels 0..65535. Config session, persisted in NVS. */
esp_err_t radar_set_gate_thresholds(uint16_t gate, uint32_t move_thresh,
                                    uint32_t still_thresh);

/* Switch the module serial output: energy mode (fw >= 1.5.4) or running
 * mode. Engineering/testing helper; init selects the best mode already. */
esp_err_t radar_set_engineering(bool energy_on);

/* Read the module firmware version (ASCII, e.g. "v1.5.34"). */
esp_err_t radar_read_fw_version(radar_fw_version_t *out);

/* Module control. radar_factory_reset writes the ESPHome factory table
 * (the LD2420 has no dedicated reset command); console-only behind the
 * explicit "radar factoryreset confirm". */
esp_err_t radar_restart_module(void);
esp_err_t radar_factory_reset(void);

/* Collect per-gate energy for `seconds` in energy mode on an empty room
 * (async; state via radar_get_state()->calibrating, result via
 * radar_calibrate_get). Proposes thresholds = peak * RADAR_CAL_MOVE_PCT/100
 * (trigger) and * RADAR_CAL_STILL_PCT/100 (keep); nothing is written. */
esp_err_t radar_calibrate(uint32_t seconds);
esp_err_t radar_calibrate_get(radar_calibration_t *out);
/* Write the proposal to the module and persist it in NVS so subsequent
 * boots compare instead of rewriting. */
esp_err_t radar_calibrate_apply(void);

#ifdef __cplusplus
}
#endif
#endif /* RADAR_LD2420_H */
