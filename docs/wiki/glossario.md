# Glossario

- **ADR** (Architecture Decision Record): nota breve di una decisione architetturale — contesto, scelta, alternative scartate, conseguenze. In `decisioni/`.
- **RMT** (Remote Control): periferica dell'ESP32 usata dal driver `led_strip` per generare il timing preciso della linea dati WS2812B.
- **ToF** (Time of Flight): misura della distanza dal tempo di volo della luce riflessa; il VL53L1X è un sensore ToF.
- **OTA** (Over-The-Air): aggiornamento firmware via rete; la partition table prevede due slot `ota_0`/`ota_1`.
- **LDO** (Low-Dropout regulator): regolatore lineare a bassa caduta; alimenterà l'ESP32 a 3,3 V direttamente dalla batteria.
- **WS2812B**: LED RGB indirizzabile con driver integrato e protocollo monofilo; nel progetto, striscia da 32 pixel.
- **GRB**: ordine di trasmissione dei canali verde-rosso-blu richiesto dalla WS2812B (default `CONFIG_LED_FX_ORDER_GRB`).
- **DevKitC-1**: board di sviluppo ufficiale Espressif per l'ESP32-S3, con porta USB-CDC.
- **ESP-IDF**: framework ufficiale Espressif per la famiglia ESP32; nel progetto, versione v5.4.4 LTS.
- **MQTT**: protocollo publish/subscribe leggero usato per il trigger e l'integrazione con Home Assistant.
- **Home Assistant**: hub domotico che scoprirà la zucca via MQTT discovery device-based.
- **18650**: cella Litio-ione cilindrica; due in parallelo sono la sorgente primaria prevista.
- **MOSFET low-side**: interruttore con il MOSFET nel ramo di massa (Drain verso il carico, Source a GND); pilotabile pienamente da un GPIO a 3,3 V.
