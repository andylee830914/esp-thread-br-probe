# ESP Thread Border Router Probe

`esp-thread-br-probe` is an ESP32-based Thread Border Router with built-in Thread network diagnostics. It combines a Matter-managed Thread Border Router, the [`esp-thread-probe`](https://github.com/andylee830914/esp-thread-probe) telemetry API, Espressif's Border Router Web UI, and a fake Matter On/Off Light accessory in one firmware image.

The project is based on Espressif's `esp-matter/examples/thread_border_router` and builds as a standalone ESP-IDF project through the ESP-IDF Component Manager. It does not require `ESP_MATTER_PATH`.

## Features

- OpenThread Border Router running on an ESP32-S3 host with an ESP32-H2 RCP
- W5500 SPI Ethernet backbone with DHCP
- Matter BLE commissioning and Thread network provisioning
- Matter Thread Border Router Management cluster
- Fake Matter-over-Thread On/Off Light accessory for commissioning and control tests
- [`esp-thread-probe`](https://github.com/andylee830914/esp-thread-probe) topology and telemetry REST API
- Espressif's official Thread Border Router Web UI and REST API
- Automatic ESP32-H2 RCP update from the ESP32-S3 firmware image
- Serial logging for Matter commissioning, Thread datasets, roles, addresses, and lifecycle events

## Hardware

The target is the [ESP Thread Border Router board](https://github.com/espressif/esp-thread-br#esp-thread-border-router-board), containing:

- ESP32-S3 host
- ESP32-H2 OpenThread Radio Co-Processor (RCP)
- W5500 SPI Ethernet daughter board used as the Thread backbone interface

The current W5500 pin configuration is defined in `main/app_main.cpp`.

## Build and flash

Activate ESP-IDF, then prepare the ESP32-H2 RCP image:

```sh
source /path/to/esp-idf/export.sh
./tools/prepare_one_usb_flash.sh
```

Configure and build the ESP32-S3 firmware:

```sh
idf.py set-target esp32s3
idf.py build
```

Flash and monitor through the ESP32-S3 USB port:

```sh
idf.py -p <S3_USB_PORT> erase-flash flash monitor
```

The build embeds the ESP32-H2 RCP image in the `rcp_fw` partition. On boot, the ESP32-S3 checks and updates the H2 over the board's internal wiring, so the normal workflow requires only the S3 USB connection.

To recover or debug the RCP independently, build and flash ESP-IDF's `examples/openthread/ot_rcp` directly to the ESP32-H2.

## ESP Matter patches

On the first build, the Component Manager downloads dependencies into `managed_components/`. Apply the operational-advertising patch after that directory exists:

```sh
patch -p0 < patches/esp-matter-defer-operational-advertising.patch
```

This patch is required for reliable commissioning with the current managed ESP Matter version. AddNOC and the `kOperationalNetworkEnabled` event can occur before the DNS-SD advertiser is ready. The patch treats `CHIP_ERROR_INCORRECT_STATE` as a temporary condition and retries operational advertisement for up to five seconds.

If `patch` reports that the patch is reversed or was previously applied, do not apply it again.

The repository also contains `esp-matter-w5500-ethernet-dnssd.patch`, but it is **not required by the current configuration**. Matter operational traffic uses Thread, while W5500 provides the Border Router backbone and HTTP management interface. Keep that patch only as an optional compatibility patch if Matter Ethernet telemetry and operational DNS-SD are later enabled on W5500. See `patches/README.md` for details.

## Matter commissioning and fake accessory

When the device has no Matter fabric, it advertises over BLE. The serial console prints:

```text
Setup PIN: ...
Manual pairing code: ...
QR payload: MT:...
```

Use the QR payload with a compatible Matter controller. During commissioning, the controller can provision the Thread operational dataset through the Thread Border Router Management cluster.

The same Matter node exposes a fake On/Off Light endpoint. It provides a simple accessory target for validating Matter-over-Thread commissioning, discovery, CASE sessions, and On/Off commands without additional end-device hardware.

Run `idf.py menuconfig` and open **Matter Accessory Identity** to configure the vendor name, product/model name, and default node name. Keep the test VID/PID aligned with the included development attestation credentials; production IDs require matching certificates.

## Network startup

After Ethernet link-up and DHCP, the device initializes the W5500 management interface. The Thread Border Router backbone starts when the Thread dataset is provisioned and the OpenThread instance is ready.

Typical serial messages include:

```text
Ethernet link connected
Ethernet got DHCP IPv4: <ip>, gateway: <gateway>, netmask: <netmask>
Starting OpenThread Border Router on ETH_DEF
Matter commissioning complete
```

## Border Router Web UI

The firmware includes Espressif's `esp_ot_br_server`. After the W5500 obtains an address, open:

```text
http://<W5500_IP>/
```

Useful endpoints include:

- `GET /node`
- `GET /diagnostics`
- `GET /node/dataset/active`
- `GET /get_properties`
- `GET /topology`

The Web UI is stored in the `web_storage` SPIFFS partition and is included by `idf.py flash`.

## Probe API

The [`esp-thread-probe`](https://github.com/andylee830914/esp-thread-probe)-compatible API reads directly from the Border Router's OpenThread instance and listens on port `8080`:

```text
GET http://<W5500_IP>:8080/health
GET http://<W5500_IP>:8080/info
GET http://<W5500_IP>:8080/mesh
GET http://<W5500_IP>:8080/neighbors
GET http://<W5500_IP>:8080/routers
GET http://<W5500_IP>:8080/children
GET http://<W5500_IP>:8080/topology
GET http://<W5500_IP>:8080/router-neighbors
GET http://<W5500_IP>:8080/router-neighbors/scan
GET http://<W5500_IP>:8080/router
GET http://<W5500_IP>:8080/ipaddr
GET http://<W5500_IP>:8080/leader
GET http://<W5500_IP>:8080/dataset
GET http://<W5500_IP>:8080/uplink
GET http://<W5500_IP>:8080/matter/qr-code
```

`/uplink` reports `transport: direct-http` because this integrated firmware does not use the probe project's companion WROOM/UART proxy.

`/matter/qr-code` returns the Matter QR payload, manual pairing code, and setup PIN. These are commissioning secrets; expose the API only on a trusted network.

## Main dependencies

- ESP-IDF
- `espressif/esp_matter`
- `espressif/esp_rcp_update`
- `espressif/w5500`
- Espressif `esp_ot_br_server`
- [`esp-thread-probe`](https://github.com/andylee830914/esp-thread-probe) `probe_core`

Dependency versions and sources are declared in `main/idf_component.yml`.
