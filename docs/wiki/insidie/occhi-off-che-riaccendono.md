---
titolo: Occhi OFF che riaccendono da soli (persistenza asimmetrica)
tipo: insidia
stato: verificato
aggiornato: 2026-10-27
fonti: [banco 2026-10-27 (broker MQTT 192.168.1.10 con paho: `set/eyes {"state":"OFF"}` + `set/scare PRESS`, ciclo TRIGGERED 8 s + COOLDOWN 30 s, occhi tornati ON blink bianco 163 al ritorno IDLE); components/ha_entities/ha_entities.c (case HA_TARGET_EYES), components/eyes_ctrl/eyes_ctrl.c (eyes_resync), components/eyes_ctrl/eyes_ctrl_core.c (eyes_plan_build)]
correlate: [../componenti/ha-mqtt.md, ../componenti/eyes-ctrl.md, ../componenti/config_store.md, ../decisioni/adr-010-occhi-zone.md]
---

# Occhi OFF che riaccendono da soli (persistenza asimmetrica)

## Il sintomo

Da Home Assistant la light "Occhi" viene spenta: gli occhi vanno via ma,
dopo un po', tornano da soli — l'utente vedeva un "leggero flicker" e look
che riapparivano (nella sessione di banco il motore era rimasto su
`blink` bianco 64% mentre l'echo MQTT diceva `solid` 95% e HA era stato
messo OFF).

## La causa: due comportamenti asimmetrici che si sommano

1. **Persistenza solo in ON.** In `command_cb` (`ha_entities.c`) il ramo
   `HA_TARGET_EYES` persisteva in `config_store` il look di riposo solo nel
   ramo ON (brightness/color/effect); il ramo OFF chiamava solo
   `led_fx_eyes_set_effect("off")` sul motore, **senza toccare la config**.
   Il look idle persistito (es. `blink`) restava quindi in NVS.
2. **Resync di `eyes_ctrl`.** A ogni evento FSM (`STATE_CHANGED`) o config
   occhi, `eyes_resync()` ri-applica il piano dello stato corrente: in IDLE
   il piano = look idle **persistito** → gli occhi tornano col vecchio look.
   Un trigger del radar (soglia 400 mm: basta passare vicino) chiude il
   ciclo TRIGGERED→COOLDOWN→IDLE e al ritorno in IDLE gli occhi riaccendono.

Con effetto idle `blink` (finestre scure periodiche desincronizzate tra i
due occhi) il ritorno appare come un flicker, non come una accensione netta.

## Il contrasto col corpo

Per la luce del corpo OFF **non** è transiente: `light` OFF forza il modo
FSM OFF (persistente). Per gli occhi la scelta di design è diversa (la
light `eyes` comanda il motore, la FSM li racconta solo con follow ON), ma
la persistenza doveva restare simmetrica: in AUTO il look di riposo è
"*tutto il look*", OFF compreso.

## La correzione

In AUTO (`state != MANUAL`) anche `state: "OFF"` su `set/eyes` persiste
`CFG_PARAM_EYES_IDLE_EFFECT = "off"`; il `CONFIG_EVENT` fa risincronizzare
`eyes_ctrl` che ri-applica il piano IDLE con effetto off (già spenti:
idempotente). Verificato a banco: OFF → scare → cooldown → IDLE = occhi
restano OFF, echo `state/eyes` resta OFF.

## Da ricordare

- Ogni comando che cambia il look del motore in AUTO deve avere la controparte
  di persistenza, **anche i rami "negativi"** (OFF/volume 0/…): i resync
  guidati dagli eventi ricostruiscono sempre lo stato *dalla config*, mai
  dal motore.
- L'echo retained su `state/<key>` non basta a dedurre lo stato del motore:
  i comandi console (`eyes ...`) cambiano il motore senza echo MQTT. Al
  banco confrontare sempre `eyes` su console e `state/eyes` su MQTT.
