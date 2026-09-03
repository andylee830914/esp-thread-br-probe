# Thread Border Router

This example creates a Matter Thread Border Router device using the ESP Matter data model.


This standalone copy uses the ESP-IDF Component Manager to download `espressif/esp_matter`, `espressif/esp_rcp_update`, and related dependencies during build. It does not require `ESP_MATTER_PATH`.

See the [docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html) for more information about building and flashing the firmware.

## 1. Additional Environment Setup

### 1.1 Hardware Platform

The [ESP Thread Border Router board](https://github.com/espressif/esp-thread-br?tab=readme-ov-file#esp-thread-border-router-board) which provides an integrated module of an ESP32-S3 and an ESP32-H2 is required for this example.

### 1.2 Firmware for RCP

The [OpenThread RCP](https://github.com/espressif/esp-idf/tree/master/examples/openthread/ot_rcp) should be available for the ESP32-H2 side of the Border Router board.

Use the one-USB workflow first. It builds the ESP-IDF `ot_rcp` example for ESP32-H2. During the later ESP32-S3 build, the managed `esp_rcp_update` component packs that H2 build output into the S3 `rcp_fw` partition:

```
$ cd /Users/andylee/Work/thread_border_router_apple_home_test
$ source "/Users/andylee/.espressif/tools/activate_idf_v6.0.2.sh"
$ ./tools/prepare_one_usb_flash.sh
$ idf.py set-target esp32s3
$ idf.py build
$ idf.py -p <S3_USB_PORT> erase-flash flash monitor
```

On boot, the S3 updates the H2 RCP through the board wiring.

You can also flash the H2 directly as a recovery/debug path:


```
$ cd /path/to/esp-idf/examples/openthread/ot_rcp
$ idf.py set-target esp32h2 build
$ idf.py -p <port> erase-flash flash
```

Or you can flash the firmware of ESP32-H2 with [esp_rcp_update](https://github.com/espressif/esp-thread-br/tree/main/components/esp_rcp_update) after enabling `AUTO_UPDATE_RCP` in menuconfig:

```
$ cd /path/to/esp-idf/examples/openthread/ot_rcp
$ idf.py set-target esp32h2 build
```

After flashing the Thread Border Router firmware to ESP32-S3, it will flash the RCP firmware to ESP32-H2 automatically.

### 1.3 Firmware for Host SoC

The default setting flash size is 8MB, set target and build as below:

```
$ idf.py set-target esp32s3
$ idf.py build
```

On the first build, ESP-IDF will create `managed_components/` and download the Matter dependencies declared in `main/idf_component.yml`.

After `managed_components/` exists, apply the local ESP Matter W5500 Ethernet/mDNS patch before building:

```
$ patch -p0 < patches/esp-matter-w5500-ethernet-dnssd.patch
```

This patch is required for the Apple Home test build when Wi-Fi is disabled and the W5500 Ethernet daughter board is used as the Matter IP backbone. It keeps the ESP32 platform DNSSD backend enabled for Ethernet operational advertising, while excluding the ESP Matter Ethernet network commissioning driver that assumes internal ESP32 EMAC instead of SPI W5500.

If the patch was already applied, `patch` may report `Reversed (or previously applied) patch detected`; do not apply it again.

If a local `sdkconfig` already exists from an older run, confirm `CONFIG_ENABLE_OTA_REQUESTOR=n`. The Apple Home commissioning test does not need the OTA requestor, and keeping it disabled avoids an unnecessary dependency edge in the managed-component build.

## 2. Apple Home Commissioning Test

This test build is intended to check whether Apple Home will commission the ESP Thread Border Router over standard Matter BLE commissioning and then provision it through the Matter Thread Border Router Management cluster.

Flash and monitor the ESP32-S3:

```
$ idf.py -p <S3_USB_PORT> erase-flash flash monitor
```

After boot, use the serial output to get the Matter setup information:

```text
Setup PIN: ...
Manual pairing code: ...
QR payload: MT:...
```

Generate or open a QR code from the printed `MT:` payload, then scan it in Apple Home.

Watch the serial monitor for:

```text
ThreadBorderRouterManagement.SetActiveDatasetRequest received
```

If that log appears, Apple Home attempted to send the active Thread dataset to the ESP Border Router. If commissioning completes but that log never appears, Apple Home accepted the Matter node but did not provision the Border Router dataset.

## 3. Official ESP Border Router Web UI and REST API

The firmware includes Espressif's official `esp_ot_br_server` from the stable `esp-thread-br` v1.3 release. After W5500 obtains a DHCP address, open:

```text
http://<W5500_IP>/
```

Useful REST endpoints include `GET /node`, `GET /diagnostics`, `GET /node/dataset/active`, `GET /get_properties`, and `GET /topology`.

The Web UI files are built into the `web_storage` SPIFFS partition and are flashed automatically by `idf.py flash`.

## 4. esp-thread-probe API

The `esp-thread-probe` telemetry API runs directly against the Border Router's OpenThread instance on port `8080`. Keeping it on a separate port avoids collisions with the official server's `/topology` and `/ipaddr` handlers while preserving every original probe path:

```text
http://<W5500_IP>:8080/health
http://<W5500_IP>:8080/info
http://<W5500_IP>:8080/mesh
http://<W5500_IP>:8080/neighbors
http://<W5500_IP>:8080/routers
http://<W5500_IP>:8080/children
http://<W5500_IP>:8080/topology
http://<W5500_IP>:8080/router-neighbors
http://<W5500_IP>:8080/router-neighbors/scan
http://<W5500_IP>:8080/router
http://<W5500_IP>:8080/ipaddr
http://<W5500_IP>:8080/leader
http://<W5500_IP>:8080/dataset
http://<W5500_IP>:8080/uplink
```

`/uplink` remains available for compatibility, but reports `transport: direct-http` because this integrated build does not use the probe's companion WROOM/UART proxy.
