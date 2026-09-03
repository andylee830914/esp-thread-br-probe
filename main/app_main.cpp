/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <esp_openthread_border_router.h>
#include <esp_br_web.h>
#include <esp_spiffs.h>
#include <esp_eth.h>
#include <esp_eth_mac_w5500.h>
#include <esp_eth_netif_glue.h>
#include <esp_eth_phy_w5500.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>

#include <esp_matter.h>
#include <esp_matter_cluster.h>
#include <esp_matter_console.h>
#include <esp_matter_endpoint.h>
#include <esp_matter_feature.h>

#include <esp_ot_config.h>
#include <probe_http.h>
#include <probe_thread.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && defined(CONFIG_OPENTHREAD_BORDER_ROUTER) && defined(CONFIG_AUTO_UPDATE_RCP)
#include <esp_ot_rcp_update.h>
#include <esp_rcp_update.h>
#endif

#include <platform/ESP32/OpenthreadLauncher.h>
#include <platform/OpenThread/GenericThreadBorderRouterDelegate.h>
#include <platform/ThreadStackManager.h>
#include <app/server/Dnssd.h>
#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <lib/core/CHIPError.h>
#include <lib/core/ErrorStr.h>
#include <platform/KvsPersistentStorageDelegate.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

#include <inttypes.h>
#include <atomic>
#include <string.h>
#include <openthread/dataset.h>
#include <openthread/error.h>
#include <openthread/ip6.h>
#include <openthread/srp_client.h>
#include <openthread/thread.h>

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
using chip::app::Clusters::ThreadBorderRouterManagement::GenericOpenThreadBorderRouterDelegate;

namespace {

const chip::RendezvousInformationFlags kAppleHomeRendezvousFlags(chip::RendezvousInformationFlag::kBLE);

constexpr spi_host_device_t kW5500SpiHost = SPI2_HOST;
constexpr int kW5500SclkGpio = 21;
constexpr int kW5500MosiGpio = 45;
constexpr int kW5500MisoGpio = 38;
constexpr int kW5500CsGpio = 41;
constexpr int kW5500IntGpio = 39;
constexpr int kW5500ResetGpio = 40;
constexpr int kW5500PhyAddr = 1;
constexpr int kW5500SpiClockMhz = 36;

esp_netif_t *s_ethernet_netif = nullptr;
esp_eth_handle_t s_ethernet_handle = nullptr;
esp_eth_netif_glue_handle_t s_ethernet_glue = nullptr;
bool s_thread_br_initialized = false;
std::atomic_bool s_w5500_start_requested{false};
bool s_ethernet_has_ip = false;
bool s_operational_mdns_restart_scheduled = false;

esp_err_t start_w5500_ethernet();
esp_err_t request_w5500_ethernet_start(const char *reason);
void init_thread_br_backbone(const char *if_key);
void restart_operational_mdns(const char *reason);

const char *connectivity_change_to_str(chip::DeviceLayer::ConnectivityChange change)
{
    switch (change) {
    case chip::DeviceLayer::kConnectivity_Established:
        return "established";
    case chip::DeviceLayer::kConnectivity_Lost:
        return "lost";
    case chip::DeviceLayer::kConnectivity_NoChange:
        return "no-change";
    default:
        return "unknown";
    }
}

const char *activity_change_to_str(chip::DeviceLayer::ActivityChange change)
{
    switch (change) {
    case chip::DeviceLayer::kActivity_Started:
        return "started";
    case chip::DeviceLayer::kActivity_Stopped:
        return "stopped";
    case chip::DeviceLayer::kActivity_NoChange:
        return "no-change";
    default:
        return "unknown";
    }
}

const char *thread_role_to_str(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED:
        return "disabled";
    case OT_DEVICE_ROLE_DETACHED:
        return "detached";
    case OT_DEVICE_ROLE_CHILD:
        return "child";
    case OT_DEVICE_ROLE_ROUTER:
        return "router";
    case OT_DEVICE_ROLE_LEADER:
        return "leader";
    default:
        return "unknown";
    }
}

const char *current_thread_role_to_str()
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    otInstance *ot_inst = chip::DeviceLayer::ThreadStackMgrImpl().OTInstance();
    if (!ot_inst) {
        return "ot-not-ready";
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otDeviceRole role = otThreadGetDeviceRole(ot_inst);
    esp_openthread_lock_release();
    return thread_role_to_str(role);
#else
    return "thread-disabled";
#endif
}

