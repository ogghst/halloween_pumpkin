/* Motore zona occhi: 2 WS2812B in coda alla catena corpo (Fase 8).
 * Puro C, nessuna dipendenza IDF salvo esp_err.h: il clock e' INIETTO
 * (parametro now_us) cosi' il modulo e' compilabile e testabile su host.
 * I renderers producono colori LINEARi: gamma e brightness sono applicati
 * dallo stadio di uscita di led_fx.c tramite una LUT dedicata alla zona
 * occhi (stessa gamma e CONFIG_LED_FX_MAX_BRIGHTNESS_PCT del corpo).
 *
 * Semantica delle timeline: breathe/color_cycle/blink sono funzioni
 * ASSOLUTE del tempo iniettato (cambiare effetto non riparte dalla fase
 * zero); un oneshot (flash/glare) sovrappone la propria timeline al look
 * base (occhi in simmetria, senza blink) e alla fine il base riprende.
 * Il colore base raggiunge il target con un lerp lineare su
 * FX_EYES_TRANSITION_MS: la transizione COOLDOWN scare->idle e' progressiva.
 */
#ifndef FX_EYES_H
#define FX_EYES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "fx_types.h"
#include "led_fx.h"

/* Costanti flash/glare, da specifica Fase 8. */
#define FX_EYES_FLASH_MS        150u     /* durata default del flash */
#define FX_EYES_GLARE_RISE_MS    40u     /* salita lineare nero -> pieno */
#define FX_EYES_GLARE_STROBO_HZ  15u     /* frequenza dello strobo */
#define FX_EYES_GLARE_STROBO_MS 400u     /* finestra dello strobo */
#define FX_EYES_GLARE_FADE_MS   800u     /* dissolvenza lineare a 0 */
#define FX_EYES_GLARE_MAX_MS 3600000u    /* cap della durata glare (1 h) */

/* Periodi delle timeline periodiche. */
#define FX_EYES_BREATHE_PERIOD_MS 4000u   /* (1-cos(2*pi*t/T))/2 */
#define FX_EYES_CYCLE_PERIOD_MS   8000u   /* giro hue completo */

/* Glow continuo alternato (richiesta utente 2026-10-27): envelope di respiro
 * con pavimento (mai nero) su una palette di colori in crossfade. */
#define FX_EYES_GLOW_COLORS       6u      /* colori della palette (hue passo costante) */
#define FX_EYES_GLOW_COLOR_MS     3000u   /* durata di un colore */
#define FX_EYES_GLOW_XFADE_PCT    25u     /* % finale del segmento in crossfade */
#define FX_EYES_GLOW_FLOOR        24u     /* pavimento envelope 0-127 (~19%) */

/* Blink: finestra di spegnimento e intervallo casuale tra i blink. */
#define FX_EYES_BLINK_MS          120u
#define FX_EYES_BLINK_MIN_MS     2000u
#define FX_EYES_BLINK_MAX_MS     8000u

/* Durata del lerp del colore base. Su target il valore arriva da Kconfig
 * (CONFIG_LED_FX_EYES_TRANSITION_MS, definita da sdkconfig.h); su host i
 * simboli Kconfig non esistono: fallback al default Kconfig, sovrascrivibile
 * dai test con -DFX_EYES_TRANSITION_MS=... */
#ifndef FX_EYES_TRANSITION_MS
#ifdef CONFIG_LED_FX_EYES_TRANSITION_MS
#define FX_EYES_TRANSITION_MS CONFIG_LED_FX_EYES_TRANSITION_MS
#else
#define FX_EYES_TRANSITION_MS 1500u
#endif
#endif

/** Inizializza il motore occhi: seed del PRNG del blink (xorshift32, 0 ->
 *  seed di riserva fisso) e sfasamento tra occhio 0 e occhio 1 (con desync
 *  l'occhio 1 e' in ritardo di 300 ms su breathe/color_cycle e di 130 ms
 *  sulla finestra di blink; con false perfettamente sincrono). */
void fx_eyes_init(uint32_t seed, bool desync);

/** Indice nel registro occhi per nome; -1 se sconosciuto. */
int fx_eyes_find_effect(const char *name);

/** Registro occhi (storage statico); ritorna il numero di effetti. */
size_t fx_eyes_get_effects(const led_fx_eyes_effect_info_t **out);

/** Validazione di un oneshot (usata dal wrapper pubblico PRIMA di accodare):
 *  solo flash/glare; glare richiede duration_ms > 0, nessuno supera il cap. */
esp_err_t fx_eyes_validate_oneshot(int idx, uint32_t duration_ms);

/* Comandi del motore: chiamati SOLO dal consumatore della coda (task anim
 * di led_fx.c). Le modifiche di stato (effetto, target, brightness, flag)
 * sono immediate; gli istanti legati al tempo (inizio lerp, partenza
 * oneshot) vengono ancorati al now_us del render successivo, cosi' la
 * sequenza drain-comandi/render resta deterministica sui test host. */

/** Seleziona l'effetto base per indice del registro (mai un oneshot). */
void fx_eyes_set_effect_idx(int idx);

/** Nuovo colore target del look base (lerp ancorato al prossimo render). */
void fx_eyes_set_color(uint8_t r, uint8_t g, uint8_t b);

/** Luminosita' della zona occhi 0-255 (solo stadio di uscita). */
void fx_eyes_set_brightness(uint8_t brightness);

/** Blink casuale sovrapposto al look base. */
void fx_eyes_set_blink_overlay(bool enable);

/** Gate del piano FSM: con false apply_fsm_plan e' ignorato. */
void fx_eyes_set_follow(bool enable);

/** Richiede un oneshot (flash: duration_ms 0 = default; glare: > 0).
 *  Con color_set=false il colore oneshot e' il target base al momento
 *  della partenza ("colore base a piena scala" del flash). */
esp_err_t fx_eyes_play_oneshot(int idx, uint32_t duration_ms, bool color_set,
                               const fx_rgb_t *color);

/** Applica il piano guidato dalla FSM (solo con follow attivo). */
esp_err_t fx_eyes_apply_fsm_plan(const led_fx_eyes_plan_t *plan);

/** Luminosita' corrente della zona occhi (per la LUT di uscita). */
uint8_t fx_eyes_brightness(void);

/** Renderizza i 2 pixel occhi (colori LINEARi) all'istante now_us. */
void fx_eyes_render(fx_rgb_t px[2], uint64_t now_us);

/** Copia thread-safe dello stato corrente (seqlock: il task anim e'
 *  l'unico scrittore, il lettore ritenta su copia incompleta). */
esp_err_t fx_eyes_get_look(led_fx_eyes_look_t *out);

#endif /* FX_EYES_H */
