# halloween_pumpkin

Firmware per zucca di Halloween basato su **ESP32-S3 DevKitC-1** con 32 LED WS2812B, sensore ToF VL53L1X e integrazione MQTT/Home Assistant.

## Prerequisiti

- Ubuntu 24.04 (versione su cui è stato testato) con i pacchetti di build dell'ESP-IDF:

  ```bash
  sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
  ```

## Setup (una tantum)

Toolchain **ESP-IDF v5.4.4** installata nativamente in `~/esp/esp-idf`, tool in `~/.espressif` (fuori dal repo):

```bash
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 --shallow-submodules --recursive -b v5.4.4 https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf && ./install.sh esp32s3
```

## Caricare l'ambiente (ogni nuova shell)

```bash
source ~/esp/esp-idf/export.sh
```

## Build / Flash / Monitor

```bash
idf.py set-target esp32s3          # solo la prima volta (rigenera sdkconfig dai defaults)
idf.py build                       # compila
idf.py -p /dev/ttyACM0 flash monitor
```

- Il DevKitC-1 espone una porta USB-CDC, in genere `/dev/ttyACM0`; per trovarla: `ls /dev/ttyACM* /dev/ttyUSB*`.
- Per uscire dal monitor: `Ctrl+]`.

## Partition table (flash 8 MB, OTA a due slot)

`partitions.csv`: `nvs` 24K, `phy_init` 4K, `otadata` 8K, `ota_0` 3M, `ota_1` 3M (con ~1,9 MB di riserva in coda alla flash).

Verifica dopo la build:

```bash
idf.py partition_table
```

## VS Code

- Estensione consigliata: `ms-vscode.esp-idf-extension` (vedi `.vscode/extensions.json`).
- Alla prima apertura eseguire il comando **"ESP-IDF: Configure ESP-IDF Extension"**: i path di IDF e dei tool sono già impostati in `.vscode/settings.json`.

## Roadmap fasi

- Fase 0 (completata): ambiente ESP-IDF v5.4.4 e skeleton del repo.
- Fasi successive (una alla volta, ognuna con relativa pianificazione): `led_effects` (WS2812B), `tof_sensor` (VL53L1X), `pumpkin_fsm`, `power_mgr`, `ha_mqtt` + `config_store`, `wifi_prov`.