otDeviceRole current_thread_role()
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    otInstance *ot_inst = chip::DeviceLayer::ThreadStackMgrImpl().OTInstance();
    if (!ot_inst) {
        return OT_DEVICE_ROLE_DISABLED;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otDeviceRole role = otThreadGetDeviceRole(ot_inst);
    esp_openthread_lock_release();
    return role;
#else
    return OT_DEVICE_ROLE_DISABLED;
#endif
}

void log_byte_span(const char *label, chip::ByteSpan span)
{
    ESP_LOGI(TAG, "%s length=%u", label, static_cast<unsigned>(span.size()));
    if (!span.empty()) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, span.data(), span.size(), ESP_LOG_INFO);
    }
}

void log_dataset_summary(const char *label, const chip::Thread::OperationalDataset &dataset)
{
    uint64_t active_timestamp = 0;
    uint16_t pan_id = 0;
    char network_name[chip::Thread::kSizeNetworkName + 1] = {};

    ESP_LOGI(TAG, "%s", label);
    if (dataset.GetActiveTimestamp(active_timestamp) == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "  active timestamp: %" PRIu64, active_timestamp);
    } else {
        ESP_LOGI(TAG, "  active timestamp: <not present>");
    }
    if (dataset.GetPanId(pan_id) == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "  PAN ID: 0x%04" PRIx16, pan_id);
    } else {
        ESP_LOGI(TAG, "  PAN ID: <not present>");
    }
    if (dataset.GetNetworkName(network_name) == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "  network name: %s", network_name);
    } else {
        ESP_LOGI(TAG, "  network name: <not present>");
    }
    log_byte_span("  dataset TLVs", dataset.AsByteSpan());
}

bool log_active_dataset_from_ot(const char *reason)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    otInstance *esp_ot_inst = esp_openthread_get_instance();
    otInstance *matter_ot_inst = chip::DeviceLayer::ThreadStackMgrImpl().OTInstance();
    ESP_LOGI(TAG, "%s: esp_openthread_get_instance=%p ThreadStackMgrImpl().OTInstance=%p",
             reason, esp_ot_inst, matter_ot_inst);
    if (!esp_ot_inst) {
        ESP_LOGW(TAG, "%s: OpenThread instance is not ready", reason);
        return false;
    }

    otOperationalDatasetTlvs dataset_tlvs = {};
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError ot_err = otDatasetGetActiveTlvs(esp_ot_inst, &dataset_tlvs);
    esp_openthread_lock_release();

    if (ot_err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "%s: otDatasetGetActiveTlvs failed: %s", reason, otThreadErrorToString(ot_err));
        return false;
    }

    ESP_LOGI(TAG, "%s: active dataset TLVs length=%u", reason, dataset_tlvs.mLength);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, dataset_tlvs.mTlvs, dataset_tlvs.mLength, ESP_LOG_INFO);
    return dataset_tlvs.mLength > 0;
#else
    ESP_LOGW(TAG, "%s: Thread is disabled", reason);
    return false;
#endif
}

