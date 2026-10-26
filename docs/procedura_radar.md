# Procedura radar — installazione e calibrazione (Fase 2)

> **Modulo fisico HLK-LD2420 e firmware adattato (2026-10-25)**: il modulo ha i pin
> 3V3/GND/OT1/RX/OT2 ([HLK-LD2420](wiki/hardware/hlk-ld2420.md)) e il firmware ora **parla il
> protocollo LD2420** (componente [radar_ld2420](wiki/componenti/radar_ld2420.md)): le sezioni
> console e calibrazione (§3–§7) valgono per il LD2420 — sensibilità in **u32 0..65535**,
> **niente comando `resolution`**, calibrazione a percentuale del picco. Il vecchio layer
> protocollo LD2410B v1.07 è stato rimosso (riferimento storico:
> [HLK-LD2410B](wiki/hardware/hlk-ld2410b.md) e sue
> [errata](wiki/insidie/ld2410b-protocollo-errata.md)).

Riferimenti: pagina hardware [HLK-LD2420](wiki/hardware/hlk-ld2420.md), pagina componente
[radar_ld2420](wiki/componenti/radar_ld2420.md), errata del protocollo v1.07 (storiche)
[ld2410b-protocollo-errata](wiki/insidie/ld2410b-protocollo-errata.md).

> **Nessun numero di questa procedura è misurato con il modulo alimentato**: al banco
> (2026-10-25) è stata verificata solo la gestione del guasto con sensore non alimentato
> (FAULT ~2 s, degrado su OUT, recovery con backoff 1→2→4→8→16→30 s). Tutto ciò che proviene
> dal protocollo HLK-LD2420 è marcato **assunto** e va confermato a banco.

## 1. Posizionamento

- Il modulo va **dietro la zucca, antenna verso l'esterno**, puntato verso la zona di avvicinamento.
- **Lontano dall'induttore del boost** e dalle linee dati dei LED: il radar a 24 GHz vede
  alimentatori commutati e cavi in movimento come possibili bersagli (assunto, da verificare:
  test EMI al §7).
- **Niente metallo, acqua o foglio schermante davanti all'antenna**: bloccano o riflettono il
  fascio (assunto).
- Il radar **attraversa plastica e pareti sottili**: se la zucca ha una parete sottile dietro,
  il modulo può vedere le persone nell'ambiente adiacente. Difesa: **limitare il cancello
  massimo** (`radar limits <min> <max> <durata_s>`, persistito in NVS) in modo che la
  portata effettiva si fermi entro la zona desiderata.
- Falsi positivi noti di questa famiglia di radar: **ventole, tende mosse dall'aria, animali**
  (assunto). Da ricercare nel test del §6, non a integrazione fatta.

## 2. Cablaggio (modulo fisico HLK-LD2420)

```text
LDO 3,3 V (pin 3V3 D1 mini) ──► 3V3 radar       [MAI il boost a 5 V: il pin è a 3,3 V]
GND comune ──► GND radar

Radar RX  ◄── GPIO27 (RADAR_TX)      dati verso il modulo
Radar OT2 ──► GPIO26 (RADAR_RX)      dati dal modulo      [fw < 1.5.3; fw ≥ 1.5.3: OT1]
Radar OT1 ──► GPIO25 (RADAR_OUT)     uscita presenza      [fw < 1.5.3; fw ≥ 1.5.3: OT2]

Filtro alimentazione: ferrite o LC + 100 nF ceramico vicino al modulo
(100 µF elettrolitico consigliato sul 3,3 V del LDO).
```

- **ATTENZIONE: il ruolo di OT1/OT2 dipende dalla versione firmware** (<1.5.3: OT1 = uscita
  presenza, OT2 = TX seriale; ≥1.5.3: invertiti) e con esso il baud (256000 vs 115200):
  leggere la versione firmware (sonda UART da PC, o prova per ipotesi e verifica dell'uscita
  presenza) prima di cablare in modo definitivo — [HLK-LD2420](wiki/hardware/hlk-ld2420.md).
