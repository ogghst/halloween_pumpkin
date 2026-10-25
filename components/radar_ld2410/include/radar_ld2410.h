/* Public API of the radar_ld2410 component (pumpkin firmware, Fase 2).
 *
 * HLK-LD2410B 24 GHz presence radar on UART1: presence events on the
 * PRESENCE_EVENT base, trigger logic, module configuration with
 * write-only-if-different boot sync, fault recovery and calibration.
 *
 * HARDWARE ASSUMPTIONS (verify on the bench, see docs/wiki):
 *  - module variant LD2410B, firmware version read at first boot;
 *  - factory baud 256000 8N1 (protocol v1.07 §2);
 *  - OUT pin: high = target present, low = none (§1.1 Table 1,
 *    default aux-control configuration 0x00).
 */
#ifndef RADAR_LD2410_H
#define RADAR_LD2410_H

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
    bool present_static;  /* static-only occupancy (no trigger) */
    bool degraded;        /* running on the OUT pin only (sensor mute) */
    uint16_t distance_mm; /* detection distance, quantized to gate width */
    uint8_t moving_energy;
} radar_event_data_t;

/* Overall snapshot for console/HA layers. */
typedef struct {
    bool presence;
    bool present_static;
    bool fault;           /* considered mute/degraded right now */
    bool engineering;
    uint16_t distance_mm; /* detection distance, quantized to gate width */
    uint8_t moving_energy;
    uint16_t gate_mm;     /* current resolution-derived gate width */
    uint16_t resolution_index;
    int64_t last_frame_us;
    /* raw module telemetry from the last report frame (§2.3.2); energies
     * are valid only in engineering mode (0 otherwise) */
    radar_target_state_t target_state;
    uint16_t moving_distance_mm;
    uint16_t static_distance_mm;
    uint8_t static_energy;
    /* diagnostics */
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t bytes_dropped;
    uint32_t recoveries;  /* successful recoveries since boot */
    bool calibrating;
} radar_state_t;

/* Calibration proposal (never written without an explicit apply). */
typedef struct {
    bool valid;
    uint32_t samples;
    uint8_t motion[RADAR_MAX_GATES + 1]; /* proposed sensitivity per gate */
    uint8_t static_sens[RADAR_MAX_GATES + 1];
    uint8_t max_energy[RADAR_MAX_GATES + 1]; /* observed empty-room max */
} radar_calibration_t;

/* Start the UART task, configure pins, sync module configuration
 * (write only what differs; nothing is written when already matching),
 * read fw version + resolution into the log. Safe no-op when the sensor
 * is absent: the fault/recovery path takes over. */
esp_err_t radar_init(void);

/* Copy of the current state snapshot. */
esp_err_t radar_get_state(radar_state_t *out);

/* Runtime trigger distance in mm; persisted in NVS namespace "radar". */
esp_err_t radar_set_trigger_mm(uint16_t trigger_mm);

/* Runtime gate limits on the module: max moving gate, max static gate
 * (2..RADAR_MAX_GATES) and the module "unmanned" duration in seconds
 * (0..65535). Config session with verified ACK (EINVAL outside the gate
 * range). On success the values are persisted in NVS (namespace "radar":
 * keys "max_mov" u8, "max_st" u8, "unmanned_s" u16), which are the same
 * desired limits the boot sync compares against the module: after this
 * call the next boot finds them already matching and writes nothing.
 * "radar factoryreset confirm" erases these keys again. */
esp_err_t radar_set_limits(uint16_t max_moving_gate, uint16_t max_static_gate,
                           uint16_t unmanned_s);

/* Configure gate sensitivity on the module (gate 0..max or RADAR_ALL_GATES;
 * levels 0..100). Config session with verified ACK. */
esp_err_t radar_set_sensitivity(uint16_t gate, uint8_t motion, uint8_t static_sens);

/* Engineering mode on/off. NOT persistent in the module (protocol §2.2.5):
 * the component re-applies its state after a module restart. */
esp_err_t radar_set_engineering(bool on);

/* Read module parameters / firmware version / range resolution. */
esp_err_t radar_read_module_params(radar_module_params_t *out);
esp_err_t radar_read_fw_version(radar_fw_version_t *out);
esp_err_t radar_read_resolution(uint16_t *index_out);

/* Module control. radar_factory_reset is exposed ONLY behind the console
 * command "radar factoryreset confirm"; the API stays available for it. */
esp_err_t radar_restart_module(void);
esp_err_t radar_factory_reset(void);

/* Collect per-gate energy for `seconds` in engineering mode on an empty
 * room (async; state via radar_get_state()->calibrating, result via
 * radar_calibrate_get). Proposes noise-max + CONFIG_RADAR_CAL_MARGIN
 * thresholds; nothing is written to the module. */
esp_err_t radar_calibrate(uint32_t seconds);
esp_err_t radar_calibrate_get(radar_calibration_t *out);
/* Write the proposal to the module (0x0064 per gate) and persist it in
 * NVS so subsequent boots compare instead of rewriting. */
esp_err_t radar_calibrate_apply(void);

#ifdef __cplusplus
}
#endif
#endif /* RADAR_LD2410_H */