void log_thread_addresses_and_srp(const char *reason)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    otInstance *ot_inst = esp_openthread_get_instance();
    if (!ot_inst) {
        ESP_LOGW(TAG, "%s: cannot log Thread IPv6/SRP; OpenThread instance is not ready", reason);
        return;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    unsigned address_index = 0;
    for (const otNetifAddress *addr = otIp6GetUnicastAddresses(ot_inst); addr; addr = addr->mNext) {
        char addr_str[OT_IP6_ADDRESS_STRING_SIZE] = {};
        otIp6AddressToString(&addr->mAddress, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG,
                 "%s: Thread IPv6[%u]=%s/%u origin=%u preferred=%d valid=%d rloc=%d meshlocal=%d srp=%d",
                 reason, address_index++, addr_str, addr->mPrefixLength, addr->mAddressOrigin,
                 addr->mPreferred, addr->mValid, addr->mRloc, addr->mMeshLocal, addr->mSrpRegistered);
    }
    if (address_index == 0) {
        ESP_LOGW(TAG, "%s: Thread has no unicast IPv6 addresses yet", reason);
    }

    const otSockAddr *srp_server = otSrpClientGetServerAddress(ot_inst);
    char srp_server_addr[OT_IP6_ADDRESS_STRING_SIZE] = {};
    otIp6AddressToString(&srp_server->mAddress, srp_server_addr, sizeof(srp_server_addr));

    const otSrpClientHostInfo *host = otSrpClientGetHostInfo(ot_inst);
    ESP_LOGI(TAG, "%s: SRP running=%d server=[%s]:%u host=%s host_state=%s auto_addr=%d host_addr_count=%u",
             reason, otSrpClientIsRunning(ot_inst), srp_server_addr, srp_server->mPort,
             host && host->mName ? host->mName : "<unset>",
             host ? otSrpClientItemStateToString(host->mState) : "<none>",
             host ? host->mAutoAddress : 0,
             host ? static_cast<unsigned>(host->mNumAddresses) : 0);

    const otSrpClientService *service = otSrpClientGetServices(ot_inst);
    unsigned service_index = 0;
    while (service) {
        ESP_LOGI(TAG, "%s: SRP service[%u] instance=%s name=%s port=%u state=%s",
                 reason, service_index++,
                 service->mInstanceName ? service->mInstanceName : "<unset>",
                 service->mName ? service->mName : "<unset>",
                 service->mPort,
                 otSrpClientItemStateToString(service->mState));
        service = service->mNext;
    }
    if (service_index == 0) {
        ESP_LOGI(TAG, "%s: SRP has no services queued yet", reason);
    }

    esp_openthread_lock_release();
#else
    ESP_LOGW(TAG, "%s: Thread is disabled", reason);
#endif
}

void restart_operational_mdns_work(intptr_t arg)
{
    const char *reason = reinterpret_cast<const char *>(arg);
    ESP_LOGI(TAG, "%s: restarting Matter operational DNS-SD advertising", reason);
    chip::app::DnssdServer::Instance().SetFabricTable(&chip::Server::GetInstance().GetFabricTable());
    chip::app::DnssdServer::Instance().StartServer(chip::Dnssd::CommissioningMode::kDisabled);
    s_operational_mdns_restart_scheduled = false;
}

void restart_operational_mdns(const char *reason)
{
    if (s_operational_mdns_restart_scheduled) {
        ESP_LOGI(TAG, "%s: Matter operational DNS-SD restart already scheduled", reason);
        return;
    }
    s_operational_mdns_restart_scheduled = true;
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(restart_operational_mdns_work,
                                                                   reinterpret_cast<intptr_t>(reason));
    if (err != CHIP_NO_ERROR) {
        s_operational_mdns_restart_scheduled = false;
        ESP_LOGE(TAG, "%s: failed to schedule Matter operational DNS-SD restart: %s", reason, chip::ErrorStr(err));
    }
}

void start_w5500_backbone_if_thread_ready(const char *reason)
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    otDeviceRole role = current_thread_role();
    bool has_dataset = log_active_dataset_from_ot(reason);
    bool attached = role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
    unsigned fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
    bool has_fabric = fabric_count > 0;
    ESP_LOGI(TAG, "%s: BR eligibility: fabric_count=%u dataset=%d thread_attached=%d thread_role=%s ethernet_ip=%d",
             reason, fabric_count, has_dataset, attached, thread_role_to_str(role), s_ethernet_has_ip);

    if (!has_fabric || !has_dataset || !attached) {
        ESP_LOGI(TAG, "%s: BR backbone deferred until a Matter fabric, Thread dataset, and Thread attachment exist",
                 reason);
        return;
    }

    log_thread_addresses_and_srp(reason);

    if (s_w5500_start_requested.load()) {
        ESP_LOGI(TAG, "%s: W5500 start already requested", reason);
        if (s_ethernet_has_ip) {
            init_thread_br_backbone("ETH_DEF");
        } else {
            ESP_LOGI(TAG, "%s: waiting for Ethernet DHCP before BR init", reason);
        }
        return;
    }
    request_w5500_ethernet_start(reason);
#endif
}