- Segnali UART e uscita presenza sono a **logica 3,3 V**: collegamento diretto all'ESP32, mai
  5 V. Alimentazione **3,3 V dal LDO** (stessa riga dell'ESP32: consumo del radar da misurare, §8).
- Pin di progetto invariati ([ADR-006](wiki/decisioni/adr-006-pivot-esp32-d1-mini.md)):
  RADAR_RX=26, RADAR_TX=27, RADAR_OUT=25; `RADAR_EN` resta non cablato (−1).
- **Il firmware parla LD2420 dal 2026-10-25**: al boot sonda il baud (115200/256000), legge la
  versione fw e sceglie la modalità seriale (frame energia per fw ≥ 1.5.4, righe testo per
  fw ≤ 1.5.3). Le sezioni 3–7 sotto sono le procedure del protocollo LD2420; la §8 misura il
  consumo reale del modulo a 3,3 V.

## 3. Comandi console

App di test `test_apps/radar_ld2420`: `idf.py -C test_apps/radar_ld2420 flash monitor`.

| Comando | Effetto |
| ------- | ------- |
| `radar status` | istantanea: presenza, distanza, modalità seriale, guasto/degrado, contatori |
| `radar version` | versione firmware del modulo (0x0000, es. "v1.5.34") e capacità frame energia |
| `radar params` | parametri correnti del modulo (0x0008): cancelli min/max, timeout, 32 soglie |
| `radar trigger <mm>` | distanza di trigger (persistita in NVS namespace `radar`) |
| `radar sensitivity <gate\|all> <move> <still>` | soglie trigger/keep per cancello, **u32 0..65535** (0x0007, persistita in NVS) |
| `radar limits <min> <max> <durata_s>` | cancelli min/max (0..15) e tenuta presenza in s (0x0007, persistita in NVS) |
| `radar engineering <on\|off>` | modalità seriale: frame energia (fw ≥ 1.5.4) / righe testo; **niente `resolution`**: il LD2420 non ha comando risoluzione |
| `radar restart` | riavvio del modulo (0x0068); il componente ripete probe/mode/boot sync |
| `radar factoryreset confirm` | il LD2420 **non ha comando di reset**: riscrive la tabella di fabbrica ESPHome e cancella la NVS `radar` |
| `radar calibrate [secondi]` | campiona la stanza vuota in modalità energia e propone le soglie (default 60 s) |
| `radar calibrate show` | mostra la proposta di calibrazione |
| `radar calibrate apply` | scrive le soglie proposte sul modulo (0x0007) e le persiste in NVS |
| `radar csv <on\|off>` | log CSV dello stato sulla console per l'analisi a banco |

Comandi `led_effects` già esistenti usati qui sotto: `fx`, `power`, `bright`
([elenco completo](wiki/componenti/led_effects.md)).

## 4. Sequenza di calibrazione delle soglie

1. **Stanza vuota**: nessuna persona e nessun oggetto in movimento nel raggio (ventole spente,
   tende ferme).
2. Serve la **modalità energia** (fw ≥ 1.5.4: verificarlo con `radar version`); poi
   `radar calibrate 60` (durata in secondi, default 60, range 10–600): il componente campiona
   l'energia per cancello e calcola una proposta (asincrona, `calibrating` in `radar status`).
3. `radar calibrate show`: proposta = **picco di rumore osservato per cancello × percentuale
   Kconfig** (`RADAR_CAL_MOVE_PCT` 250 % per la soglia trigger, `RADAR_CAL_STILL_PCT` 225 %
   per la tenuta). **Niente viene scritto sul modulo finché non c'è apply.**
4. **Controllo delle soglie proposte**: verificare che il picco sia stabile (stanza davvero
   vuota) e che le soglie non rendano sordi i cancelli lontani: una soglia molto più alta
   dell'energia di un bersaglio reale riduce la sensibilità di quel cancello.
5. `radar calibrate apply` **solo dopo conferma visiva**: scrive le due banche di soglie
   (0x0007) e le persiste in NVS namespace `radar`, così ai boot successivi il sync confronta
   invece di riscrivere.

## 5. Verifica distanze a 1/2/3 m

- Posare marcatori a 1, 2 e 3 m sull'asse di punta; far camminare una persona lungo l'asse.
- Leggere la distanza con `radar status` (valore in mm; sul wire in cm) ad ogni passaggio e
  annotare.
- La distanza arriva dal **range** del frame LD2420: non esiste comando di risoluzione e la
  mappa **cancello→distanza** (16 cancelli) è interna al modulo, da caratterizzare al banco
  ([domande aperte](wiki/domande-aperte.md)).
- Criterio: la distanza letta deve crescere con i marcatori e restare stabile a bersaglio
  fermo; un raggio utile sbagliato si corregge con `radar trigger <mm>` o
  `radar limits <min> <max> <s>` (persistiti in NVS).

## 6. Test 30 min di falsi positivi (stanza vuota)

1. Stanza vuota, `radar csv on`, cronometro da 30 min.
2. Per gli **ultimi 10 min** accendere il carico LED al massimo per stressare l'alimentazione:
   `fx rainbow`, `bright 255`, `power on` (test EMI boost→radar).
3. Criteri di passata: **nessun `PRESENCE_ON`**, `radar status` sempre `presence=false`, contatore
   `frames_bad`/`bytes_dropped` stabile, nessun `RADAR_EVENT_RADAR_FAULT`.
4. Se compaiono falsi positivi, leggere con `radar engineering on` **quale cancello** si muove:
   quello indica la sorgente (ventilazione, boost, parete) e si allontana alzando la soglia di
   quel cancello (o limitandolo con `radar limits`).

## 7. Hot-plug UART

1. Con l'app in esecuzione, **scollegare a caldo** il connettore del radar: entro
   `CONFIG_RADAR_FAULT_TIMEOUT_MS` (default 2000 ms) atteso log/evento `RADAR_EVENT_RADAR_FAULT`
   e degrado sul pin OUT (lettura GPIO25).
2. **Ricollegare**: il percorso di recovery con backoff deve riacquisire i frame e pubblicare
   `RADAR_EVENT_RADAR_OK` (`recoveries` in `radar status` aumenta di 1).
3. Ripetere 3 volte; verificare che nessun riavvio dell'ESP32 sia necessario.

**Verificato al banco (2026-10-25) con il sensore non alimentato**: FAULT latched a ~2 s con
evento, `degraded=true` (presenza pilotata dal pin OUT), console viva e graceful, recovery con
**backoff esponenziale 1→2→4→8→16→30 s** (un bug ms-vs-us nel calcolo del backoff fu trovato e
corretto proprio in quel test). Da ripetere con il modulo alimentato per il percorso dati completo.

## 8. Misura consumo 3,3 V

- Multimetro in serie sul filo **3,3 V** del modulo (o shunt/INA219): stanza vuota e con
  persona nella zona; annotare i due valori (il precedente ~80 mA @ 5 V era un assunto del
  LD2410B, non applicabile a questo modulo).
- Verificare anche che **LDO + ESP32 + radar** reggano il picco complessivo (tensione stabile).
- Aggiornare [bilancio energetico](wiki/misure/bilancio-energetico-stima.md) e
  [domande aperte](wiki/domande-aperte.md) con i numeri veri.

## 9. Checklist di accettazione

- [ ] Versione firmware letta (`radar version`) e registrata in wiki.
- [ ] Polarità OUT verificata (default assunto: alto = presente).
- [ ] Calibrazione proposta, controllata e applicata (§4); soglie registrate.
- [ ] Distanze 1/2/3 m plausibili e stabili, mappa cancello→distanza annotata (§5).
- [ ] 30 min stanza vuota senza falsi positivi, di cui 10 min a rainbow 100% (§6).
- [ ] Consumo 3,3 V misurato (§8).
- [ ] Hot-plug UART con recovery senza reboot (§7).
