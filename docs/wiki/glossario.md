# Glossario

- **ADR** (Architecture Decision Record): nota breve di una decisione architetturale — contesto, scelta, alternative scartate, conseguenze. In `decisioni/`.
- **RMT** (Remote Control): periferica dell'ESP32 usata dal driver `led_strip` per generare il timing preciso della linea dati WS2812B.
- **ToF** (Time of Flight): misura della distanza dal tempo di volo della luce riflessa; il VL53L1X è un sensore ToF.
- **OTA** (Over-The-Air): aggiornamento firmware via rete; la partition table prevede due slot `ota_0`/`ota_1`.
- **LDO** (Low-Dropout regulator): regolatore lineare a bassa caduta; alimenterà l'ESP32 a 3,3 V direttamente dalla batteria.
- **WS2812B**: LED RGB indirizzabile con driver integrato e protocollo monofilo; nel progetto, matrice 8×8 da 64 pixel in un'unica catena DIN.
- **Serpentina**: cablaggio tipico delle matrici LED in cui la catena DIN percorre una riga e prosegue in senso inverso su quella successiva; qui origine alto-sinistra e indice `y*8 + (y pari ? x : 7-x)` (assunto da verificare a banco).
- **Pixel test**: accensione di un solo pixel per la verifica di banco della mappatura (`led_fx_pixel_test`, comando console `pixel`).
- **GRB**: ordine di trasmissione dei canali verde-rosso-blu richiesto dalla WS2812B (default `CONFIG_LED_FX_ORDER_GRB`).
- **ESP32 D1 mini**: board clonica compatta per l'ESP32 classico; l'unità al banco usa il bridge USB-UART **CP2102** e la porta `/dev/ttyUSB0` (console UART0 115200; altre unità montano un CH340).
- **ESP-IDF**: framework ufficiale Espressif per la famiglia ESP32; nel progetto, versione v5.4.4 LTS.
- **MQTT**: protocollo publish/subscribe leggero usato per il trigger e l'integrazione con Home Assistant.
- **Home Assistant**: hub domotico che scoprirà la zucca via MQTT discovery device-based.
- **18650**: cella Litio-ione cilindrica; due in parallelo sono la sorgente primaria prevista.
- **MOSFET low-side**: interruttore con il MOSFET nel ramo di massa (Drain verso il carico, Source a GND); pilotabile pienamente da un GPIO a 3,3 V.