void print_apple_home_onboarding_payload()
{
    chip::PayloadContents payload;
    CHIP_ERROR err = GetPayloadContents(payload, kAppleHomeRendezvousFlags);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to create Matter setup payload: %s", chip::ErrorStr(err));
        return;
    }

    char qr_code_buffer[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1];
    chip::MutableCharSpan qr_code(qr_code_buffer);
    err = GetQRCode(qr_code, payload);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to create Matter QR payload: %s", chip::ErrorStr(err));
        return;
    }

    char manual_code_buffer[chip::kManualSetupLongCodeCharLength + 2];
    chip::MutableCharSpan manual_code(manual_code_buffer);
    err = GetManualPairingCode(manual_code, payload);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to create Matter manual pairing code: %s", chip::ErrorStr(err));
        return;
    }

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "Apple Home Matter BLE commissioning test payload");
    ESP_LOGI(TAG, "Setup PIN: %08" PRIu32, payload.setUpPINCode);
    ESP_LOGI(TAG, "Manual pairing code: %s", manual_code.data());
    ESP_LOGI(TAG, "QR payload: %s", qr_code.data());
    ESP_LOGI(TAG, "Scan the QR payload as a standard Matter QR code in Apple Home.");
    probe_http_set_matter_onboarding(qr_code.data(), manual_code.data(), payload.setUpPINCode);
    PrintQrCodeURL(qr_code);
    ESP_LOGI(TAG, "==================================================");
}

void init_thread_br_backbone(const char *if_key)
{
#if CONFIG_OPENTHREAD_BORDER_ROUTER
    if (s_thread_br_initialized) {
        ESP_LOGI(TAG, "Thread Border Router backbone already initialized");
        return;
    }

    esp_netif_t *backbone_netif = esp_netif_get_handle_from_ifkey(if_key);
    if (!backbone_netif) {
        ESP_LOGE(TAG, "Backbone netif not found: %s", if_key);
        return;
    }

    ESP_LOGI(TAG, "Starting OpenThread Border Router on %s; backbone_netif=%p", if_key, backbone_netif);
    esp_openthread_set_backbone_netif(backbone_netif);
    ESP_LOGI(TAG, "Configured OpenThread backbone netif=%p", esp_openthread_get_backbone_netif());
    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_openthread_border_router_init();
    esp_openthread_lock_release();
    ESP_LOGI(TAG, "esp_openthread_border_router_init() -> %s", esp_err_to_name(err));
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_thread_br_initialized = true;
        ESP_LOGI(TAG, "BR initialized; backbone_netif=%p", esp_openthread_get_backbone_netif());
        restart_operational_mdns("Thread Border Router backbone ready");
    } else {
        ESP_LOGE(TAG, "Thread Border Router backbone init failed: %s", esp_err_to_name(err));
    }
#endif
}

void on_ethernet_event(void *, esp_event_base_t, int32_t event_id, void *)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link connected");
        if (s_ethernet_netif) {
            ESP_ERROR_CHECK(esp_netif_create_ip6_linklocal(s_ethernet_netif));
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link disconnected");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

void on_ethernet_got_ip(void *, esp_event_base_t, int32_t, void *event_data)
{
    const ip_event_got_ip_t *event = static_cast<const ip_event_got_ip_t *>(event_data);
    s_ethernet_has_ip = true;
    ESP_LOGI(TAG, "Ethernet got DHCP IPv4: " IPSTR ", gateway: " IPSTR ", netmask: " IPSTR,
             IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw), IP2STR(&event->ip_info.netmask));
    esp_err_t probe_http_err = probe_http_start();
    if (probe_http_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start esp-thread-probe API: %s", esp_err_to_name(probe_http_err));
    }
    otDeviceRole role = current_thread_role();
    bool has_dataset = log_active_dataset_from_ot("Ethernet got IP");
    bool attached = role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
    unsigned fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
    ESP_LOGI(TAG, "Ethernet got IP: BR eligibility: fabric_count=%u dataset=%d thread_attached=%d thread_role=%s ethernet_ip=%d",
             fabric_count, has_dataset, attached, thread_role_to_str(role), s_ethernet_has_ip);
    if (fabric_count > 0 && has_dataset && attached) {
        init_thread_br_backbone("ETH_DEF");
    } else {
        ESP_LOGI(TAG, "Ethernet is up for management UI; BR init deferred until a Matter fabric, Thread dataset, and Thread attachment exist");
    }
}

