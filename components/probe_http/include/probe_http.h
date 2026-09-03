#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the esp-thread-probe-compatible API server on TCP port 8080. */
esp_err_t probe_http_start(void);

/** Publish the Matter onboarding payload through GET /matter/qr-code. */
void probe_http_set_matter_onboarding(const char *qr_code, const char *manual_code, uint32_t setup_pin);

#ifdef __cplusplus
}
#endif
