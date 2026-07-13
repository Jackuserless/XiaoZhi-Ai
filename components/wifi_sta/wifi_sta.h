#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STA_STATE_IDLE = 0,
    WIFI_STA_STATE_INITIALIZING,
    WIFI_STA_STATE_CONNECTING,
    WIFI_STA_STATE_CONNECTED,
    WIFI_STA_STATE_DISCONNECTED,
    WIFI_STA_STATE_STOPPING,
    WIFI_STA_STATE_ERROR
} wifi_sta_state_t;

typedef struct wifi_sta_t wifi_sta_t;

wifi_sta_t *wifi_sta_create(void);
void wifi_sta_destroy(wifi_sta_t *handle);
void wifi_sta_init(wifi_sta_t *handle);
void wifi_sta_start(wifi_sta_t *handle);
void wifi_sta_stop(wifi_sta_t *handle);
void wifi_sta_connect(wifi_sta_t *handle, const char *ssid, const char *password);
bool wifi_sta_is_connected(const wifi_sta_t *handle);
void wifi_sta_update(wifi_sta_t *handle);
void wifi_sta_wait_for_connection(wifi_sta_t *handle, unsigned long timeout_ms);
wifi_sta_state_t wifi_sta_state(const wifi_sta_t *handle);
void wifi_sta_task_loop(wifi_sta_t *handle);
void wifi_sta_update_ip_info(wifi_sta_t *handle);

#ifdef __cplusplus
}
#endif