esp_err_t start_w5500_ethernet()
{
    if (s_ethernet_handle) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting W5500 Ethernet: MOSI=%d MISO=%d SCLK=%d CS=%d INT=%d RST=%d",
             kW5500MosiGpio, kW5500MisoGpio, kW5500SclkGpio, kW5500CsGpio, kW5500IntGpio, kW5500ResetGpio);

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(err));
        return err;
    }

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = kW5500MosiGpio;
    bus_config.miso_io_num = kW5500MisoGpio;
    bus_config.sclk_io_num = kW5500SclkGpio;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;

    err = spi_bus_initialize(kW5500SpiHost, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize W5500 SPI bus: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_ethernet_netif = esp_netif_new(&netif_config);
    if (!s_ethernet_netif) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return ESP_FAIL;
    }

    spi_device_interface_config_t spi_device_config = {};
    spi_device_config.mode = 0;
    spi_device_config.clock_speed_hz = kW5500SpiClockMhz * 1000 * 1000;
    spi_device_config.queue_size = 20;
    spi_device_config.spics_io_num = kW5500CsGpio;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(kW5500SpiHost, &spi_device_config);
    w5500_config.base.int_gpio_num = kW5500IntGpio;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = kW5500PhyAddr;
    phy_config.reset_gpio_num = kW5500ResetGpio;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!mac || !phy) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC/PHY");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_ethernet_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install W5500 Ethernet driver: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t eth_mac[6] = {};
    ESP_ERROR_CHECK(esp_read_mac(eth_mac, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(s_ethernet_handle, ETH_CMD_S_MAC_ADDR, eth_mac));

    s_ethernet_glue = esp_eth_new_netif_glue(s_ethernet_handle);
    if (!s_ethernet_glue) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif glue");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_netif_attach(s_ethernet_netif, s_ethernet_glue));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_ethernet_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_ethernet_got_ip, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(s_ethernet_handle));

    return ESP_OK;
}

esp_err_t request_w5500_ethernet_start(const char *reason)
{
    bool expected = false;
    if (!s_w5500_start_requested.compare_exchange_strong(expected, true)) {
        ESP_LOGI(TAG, "%s: W5500 start already requested", reason);
        return ESP_OK;
    }

    esp_err_t err = start_w5500_ethernet();
    if (err != ESP_OK) {
        s_w5500_start_requested.store(false);
        ESP_LOGE(TAG, "%s: W5500 Ethernet start failed: %s", reason, esp_err_to_name(err));
    }
    return err;
}

class LoggingThreadBorderRouterDelegate : public GenericOpenThreadBorderRouterDelegate {
public:
    using GenericOpenThreadBorderRouterDelegate::GenericOpenThreadBorderRouterDelegate;

    CHIP_ERROR Init(ThreadBorderRouterManagement::Delegate::AttributeChangeCallback *callback) override
    {
        ESP_LOGI(TAG, "ThreadBorderRouterManagement delegate init, callback=%p", callback);
        CHIP_ERROR err = GenericOpenThreadBorderRouterDelegate::Init(callback);
        ESP_LOGI(TAG, "ThreadBorderRouterManagement delegate init result: %s", chip::ErrorStr(err));
        return err;
    }

    CHIP_ERROR GetDataset(chip::Thread::OperationalDataset &dataset,
                          ThreadBorderRouterManagement::Delegate::DatasetType type) override
    {
        CHIP_ERROR err = GenericOpenThreadBorderRouterDelegate::GetDataset(dataset, type);
        ESP_LOGI(TAG, "ThreadBorderRouterManagement Get%sDataset -> %s",
                 type == ThreadBorderRouterManagement::Delegate::DatasetType::kActive ? "Active" : "Pending",
                 chip::ErrorStr(err));
        if (err == CHIP_NO_ERROR) {
            log_dataset_summary(type == ThreadBorderRouterManagement::Delegate::DatasetType::kActive ?
                                "Active Dataset read" : "Pending Dataset read",
                                dataset);
        }
        return err;
    }

    void SetActiveDataset(const chip::Thread::OperationalDataset &activeDataset, uint32_t sequenceNum,
                          ThreadBorderRouterManagement::Delegate::ActivateDatasetCallback *callback) override
    {
        ESP_LOGI(TAG, "ThreadBorderRouterManagement.SetActiveDatasetRequest received, sequence=%" PRIu32, sequenceNum);
        log_dataset_summary("Requested Active Dataset", activeDataset);
        GenericOpenThreadBorderRouterDelegate::SetActiveDataset(activeDataset, sequenceNum, callback);
        ESP_LOGI(TAG, "SetActiveDatasetRequest dispatched to OpenThread, current role=%s", current_thread_role_to_str());
    }

    CHIP_ERROR CommitActiveDataset() override
    {
        ESP_LOGI(TAG, "CommissioningComplete: committing active dataset, current role=%s", current_thread_role_to_str());
        CHIP_ERROR err = GenericOpenThreadBorderRouterDelegate::CommitActiveDataset();
        ESP_LOGI(TAG, "CommitActiveDataset result: %s", chip::ErrorStr(err));
        return err;
    }

