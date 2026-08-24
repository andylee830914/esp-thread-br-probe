/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_openthread_lock.h>
#include <esp_openthread_border_router.h>
#include <esp_spiffs.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_feature.h>

#include <app_reset.h>
#include <esp_ot_config.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && defined(CONFIG_OPENTHREAD_BORDER_ROUTER) && defined(CONFIG_AUTO_UPDATE_RCP)
#include <esp_ot_rcp_update.h>
#include <esp_rcp_update.h>
#endif

#include <platform/ESP32/OpenthreadLauncher.h>
#include <platform/OpenThread/GenericThreadBorderRouterDelegate.h>
#include <platform/ThreadStackManager.h>
#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <lib/core/CHIPError.h>
#include <lib/support/ErrorStr.h>
#include <platform/KvsPersistentStorageDelegate.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

#include <inttypes.h>
#include <openthread/thread.h>

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
using chip::app::Clusters::ThreadBorderRouterManagement::GenericOpenThreadBorderRouterDelegate;

namespace {

const chip::RendezvousInformationFlags kAppleHomeRendezvousFlags(chip::RendezvousInformationFlag::kBLE);

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
    PrintQrCodeURL(qr_code);
    ESP_LOGI(TAG, "==================================================");
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
        break;
    case chip::DeviceLayer::DeviceEventType::kESPSystemEvent:
        if (event->Platform.ESPSystemEvent.Base == IP_EVENT &&
                event->Platform.ESPSystemEvent.Id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "Wi-Fi station got IP; initializing Thread Border Router backbone");
#if CONFIG_OPENTHREAD_BORDER_ROUTER
            static bool sThreadBRInitialized = false;
            if (!sThreadBRInitialized) {
                esp_openthread_set_backbone_netif(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
                esp_openthread_lock_acquire(portMAX_DELAY);
                esp_openthread_border_router_init();
                esp_openthread_lock_release();
                sThreadBRInitialized = true;
            }
#endif
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
    node_t *node = node::create(&node_config, NULL, NULL);
    static chip::KvsPersistentStorageDelegate tbr_storage_delegate;
    chip::DeviceLayer::PersistedStorage::KeyValueStoreManager  &kvsManager = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr();
    LogErrorOnFailure(tbr_storage_delegate.Init(&kvsManager));
    GenericOpenThreadBorderRouterDelegate *delegate = chip::Platform::New<LoggingThreadBorderRouterDelegate>(&tbr_storage_delegate);
    if (!delegate) {
        ESP_LOGE(TAG, "Failed to create thread_border_router delegate");
        return;
    }
    char threadBRName[] = "Espressif-ThreadBR";
    delegate->SetThreadBorderRouterName(chip::CharSpan(threadBRName));
    thread_border_router::config_t tbr_config;
    tbr_config.thread_border_router_management.delegate = delegate;
    endpoint_t *tbr_endpoint = thread_border_router::create(node, &tbr_config, ENDPOINT_FLAG_NONE, NULL);
    if (!node || !tbr_endpoint) {
        ESP_LOGE(TAG, "Failed to create data model");
        return;
    }
    cluster_t *tbr_cluster = cluster::get(tbr_endpoint, ThreadBorderRouterManagement::Id);
    cluster::thread_border_router_management::feature::pan_change::add(tbr_cluster);
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

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Matter start failed: %d", err);
    } else {
        print_apple_home_onboarding_payload();
        ESP_LOGI(TAG, "Matter started. If the device has no fabric, BLE commissioning should be advertising.");
    }
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && defined(CONFIG_OPENTHREAD_BORDER_ROUTER) && defined(CONFIG_AUTO_UPDATE_RCP)
    if (err == ESP_OK) {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        using namespace chip::DeviceLayer;
        bool thread_was_enabled = ThreadStackMgr().IsThreadEnabled();
        if (thread_was_enabled) {
            if (ThreadStackMgr().SetThreadEnabled(false) != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "Failed to disable Thread before updating RCP");
                return;
            }
        }
        esp_ot_update_rcp_if_different();
        if (thread_was_enabled) {
            if (ThreadStackMgr().SetThreadEnabled(true) != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "Failed to enable Thread after updating RCP");
                return;
            }
        }
    }
#endif
#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif // CONFIG_ENABLE_CHIP_SHELL
}
