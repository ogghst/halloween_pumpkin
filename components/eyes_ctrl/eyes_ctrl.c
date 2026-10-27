/* Layer IDF di eyes_ctrl (pumpkin firmware, Fase 8): sottoscrive gli
 * eventi FSM/config sul default event loop e pilota gli occhi tramite
 * led_fx_eyes_apply_fsm_plan / led_fx_eyes_follow_fsm. */
#include "eyes_ctrl.h"

#include <stdbool.h>

#include "esp_event.h"
#include "esp_log.h"

#include "config_store.h"
#include "eyes_ctrl_core.h"
#include "led_fx.h"
#include "pumpkin_fsm.h"

static const char *TAG = "eyes_ctrl";

static bool s_initialized;

/* Rilegge la configurazione, risincronizza il gate led_fx_eyes_follow_fsm
 * e ri-applica il piano dello stato corrente della FSM. Con gli occhi
 * disabilitati li spegne una volta (effetto "off"). Contesto: task del
 * default event loop, mai ISR: niente delay, niente heap. */
static void eyes_resync(void)
{
    pumpkin_config_t cfg;

    if (config_store_get(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "config_store_get fallita: occhi non toccati");
        return;
    }

    led_fx_eyes_follow_fsm(eyes_follow_allowed(&cfg));
    if (!cfg.eyes_enabled) {
        led_fx_eyes_set_effect("off");
        return;
    }

    led_fx_eyes_plan_t plan;
    if (eyes_plan_build(pumpkin_fsm_get_state(), &cfg, &plan)) {
        led_fx_eyes_apply_fsm_plan(&plan);
    }
    /* Gate chiuso o stato MANUAL: nessun tocco, gli occhi restano liberi. */
}

static void on_fsm_state_changed(void *arg, esp_event_base_t base,
                                 int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)id;

    const fsm_state_change_t *ev = (const fsm_state_change_t *)event_data;
    ESP_LOGI(TAG, "stato FSM %d -> %d: risincronizzo gli occhi",
             (int)ev->prev_state, (int)ev->state);
    eyes_resync();
}

/* Parametri config che riguardano gli occhi: solo su questi si risincronizza. */
static bool is_eyes_param(config_param_id_t id)
{
    switch (id) {
    case CFG_PARAM_EYES_ENABLED:
    case CFG_PARAM_EYES_FOLLOW_FSM:
    case CFG_PARAM_EYES_IDLE_COLOR:
    case CFG_PARAM_EYES_SCARE_COLOR:
    case CFG_PARAM_EYES_IDLE_EFFECT:
    case CFG_PARAM_EYES_IDLE_BRIGHTNESS_PCT:
    case CFG_PARAM_EYES_SCARE_BRIGHTNESS_PCT:
        return true;
    default:
        return false;
    }
}

static void on_config_changed(void *arg, esp_event_base_t base,
                              int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)id;

    const config_change_t *ev = (const config_change_t *)event_data;
    if (!is_eyes_param(ev->id)) {
        return;
    }
    ESP_LOGI(TAG, "parametro config occhi %d cambiato: risincronizzo",
             (int)ev->id);
    eyes_resync();
}

esp_err_t eyes_ctrl_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Primo allineamento prima di registrare i listener: nessun evento
     * perso né doppio apply tra registrazione e piano iniziale. */
    eyes_resync();

    esp_err_t err = esp_event_handler_register(FSM_EVENT,
                                               FSM_EVENT_STATE_CHANGED,
                                               on_fsm_state_changed, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(CONFIG_EVENT, CONFIG_EVENT_CHANGED,
                                     on_config_changed, NULL);
    if (err != ESP_OK) {
        (void)esp_event_handler_unregister(FSM_EVENT,
                                           FSM_EVENT_STATE_CHANGED,
                                           on_fsm_state_changed);
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "occhi agganciati alla FSM (pixel 64-65)");
    return ESP_OK;
}
