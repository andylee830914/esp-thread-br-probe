# Local Patches

These patches are intentionally kept outside `managed_components` because ESP-IDF
component manager may regenerate downloaded components.

## esp-matter-w5500-ethernet-dnssd.patch

Apply after dependencies are downloaded:

```sh
cd /Users/andylee/Work/thread_border_router_apple_home_test
patch -p0 < patches/esp-matter-w5500-ethernet-dnssd.patch
patch -p0 < patches/esp-matter-defer-operational-advertising.patch
```

This patch lets the test firmware use the ESP Matter Ethernet operational mDNS
path with a manually initialized W5500 backbone:

- keeps `ESP32DnssdImpl.cpp` when Wi-Fi is off but Ethernet telemetry is on
- excludes `NetworkCommissioningDriver_Ethernet.cpp` when Ethernet network
  commissioning is disabled, because that driver assumes internal ESP32 EMAC
  rather than SPI W5500

## esp-matter-defer-operational-advertising.patch

This patch avoids treating an early Matter operational DNS-SD advertise attempt
as a hard-looking commissioning failure while the DNS-SD advertiser is still
initializing. It:

- logs `CHIP_ERROR_INCORRECT_STATE` from the immediate AddNOC advertise attempt
  as deferred instead of failed
- retries the `kOperationalNetworkEnabled` advertise path until the advertiser
  is initialized, then logs `Operational advertising enabled`