    CHIP_ERROR RevertActiveDataset() override
    {
        ESP_LOGW(TAG, "Fail-safe expired or startup recovery: reverting active dataset");
        CHIP_ERROR err = GenericOpenThreadBorderRouterDelegate::RevertActiveDataset();
        ESP_LOGW(TAG, "RevertActiveDataset result: %s", chip::ErrorStr(err));
        return err;
    }

    CHIP_ERROR SetPendingDataset(const chip::Thread::OperationalDataset &pendingDataset) override
    {
        ESP_LOGI(TAG, "ThreadBorderRouterManagement.SetPendingDatasetRequest received");
        log_dataset_summary("Requested Pending Dataset", pendingDataset);
        CHIP_ERROR err = GenericOpenThreadBorderRouterDelegate::SetPendingDataset(pendingDataset);
        ESP_LOGI(TAG, "SetPendingDataset result: %s", chip::ErrorStr(err));
        return err;
    }
};

} // namespace

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::PublicEventTypes::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning complete, fabrics=%u",
                 static_cast<unsigned>(chip::Server::GetInstance().GetFabricTable().FabricCount()));
        start_w5500_backbone_if_thread_ready("Matter commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Matter fail-safe timer expired before CommissioningComplete");
        break;
    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEAdvertisingChange:
        ESP_LOGI(TAG, "Matter BLE advertising %s",
                 activity_change_to_str(event->CHIPoBLEAdvertisingChange.Result));
        break;
    case chip::DeviceLayer::DeviceEventType::kCHIPoBLESubscribe:
        ESP_LOGI(TAG, "Matter BLE central subscribed");
        break;
    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEUnsubscribe:
        ESP_LOGI(TAG, "Matter BLE central unsubscribed");
        break;
    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionEstablished:
        ESP_LOGI(TAG, "Matter BLE connection established");
        break;
    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionClosed:
        ESP_LOGI(TAG, "Matter BLE connection closed");
        break;
    case chip::DeviceLayer::DeviceEventType::kSecureSessionEstablished:
        ESP_LOGI(TAG, "Matter secure session established");
        break;
    case chip::DeviceLayer::DeviceEventType::kThreadConnectivityChange:
        ESP_LOGI(TAG, "Thread connectivity %s, role=%s",
                 connectivity_change_to_str(event->ThreadConnectivityChange.Result), current_thread_role_to_str());
        start_w5500_backbone_if_thread_ready("Thread connectivity change");
        break;
    case chip::DeviceLayer::DeviceEventType::kThreadStateChange:
        ESP_LOGI(TAG, "Thread state changed: role_changed=%d addr_changed=%d netdata_changed=%d child_changed=%d "
                      "ot_flags=0x%08" PRIx32 " role=%s",
                 event->ThreadStateChange.RoleChanged, event->ThreadStateChange.AddressChanged,
                 event->ThreadStateChange.NetDataChanged, event->ThreadStateChange.ChildNodesChanged,
                 event->ThreadStateChange.OpenThread.Flags, current_thread_role_to_str());
        if (event->ThreadStateChange.OpenThread.Flags & OT_CHANGED_ACTIVE_DATASET) {
            ESP_LOGI(TAG, "OpenThread active dataset changed");
        }
        if (event->ThreadStateChange.OpenThread.Flags & OT_CHANGED_PENDING_DATASET) {
            ESP_LOGI(TAG, "OpenThread pending dataset changed");
        }
        if (event->ThreadStateChange.OpenThread.Flags & OT_CHANGED_THREAD_ROLE) {
            ESP_LOGI(TAG, "OpenThread role changed: %s", current_thread_role_to_str());
        }
        if (event->ThreadStateChange.OpenThread.Flags & OT_CHANGED_THREAD_NETIF_STATE) {
            ESP_LOGI(TAG, "OpenThread Thread netif state changed");
        }
        if (event->ThreadStateChange.OpenThread.Flags &
            (OT_CHANGED_ACTIVE_DATASET | OT_CHANGED_THREAD_ROLE | OT_CHANGED_THREAD_NETIF_STATE)) {
            start_w5500_backbone_if_thread_ready("Thread state change");
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kESPSystemEvent:
        if (event->Platform.ESPSystemEvent.Base == IP_EVENT &&
                event->Platform.ESPSystemEvent.Id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "Wi-Fi station got IP");
        } else if (event->Platform.ESPSystemEvent.Base == IP_EVENT &&
                event->Platform.ESPSystemEvent.Id == IP_EVENT_ETH_GOT_IP) {
            ESP_LOGI(TAG, "Ethernet got IP");
        }
        break;
    default:
        break;
    }
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    nvs_flash_init();
    // If there is no commissioner in the controller, we need a default node so that the controller can be commissioned
    // to a specific fabric.
    node::config_t node_config;
    auto &basic_info = node_config.root_node.basic_information;
    strlcpy(basic_info.node_label, CONFIG_MATTER_ACCESSORY_NAME, sizeof(basic_info.node_label));
    node_t *node = node::create(&node_config, NULL, NULL);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }
    endpoint_t *root_endpoint = endpoint::get(node, 0);
    if (!root_endpoint) {
        ESP_LOGE(TAG, "Failed to find root endpoint 0");
        return;
    }
    on_off_light::config_t light_config;
    light_config.on_off.on_off = false;
    endpoint_t *light_endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    if (!light_endpoint) {
        ESP_LOGE(TAG, "Failed to create fake On/Off Light endpoint");
        return;
    }
    ESP_LOGI(TAG, "Fake Matter-over-Thread On/Off Light endpoint=%u; Thread NetworkCommissioning endpoint=0",
             static_cast<unsigned>(endpoint::get_id(light_endpoint)));
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#if defined(CONFIG_OPENTHREAD_BORDER_ROUTER) && defined(CONFIG_AUTO_UPDATE_RCP)
    esp_vfs_spiffs_conf_t rcp_fw_conf = {
        .base_path = "/rcp_fw", .partition_label = "rcp_fw", .max_files = 10, .format_if_mount_failed = false
    };
    if (ESP_OK != esp_vfs_spiffs_register(&rcp_fw_conf)) {
        ESP_LOGE(TAG, "Failed to mount rcp firmware storage");
        return;
    }
    esp_rcp_update_config_t rcp_update_config = ESP_OPENTHREAD_RCP_UPDATE_CONFIG();
    esp_rcp_update_init(&rcp_update_config);
    esp_ot_register_rcp_handler();
