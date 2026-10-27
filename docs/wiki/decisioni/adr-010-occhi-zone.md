---
titolo: ADR-010 — Occhi come zona del motore LED
tipo: decisione
stato: assunto
aggiornato: 2026-10-27
fonti: [fase8, components/led_effects/fx_eyes.c, components/led_effects/include/led_fx.h, components/eyes_ctrl/, components/config_store/include/config_store.h (v3), components/ha_entities/ha_entities.c, brief Fase 8 2026-10-27]
correlate: [../componenti/led_effects.md, ../componenti/eyes-ctrl.md, ../componenti/config_store.md, ../componenti/ha-mqtt.md, adr-007-fsm-config-store.md, ../../../docs/pinout.md, ../../../docs/mqtt.md]
---

# ADR-010 — Occhi come zona del motore LED

## Contesto

La Fase 8 aggiunge 2 LED WS2812B "occhi", **dello stesso tipo** dei 64 del corpo,
collegati **in serie in coda alla stessa catena dati** (65° e 66° LED, indici
0-based 64 e 65; nessun GPIO aggiuntivo — la linea dati separata su GPIO18 resta
un'opzione non implementata). Serve: effetti propri per gli occhi sincronizzati
con lo spavento del corpo, controllo da Home Assistant, transition progressive
nel ritorno al riposo, e zero regressioni per chi usa solo il corpo.

## Scelta

- **Una zona, non un secondo driver**: un solo buffer `s_frame[66]`, un solo
  task `anim` a 60 fps, un solo flush RMT. Il corpo renderizza su `[0, EYES_START)`,
  gli occhi su `[EYES_START, EYES_START+2)` con il motore puro `fx_eyes.c`
  (clock iniettato in µs, host-testato). Il conteggio (`LED_FX_NUM_PIXELS`,
  default 66) e l'inizio zona occhi (`LED_FX_EYES_START`, default 64) sono in
  Kconfig: con `NUM_PIXELS <= EYES_START+1` la zona si disattiva con un warning
  (compatibilità con applicazioni ferme a 64 px).
- **Gamma, cap e stima corrente comuni; luminosità per zona**: due LUT di
  output (corpo e occhi) costruite con la stessa gamma e lo stesso cap
  `LED_FX_MAX_BRIGHTNESS_PCT`; l'overlay di stato (rete/provisioning) resta
  **solo sul corpo**; la stima corrente cammina l'intero frame (66 px: +120 mA
  a bianco pieno).
- **Effetti occhi con nome stringa** (registry proprio, same pattern del corpo):
  `off`, `solid`, `breathe`, `color_cycle`, `blink`, `glow` (glow continuo
  alternato su 6 colori in crossfade, con pavimento di luminosita'), `flash`
  (one-shot) e `glare` (one-shot a durata dinamica: salita 40 ms → mantenimento →
  strobo 15 Hz 400 ms → dissolvenza 800 ms; la durata totale è passata dal
  chiamante, così copre esattamente `trigger_duration_s`). Lieve sfasamento tra
  i due occhi (300 ms su breathe/hue/glow, 130 ms sulla finestra blink) via
  `LED_FX_EYES_DESYNC`.
- **La FSM non sa nulla degli occhi**: il mini-componente `eyes_ctrl` ascolta
  `FSM_EVENT_STATE_CHANGED` e `CONFIG_EVENT_CHANGED` e applica un **piano**
  (`led_fx_eyes_plan_t`) tramite `led_fx_eyes_apply_fsm_plan()`. Il piano passa
  dalla stessa coda comandi del task `anim` (nessun accesso diretto al
  framebuffer, nessun delay, nessun heap). Symmetric di [ADR-007](adr-007-fsm-config-store.md):
  config_store resta fonte unica, gli occhi sono periferica del look.
- **Il gate `eyes_follow_fsm` è doppio**: `eyes_ctrl` non costruisce né invia
  il piano se `eyes_follow_fsm` o `eyes_enabled` sono falsi (e in `MANUAL`
  non invia nulla: HA comanda), e il motore occhi ignora comunque i piani se
  `led_fx_eyes_follow_fsm(false)`. Gli effetti base settati via API pubblica
  (HA/console) non sono mai gate-ati: con il follow disattivato gli occhi
  restano come impostato.
- **Transizione di riposo = lerp, non stato**: il colore base degli occhi
  insegue il target in `LED_FX_EYES_TRANSITION_MS` (default 1500 ms); il
  piano `COOLDOWN` si limita a ripristinare il look idle e il lerp produce la
  transizione progressiva dal colore di spavento a quello di riposo. Il glare,
  invece, ha envelope propria (istantanea sulla salita: max luminosità entro
  50 ms dall'inizio dello spavento).
- **Config v3**: 7 parametri occhi in coda al blob (209 → 235 B, migrazione
  2→3 con default identici ai Kconfig; vedi [config_store](../componenti/config_store.md)).
- **Cinque nuove entità HA** (registro da 16 a 24 slot): light `eyes` (JSON con
  brightness+RGB+effect_list dei soli effetti persistenti), i due light
  colore-only `eyes_idle_color`/`eyes_scare_color` (nuovo tipo `HA_ET_LIGHT_RGB`)
  e i due switch `eyes_enabled`/`eyes_follow_fsm`; echo `state/eyes` dal nuovo
  evento `LED_FX_EVENT`. Dettagli in [docs/mqtt.md](../../../docs/mqtt.md).

## Conseguenze

- Nessuna regressione per il corpo: render, pixel test, mappatura serpentina e
  overlay invariati; il corpo vede `n = EYES_START` invece di `NUM_PIXELS`
  (rainbow distribuito su 64 px come prima).
- L'evento `LED_FX_EVENT_EYES` (post dal task `anim` dopo ogni comando occhi
  applicato) è il canale di echo per HA e console: nessun polling.
- Costo IRAM/flash dell'aggiunta è a carico di `fx_eyes.c` (~1,5 KB flash,
  nessun buffer extra: 2 pixel in coda al frame esistente).
- **Da verificare a banco** (criteri Fase 8): tensione al pixel 65 ≥ 4,5 V a
  bianco pieno, corrente reale entro ±15% dalla stima, 30 min a 60 fps senza
  frame persi, 1 ora con 100 trigger — vedi [domande aperte](../domande-aperte.md).
