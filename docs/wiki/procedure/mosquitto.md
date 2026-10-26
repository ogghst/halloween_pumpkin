---
titolo: Server MQTT locale per i test (Mosquitto)
tipo: procedura
stato: verificato
aggiornato: 2026-10-26
fonti: [systemctl/ss/mosquitto -h e dpkg sul PC di sviluppo 2026-10-25, /etc/mosquitto/mosquitto.conf, contratto Fase 5 2026-10-26]
correlate: [../decisioni/adr-004-trigger-mqtt-contratto.md, ../decisioni/adr-005-discovery-device-based.md, ../glossario.md]
---

# Server MQTT locale per i test (Mosquitto)

Per provare i messaggi MQTT della zucca (Fase 5 `ha_mqtt`: [trigger via MQTT](../decisioni/adr-004-trigger-mqtt-contratto.md)
e [discovery device-based](../decisioni/adr-005-discovery-device-based.md)) il PC di sviluppo
ospita un broker **Mosquitto 2.0.18** installato dall'utente (pacchetto `mosquitto`
2.0.18-1build3). Fatti verificati il 2026-10-25:

- Servizio systemd `mosquitto` **attivo** e **abilitato al boot**.
- In ascolto su **127.0.0.1:1883 e [::1]:1883 — solo localhost** (verificato con `ss -ltn`):
  è il default di Mosquitto 2.0 senza listener esplicito.
- Config: `/etc/mosquitto/mosquitto.conf` quasi tutto di default (persistenza in
  `/var/lib/mosquitto/`, log su `/var/log/mosquitto/mosquitto.log`, include di
  `/etc/mosquitto/conf.d/` che è vuoto). Nessuna autenticazione configurata.

## Limite da ricordare: il broker accetta solo connessioni locali

Così com'è, **la board ESP32 non può connettersi**: il broker è raggiungibile solo dal PC.
In Fase 5, per i test con la zucca sulla stessa LAN, va aggiunto un listener esposto
(es. `/etc/mosquitto/conf.d/zucca.conf`):

```conf
listener 1883 0.0.0.0
allow_anonymous false   # richiesto da Mosquitto 2.0 quando si definisce un listener
password_file /etc/mosquitto/passwd   # credenziali create con mosquitto_passwd
```

e poi riavviato il servizio. **Queste modifiche richiedono sudo**: le esegue l'utente nel
proprio terminale (mai password in chat); le credenziali NON vanno scritte in wiki né nel
repo — in firmware finiranno in NVS/Kconfig di fase 5. Finché il listener non esiste, i
messaggi si provano solo **dal PC stesso** (broker e client sulla stessa macchina).

## Client CLI da installare (ancora mancanti)

`mosquitto_pub` e `mosquitto_sub` NON sono installati (solo il server): roundtrip MQTT non
ancora eseguibile. Installare il pacchetto `mosquitto-clients` (sudo, utente):

```bash
sudo apt install mosquitto-clients
```

## Test di roundtrip locale (dopo l'installazione dei client)

```bash
# terminale 1: ascolta il sottoalbero della zucca e la discovery
mosquitto_sub -h 127.0.0.1 -t 'pumpkin/#' -t 'homeassistant/#' -v
# terminale 2: pubblica un messaggio di prova
mosquitto_pub -h 127.0.0.1 -t 'pumpkin/<id>/test' -m 'ciao dalla zucca'
```

L'argomento root `pumpkin/<id>/` è il prefisso dei messaggi del firmware
([contratto trigger](../decisioni/adr-004-trigger-mqtt-contratto.md)): `<id>` = 6 cifre
esadecimali minuscole dai byte `[3..5]` del MAC STA (es. `pumpkin/717e50/...`), coerente
con l'hostname `pumpkin-<id>` di `net_mgr`. Struttura completa dei topic in
[docs/mqtt.md](../../mqtt.md) (Fase 5).

> **Correzione (2026-10-26)**: questa pagina indicava il prefisso previsto `zucca/`;
> il contratto della Fase 5 lo definisce `pumpkin/<id>/`. Comandi e testo sopra corretti.

## Diagnostica

- Log del broker: `/var/log/mosquitto/mosquitto.log` (lettura con privilegi).
- Stato servizio: `systemctl status mosquitto`.
- Porta in ascolto: `ss -ltn | grep 1883`.