#endif // CONFIG_OPENTHREAD_BORDER_ROUTER && CONFIG_AUTO_UPDATE_RCP
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif // CHIP_DEVICE_CONFIG_ENABLE_THREAD

#if CONFIG_OPENTHREAD_BR_START_WEB
    esp_vfs_spiffs_conf_t web_server_conf = {
        .base_path = "/spiffs", .partition_label = "web_storage", .max_files = 10, .format_if_mount_failed = false
    };
    err = esp_vfs_spiffs_register(&web_server_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount Border Router Web UI storage: %s", esp_err_to_name(err));
        return;
    }
#endif

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Matter start failed: %d", err);
    }
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && defined(CONFIG_OPENTHREAD_BORDER_ROUTER) && defined(CONFIG_AUTO_UPDATE_RCP)
    if (err == ESP_OK) {
        // The updater only reads the running RCP version when it matches the bundled image.
        // Do not cycle Thread after Matter has restored its operational state: doing so tears
        // down the interface underneath existing operational DNS-SD and UDP listeners.
        ESP_LOGI(TAG, "Checking RCP firmware without cycling the active Thread interface");
        esp_ot_update_rcp_if_different();
    }
#endif
    if (err == ESP_OK) {
#if CONFIG_OPENTHREAD_BR_START_WEB
        esp_br_web_start(const_cast<char *>("/spiffs"));
        ESP_LOGI(TAG, "Official ESP Border Router Web UI/REST server armed; it will start when W5500 gets an IP");
#endif
        // Keep probe warnings/errors, but suppress its periodic router scan progress output.
        esp_log_level_set("probe_thread", ESP_LOG_WARN);
        esp_err_t probe_scan_err = probe_thread_start_background_scan();
        if (probe_scan_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start esp-thread-probe background scan: %s", esp_err_to_name(probe_scan_err));
        }
        request_w5500_ethernet_start("Matter started");
        print_apple_home_onboarding_payload();
        log_active_dataset_from_ot("Matter started");
        ESP_LOGI(TAG, "Matter started. W5500 management IP may come up now; BR init is deferred until Thread dataset is provisioned and attached.");
    }
#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif // CONFIG_ENABLE_CHIP_SHELL
}
