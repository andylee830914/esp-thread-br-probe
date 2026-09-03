#include "probe_http.h"

#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "probe_thread.h"

#define PROBE_HTTP_PORT 8080

static const char *TAG = "probe_http";
static httpd_handle_t s_server;
static char s_matter_qr_code[256];
static char s_matter_manual_code[32];
static uint32_t s_matter_setup_pin;

typedef cJSON *(*probe_json_factory_t)(void);

typedef struct {
    const char *path;
    probe_json_factory_t factory;
} probe_route_t;

static cJSON *health_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "board", "esp32s3-thread-border-router");
    cJSON_AddBoolToObject(root, "matter_started", true);
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    return root;
}

static cJSON *info_json(void)
{
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "esp-thread-probe-border-router");
    cJSON_AddStringToObject(root, "version", "0.2.0-integrated");
    cJSON_AddStringToObject(root, "transport", "direct-http");
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(root, "chip_model", chip.model);
    cJSON_AddNumberToObject(root, "chip_revision", chip.revision);
    cJSON_AddNumberToObject(root, "cores", chip.cores);
    cJSON_AddItemToObject(root, "thread", probe_thread_info_json());
    return root;
}

static cJSON *uplink_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "source", "integrated");
    cJSON_AddStringToObject(root, "name", "esp-thread-probe-border-router");
    cJSON_AddStringToObject(root, "transport", "direct-http");
    cJSON_AddNumberToObject(root, "http_port", PROBE_HTTP_PORT);
    cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
    return root;
}

static cJSON *matter_qr_code_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "qr_code", s_matter_qr_code);
    cJSON_AddStringToObject(root, "manual_pairing_code", s_matter_manual_code);
    cJSON_AddNumberToObject(root, "setup_pin", s_matter_setup_pin);
    cJSON_AddStringToObject(root, "warning", "Commissioning secret; expose only on a trusted network");
    return root;
}

static const probe_route_t s_routes[] = {
    {"/uplink", uplink_json},
    {"/health", health_json},
    {"/info", info_json},
    {"/mesh", probe_thread_mesh_json},
    {"/neighbors", probe_thread_neighbors_json},
    {"/routers", probe_thread_routers_json},
    {"/children", probe_thread_children_json},
    {"/topology", probe_thread_topology_json},
    {"/router-neighbors", probe_thread_router_neighbors_json},
    {"/router-neighbors/scan", probe_thread_router_neighbors_scan_json},
    {"/router", probe_thread_router_json},
    {"/ipaddr", probe_thread_ipaddr_json},
    {"/leader", probe_thread_leader_json},
    {"/dataset", probe_thread_dataset_json},
    {"/matter/qr-code", matter_qr_code_json},
};

void probe_http_set_matter_onboarding(const char *qr_code, const char *manual_code, uint32_t setup_pin)
{
    strlcpy(s_matter_qr_code, qr_code ? qr_code : "", sizeof(s_matter_qr_code));
    strlcpy(s_matter_manual_code, manual_code ? manual_code : "", sizeof(s_matter_manual_code));
    s_matter_setup_pin = setup_pin;
}

static esp_err_t probe_get_handler(httpd_req_t *req)
{
    const probe_route_t *route = req->user_ctx;
    ESP_RETURN_ON_FALSE(route && route->factory, ESP_ERR_INVALID_ARG, TAG, "invalid route");

    cJSON *root = route->factory();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "failed to create response for %s", route->path);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(payload, ESP_ERR_NO_MEM, TAG, "failed to serialize response for %s", route->path);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

esp_err_t probe_http_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = PROBE_HTTP_PORT;
    config.ctrl_port = PROBE_HTTP_PORT + 1;
    config.max_uri_handlers = sizeof(s_routes) / sizeof(s_routes[0]);
    config.stack_size = 8192;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 8;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "failed to start API server");

    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
        httpd_uri_t uri = {
            .uri = s_routes[i].path,
            .method = HTTP_GET,
            .handler = probe_get_handler,
            .user_ctx = (void *)&s_routes[i],
        };
        esp_err_t err = httpd_register_uri_handler(s_server, &uri);
        if (err != ESP_OK) {
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }

    ESP_LOGI(TAG, "esp-thread-probe API listening on port %d", PROBE_HTTP_PORT);
    return ESP_OK;
}
