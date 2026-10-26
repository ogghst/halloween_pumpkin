---
titolo: Componente config_store
tipo: componente
stato: verificato
aggiornato: 2026-10-26
fonti: [components/config_store/include/config_store.h, components/config_store/config_store.c, components/config_store/config_store_core.c, components/config_store/Kconfig, components/config_store/host_tests (13 casi, verdi 2026-10-26)]
correlate: [pumpkin_fsm.md, ../decisioni/adr-007-fsm-config-store.md, ../../fsm.md, ../procedure/host-tests.md, ../domande-aperte.md]
---

# Componente `config_store`

Fonte unica di verità per i parametri utente della zucca (`components/config_store/`),
consegnato con la Fase 3. Come la FSM lo consuma: [docs/fsm.md](../../fsm.md).

**Stato**: logica pura **verificata su host** (13 test verdi: sanitize, migrate v1,
serialize/deserialize a layout noto); layer IDF (scrittura NVS reale, debounce) **non
ancora esercitato a banco** — persistenza dopo reboot e usura flash sono
[da confermare](../domande-aperte.md).

## Perché queste scelte

- **Una sola copia dei parametri** (decisione in [ADR-007](../decisioni/adr-007-fsm-config-store.md)):
  prima del componente ogni driver teneva la sua NVS (il radar ha ancora la sua copia
  interna, sincronizzata); senza una fonte unica, UI e Home Assistant (Fase 5)
  dovrebbero scrivere in più posti e il boot avrebbe semantica ambigua.
- **Debounce del commit NVS (2000 ms)**: limita l'usura della flash quando i parametri
  cambiano a raffica; `config_store_flush` committa subito (es. prima di un reboot
  voluto). Il valore è Kconfig (`CONFIG_STORE_DEBOUNCE_MS`, default 2000, range 100–60000).
- **Evento con id** (`CONFIG_EVENT` / `CONFIG_EVENT_CHANGED`, payload
  `config_change_t{id}`): i consumatori (la FSM) reagiscono al singolo parametro senza
  pollare; l'evento parte **subito** al set, non al commit.

## Come funziona

- Copia RAM protetta da **mutex FreeRTOS** (non portMUX: le sezioni critiche chiamano
  la NVS); i getter servono per copia (`config_store_get`), i setter sono tipizzati
  (`set_u16/set_pct/set_bool/set_color/set_anim`).
- Persistenza: namespace NVS **`pumpkin`**, chiavi `ver` (u8) + `cfg` (blob v1
  little-endian senza padding, layout stabile across struct changes).
- `config_store_init(defaults)`: chiama `nvs_flash_init` in modo idempotente, carica il
  blob, **migrazione** a catena fino a `PUMPKIN_CONFIG_VERSION` (v1; versione futura →
  `ESP_ERR_NOT_SUPPORTED`) e **sanifica** (clamp dei range, stringhe terminate, nomi
  effetto vuoti/troppo lunghi corretti); senza blob valido valgono i default Kconfig
  (sanificati, non mutati).
- Set con **valore identico = no-op**: né evento né commit; il loop-back di chi
  reagisce agli eventi suoi stessi è idempotente.
- Elenco parametri e default: [Kconfig](../../../components/config_store/Kconfig) e
  tabella in [docs/fsm.md](../../fsm.md).

## Cosa NON fare

- **Non leggere/scrivere le chiavi NVS a mano**: passa sempre dai setter tipizzati
  (validazione, evento e debounce sono lì).
- **Non riordinare né rimuovere campi del blob v1**: solo append in coda + bump di
  `PUMPKIN_CONFIG_VERSION` + step di migrate in `config_store_core.c` (i vecchi blob
  devono restare leggibili).
- **Non trattare `CONFIG_EVENT_CHANGED` come "commit avvenuto"**: arriva al set, la
  scrittura NVS è rimandata dal debounce; per forzarla, `config_store_flush`.
- **Non fidarsi dei nomi effetto caricati dalla NVS**: sono stringhe del registry
  `led_effects`, ma la validità è controllata solo al consumo (vedi
  [domande aperte](../domande-aperte.md)).
- Niente chiamate dai task critici in loop stretto: ogni set valida e (se cambia)
  pubblica l'evento — non è gratis.
