# Local Patches

These patches are intentionally kept outside `managed_components` because ESP-IDF
component manager may regenerate downloaded components.

## Current requirement

After dependencies are downloaded, the current project requires only:

```sh
patch -p0 < patches/esp-matter-defer-operational-advertising.patch
```

If `patch` reports that it was previously applied, do not apply it again.

## esp-matter-defer-operational-advertising.patch

**Required with the currently pinned ESP Matter version.**

AddNOC and `kOperationalNetworkEnabled` may trigger operational DNS-SD before
the advertiser is initialized. Without a retry, Matter commissioning can add
the fabric but fail to discover the new operational node. This patch:

- treats `CHIP_ERROR_INCORRECT_STATE` as a temporary initialization race
- retries every 250 ms, up to 20 times (five seconds total)
- logs `Operational advertising enabled` after a successful retry

The application also restarts operational DNS-SD when the Border Router becomes
ready, but that happens through a separate lifecycle path and does not replace
the commissioning-event retry.

## esp-matter-w5500-ethernet-dnssd.patch

**Not required by the current configuration.**

The current project has `CONFIG_ENABLE_ETHERNET_TELEMETRY=n`. Matter operational
traffic and DNS-SD use Thread; W5500 is used for the Border Router backbone and
the HTTP management interfaces. With this setting, ESP Matter already excludes
the Ethernet-specific sources affected by this patch.

Keep the patch as an optional compatibility patch if the project later enables
Matter Ethernet telemetry and operational DNS-SD directly on W5500. It then:

- keeps `ESP32DnssdImpl.cpp` when Wi-Fi is off but Ethernet telemetry is on
- excludes `NetworkCommissioningDriver_Ethernet.cpp` when Ethernet network
  commissioning is disabled, because that driver assumes internal ESP32 EMAC
  rather than SPI W5500
