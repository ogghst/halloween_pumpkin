# Procedura radar — installazione e calibrazione (Fase 2)

> **Modulo fisico identificato come HLK-LD2420** (pin 3V3/GND/OT1/RX/OT2,
> [HLK-LD2420](wiki/hardware/hlk-ld2420.md)): il cablaggio (§2) è quello del LD2420; i comandi
> console e le procedure di calibrazione/verifica (§3–§7) si riferiscono al **protocollo
> LD2410B implementato dal firmware**, che non dialoga ancora con il modulo — decisione di
> adattamento in [domande aperte](wiki/domande-aperte.md).

Riferimenti: pagina hardware [HLK-LD2410B](wiki/hardware/hlk-ld2410b.md), pagina componente
[radar_ld2410](wiki/componenti/radar_ld2410.md), errata del protocollo v1.07
[ld2410b-protocollo-errata](wiki/insidie/ld2410b-protocollo-errata.md).

> **Nessun numero di questa procedura è misurato**: hardware non ancora collegato. Ciò che
> proviene dal datasheet/protocollo Hi-Link v1.07 è marcato **assunto** e va confermato a banco.

## 1. Posizionamento

- Il modulo va **dietro la zucca, antenna verso l'esterno**, puntato verso la zona di avvicinamento.
- **Lontano dall'induttore del boost** e dalle linee dati dei LED: il radar a 24 GHz vede
  alimentatori commutati e cavi in movimento come possibili bersagli (assunto, da verificare:
  test EMI al §7).
- **Niente metallo, acqua o foglio schermante davanti all'antenna**: bloccano o riflettono il
  fascio (assunto).
- Il radar **attraversa plastica e pareti sottili**: se la zucca ha una parete sottile dietro,
  il modulo può vedere le persone nell'ambiente adiacente. Difesa: **limitare il cancello
  massimo** (`radar limits <mov> <st> <durata_s>`, persistente nel modulo) in modo che la
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
- **Il firmware attuale parla protocollo LD2410B e non dialoga ancora con il LD2420**:
  le sezioni 3–7 sotto sono le procedure del layer protocollo LD2410B; la §8 misura il consumo
  reale del modulo a 3,3 V. Decisione di adattamento: [domande aperte](wiki/domande-aperte.md).

## 3. Comandi console

App di test `test_apps/radar_ld2410`: `idf.py -C test_apps/radar_ld2410 flash monitor`.

| Comando | Effetto |
| ------- | ------- |
| `radar status` | istantanea: presenza, distanza, energia, guasto, contatori |
| `radar version` | versione firmware del modulo (0x00A0) |
| `radar params` | parametri correnti del modulo (0x0061): cancelli, soglie, durata |
| `radar resolution` | risoluzione del cancello letta dal modulo (0x00AB) |
| `radar trigger <mm>` | distanza di trigger (persistita in NVS namespace `radar`) |
| `radar sensitivity <gate\|all> <motion> <static>` | soglie per cancello (0x0064, persistente) |
| `radar limits <mov> <st> <durata_s>` | cancelli massimi e durata no-one (0x0060, persistente) |
| `radar engineering <on\|off>` | modalità engineering: energia per cancello (NON persistente) |
| `radar restart` | riavvio del modulo (0x00A3) |
| `radar factoryreset confirm` | reset di fabbrica (0x00A2, effettivo dopo riavvio) |
| `radar calibrate [secondi]` | campiona la stanza vuota e propone le soglie (default 60 s) |
| `radar calibrate show` | mostra la proposta di calibrazione |
| `radar calibrate apply` | scrive le soglie proposte sul modulo (0x0064) e le persiste in NVS |
| `radar csv <on\|off>` | log CSV dei frame sulla console per l'analisi a banco |

Comandi `led_effects` già esistenti usati qui sotto: `fx`, `power`, `bright`
([elenco completo](wiki/componenti/led_effects.md)).

## 4. Sequenza di calibrazione delle soglie

1. **Stanza vuota**: nessuna persona e nessun oggetto in movimento nel raggio (ventole spente,
   tende ferme).
2. `radar engineering on`, poi `radar calibrate 60` (durata in secondi, default 60, range
   10–600): il componente campiona l'energia per cancello e calcola una proposta.
3. `radar calibrate show`: proposta = massimo rumore osservato per cancello + margine
   (`CONFIG_RADAR_CAL_MARGIN`, default 10). **Niente viene scritto sul modulo finché non c'è apply.**
4. **Controllo delle soglie proposte**: confrontarle con i default di fabbrica (motion
   50/50/40/30/20/15/15/15/15 per i cancelli 0–8, static 40/40/30/30/20/20/20/20/20 per i
   cancelli 2–8 — assunto, Table 7) e con il valore seen: una soglia a 100 disabilita il cancello.
5. `radar calibrate apply` **solo dopo conferma visiva**: scrive la sensibilità per cancello
   (0x0064) e la persiste in NVS namespace `radar` così ai boot successivi confronta invece di
   riscrivere.

## 5. Verifica distanze a 1/2/3 m

- Posare marcatori a 1, 2 e 3 m sull'asse di punta; far camminare una persona lungo l'asse.
- Leggere la distanza con `radar status` (valore in mm) ad ogni passaggio e annotare.
- La distanza è **quantizzata al cancello**: con la risoluzione di fabbrica 0,75 m due punti
  della stessa cella danno lo stesso valore; l'errore atteso è fino a **±mezzo cancello**
  (±375 mm) per costruzione, non è un difetto. Con `radar resolution` si conferma l'indice
  attivo; la risoluzione 0,2 m (persistente, effettiva dopo riavvio) dimezza l'errore se serve.
- Criterio: la distanza letta deve cadere nel **cancello corretto** (1 m → cancello 1–2,
  2 m → cancello 2–3, 3 m → cancello 4, con risoluzione 0,75 m — assunto).

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
- [ ] Distanze 1/2/3 m corrette in cancelli (§5).
- [ ] 30 min stanza vuota senza falsi positivi, di cui 10 min a rainbow 100% (§6).
- [ ] Consumo 3,3 V misurato (§8).
- [ ] Hot-plug UART con recovery senza reboot (§7).
