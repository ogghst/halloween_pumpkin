---
titolo: Sottocomandi console annidati: argv senza shift, quoting split_argv e comandi del pannello desktop
tipo: insidia
stato: verificato
aggiornato: 2026-10-27
fonti: [banco 2026-10-27 (ESP32 D1 mini, console `/dev/ttyUSB0` 115200): sequenza CA riprodotta riga per riga, commit NVS `CA salvata in NVS (1119 byte)`, post-flash `ha ca status: ca_tls=nvs ca_bytes=1119`; main/main.c `ha_ca_cmd`, ~/esp/esp-idf/components/console/split_argv.c (IDF v5.4.4), tools/desktop/pumpkin_desktop.py]
correlate: [comandi-ha-muti-e-coda-led.md, ../componenti/ha-mqtt.md, ../procedure/provisioning.md]
---

# Sottocomandi console annidati: argv senza shift, quoting split_argv, pannello desktop

Tre difetti indipendenti scoperti installando la CA TLS dal pannello desktop:
il primo nel tool, gli altri due nell'interazione console/firmware. Tutti
verificati a banco con la sequenza completa (invio PEM, commit NVS, status).

## 1. Il pannello mandava `ca begin`/`ca status` senza il prefisso `ha`

### Il sintomo

Il pulsante "Invia CA al dispositivo" del pannello desktop produceva
`Unrecognized command` (stringa di `esp_console_common.c:179` di IDF: il
**primo token** non corrisponde a nessun comando registrato).

### La causa

`ca` non è un comando: è un sottocomando di `ha`. In più le righe PEM venivano
inviate senza il prefisso `+` richiesto dal firmware.

### La regola nel codice

Il pannello (`tools/desktop/pumpkin_desktop.py`, worker `_send_ca`) ora invia:

```python
monitor.send_line("ha ca begin")
monitor.send_line(f'ha ca "+{line}"')   # una riga PEM per volta
monitor.send_line("ha ca status")
```

## 2. `split_argv` apre le virgolette solo a inizio token

### Il sintomo

Le righe `-----BEGIN CERTIFICATE-----` e `-----END CERTIFICATE-----`
contengono uno spazio: inviate come `ha ca +-----BEGIN CERTIFICATE-----`
l'argv diventa `["ha","ca","+-----BEGIN","CERTIFICATE-----"]` (argc 4) e il
ramo `+` (che richiede argc 3) risponde usage.

### La causa

`esp_console_split_argv` (IDF v5.4.4) riconosce la virgoletta `"` **solo nello
stato `SS_SPACE`**, cioè se il token *comincia* con `"`: una virgoletta a
metà token viene copiata letteralmente.

### La regola nel codice

L'argomento va quotato per intero, virgoletta prima del `+`:

```
ha ca "+-----BEGIN CERTIFICATE-----"
```

Le righe base64 (nessuno spazio) funzionano anche senza quoting, ma il
pannello quota sempre per uniformità.

## 3. `ha_ca_cmd`: dispatcher annidato che non shifta argv

### Il sintomo

Dopo le correzioni 1 e 2, `ha ca begin` e `ha ca status` rispondevano comunque
usage; solo `ha ca +<riga>` funzionava (le righe PEM erano accettate e il
commit su NVS avveniva).

### La causa

`cmd_ha` chiama `ha_ca_cmd(argc, argv)` **senza shiftare** argv: dentro
`ha_ca_cmd` vale `argv[0]="ha"`, `argv[1]="ca"`, `argv[2]=sottocomando`. I
rami `clear`/`begin`/`status` erano scritti come se fossero top-level
(`argc == 3 && strcmp(argv[1], "clear")` ecc.): `argv[1]` è sempre `"ca"`,
quindi mai veri. Solo il ramo `+` era indicizzato correttamente su `argv[2]`.

### La regola nel codice

Un dispatcher annidato che non shifta argv deve indicizzare i sottocomandi su
`argv[2]` con `argc == 3`:

```c
if (argc == 3 && strcmp(argv[2], "clear") == 0) { ... }
if (argc == 3 && strcmp(argv[2], "begin") == 0) { ... }
if (argc == 3 && strcmp(argv[2], "status") == 0) { ... }
if (argc == 3 && argv[2][0] == '+' && argv[2][1] != '\0') { ... }
```

## Verifica a banco (2026-10-27)

- Sequenza corretta via seriale: 19 righe PEM → 19 × `ha ca +: ok`,
  `I (…) ha_mqtt: CA salvata in NVS (1119 byte)`.
- Dopo build+flash del fix: `ha ca status: ca_tls=nvs ca_bytes=1119`
  (la CA sopravvive al flash: NVS non toccata), `ha ca begin` stampa le
  istruzioni con esempi quottati, `ha ca` nudo risponde usage.
- Build con zero warning.
