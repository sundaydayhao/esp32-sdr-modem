#pragma once

#include "esp_err.h"

/* IDF 5.5 delivers an EP0 completion before decrementing its inflight count.
 * Keep both tasks on core 0 and the daemon above the client, so the daemon
 * finishes bookkeeping before the client reuses a transfer or closes a device. */
#define USB_HOST_DAEMON_PRIORITY 7
#define USB_HOST_CLIENT_PRIORITY 6

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_host_service_start(void);
esp_err_t usb_host_service_power_on(void);
/* Called only by the USB client owner after closing/draining its device. */
esp_err_t usb_host_service_restart_port(void);

#ifdef __cplusplus
}
#endif
