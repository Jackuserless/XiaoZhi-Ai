#include "wifi_sta.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "WIFI_STA";

typedef struct wifi_sta_handle_t {
    bool initialized;
    bool connected;
    bool connection_requested;
    wifi_sta_state_t state;
    TaskHandle_t task_handle;
    bool task_running;
    bool stop_requested;
    esp_netif_t *netif;
    void *instance_any_id;
    void *instance_got_ip;
    char ssid[33];
    char password[65];
} wifi_sta_handle_t;

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static const char *state_name(wifi_sta_handle_t *self, wifi_sta_state_t state) {
    (void)self;
    switch (state) {
        case WIFI_STA_STATE_IDLE:
            return "IDLE";
        case WIFI_STA_STATE_INITIALIZING:
            return "INITIALIZING";
        case WIFI_STA_STATE_CONNECTING:
            return "CONNECTING";
        case WIFI_STA_STATE_CONNECTED:
            return "CONNECTED";
        case WIFI_STA_STATE_DISCONNECTED:
            return "DISCONNECTED";
        case WIFI_STA_STATE_STOPPING:
            return "STOPPING";
        case WIFI_STA_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

static void set_state(wifi_sta_handle_t *self, wifi_sta_state_t new_state) {
    if (self->state != new_state) {
        ESP_LOGI(TAG, "Wi-Fi state: %s -> %s", state_name(self, self->state), state_name(self, new_state));
        self->state = new_state;
    }
}

static void wifi_sta_task_entry(void *arg) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)arg;
    wifi_sta_task_loop(self);
}

static void wifi_sta_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                set_state(self, WIFI_STA_STATE_IDLE);
                break;
            case WIFI_EVENT_STA_CONNECTED:
                set_state(self, WIFI_STA_STATE_CONNECTING);
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                self->connected = false;
                self->connection_requested = false;
                set_state(self, WIFI_STA_STATE_DISCONNECTED);
                ESP_LOGW(TAG, "Wi-Fi disconnected");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        self->connected = true;
        self->connection_requested = false;
        set_state(self, WIFI_STA_STATE_CONNECTED);
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

wifi_sta_t *wifi_sta_create(void) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)calloc(1, sizeof(wifi_sta_handle_t));
    if (self != NULL) {
        self->state = WIFI_STA_STATE_IDLE;
    }
    return (wifi_sta_t *)self;
}

void wifi_sta_destroy(wifi_sta_t *handle) {
    if (handle == NULL) {
        return;
    }
    wifi_sta_stop(handle);
    free(handle);
}

void wifi_sta_init(wifi_sta_t *handle) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)handle;
    if (self == NULL) {
        return;
    }

    if (self->state == WIFI_STA_STATE_INITIALIZING || self->state == WIFI_STA_STATE_CONNECTING ||
        self->state == WIFI_STA_STATE_CONNECTED || self->state == WIFI_STA_STATE_STOPPING) {
        return;
    }

    if (self->initialized) {
        set_state(self, WIFI_STA_STATE_IDLE);
        return;
    }

    set_state(self, WIFI_STA_STATE_INITIALIZING);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    self->netif = esp_netif_create_default_wifi_sta();
    if (self->netif == NULL) {
        set_state(self, WIFI_STA_STATE_ERROR);
        ESP_LOGE(TAG, "Failed to create default Wi-Fi station netif");
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &wifi_sta_event_handler,
                                                       self,
                                                       &self->instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP,
                                                       &wifi_sta_event_handler,
                                                       self,
                                                       &self->instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_start());
    self->initialized = true;
    set_state(self, WIFI_STA_STATE_IDLE);
    ESP_LOGI(TAG, "Wi-Fi station initialized");
}

void wifi_sta_start(wifi_sta_t *handle) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)handle;
    if (self == NULL || self->task_running) {
        return;
    }

    self->stop_requested = false;
    BaseType_t ret = xTaskCreatePinnedToCore(&wifi_sta_task_entry,
                                             "wifi_sta_task",
                                             4096,
                                             self,
                                             5,
                                             &self->task_handle,
                                             1);
    if (ret != pdPASS) {
        self->task_handle = NULL;
        set_state(self, WIFI_STA_STATE_ERROR);
        ESP_LOGE(TAG, "Failed to create Wi-Fi task");
        return;
    }

    self->task_running = true;
}

void wifi_sta_stop(wifi_sta_t *handle) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)handle;
    if (self == NULL) {
        return;
    }

    self->stop_requested = true;
    set_state(self, WIFI_STA_STATE_STOPPING);
    if (!self->task_running && self->initialized) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        if (self->instance_any_id != NULL) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, self->instance_any_id);
            self->instance_any_id = NULL;
        }
        if (self->instance_got_ip != NULL) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, self->instance_got_ip);
            self->instance_got_ip = NULL;
        }
        self->initialized = false;
        self->connected = false;
        self->connection_requested = false;
        self->ssid[0] = '\0';
        self->password[0] = '\0';
        self->netif = NULL;
        set_state(self, WIFI_STA_STATE_DISCONNECTED);
    }
}

void wifi_sta_connect(wifi_sta_t *handle, const char *ssid, const char *password) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)handle;
    if (self == NULL) {
        return;
    }

    if (self->state == WIFI_STA_STATE_CONNECTED) {
        return;
    }

    if (!self->initialized) {
        wifi_sta_init(handle);
    }

    if (ssid == NULL || password == NULL) {
        ESP_LOGE(TAG, "SSID or password is null");
        set_state(self, WIFI_STA_STATE_ERROR);
        return;
    }

    copy_string(self->ssid, sizeof(self->ssid), ssid);
    copy_string(self->password, sizeof(self->password), password);

    wifi_config_t wifi_config = {0};
    copy_string((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), self->ssid);
    copy_string((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), self->password);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    self->connected = false;
    self->connection_requested = true;
    set_state(self, WIFI_STA_STATE_CONNECTING);
    wifi_sta_start(handle);

    ESP_LOGI(TAG, "Requesting connect to SSID: %s", self->ssid);
}

bool wifi_sta_is_connected(const wifi_sta_t *handle) {
    const wifi_sta_handle_t *self = (const wifi_sta_handle_t *)handle;
    return self != NULL && (self->state == WIFI_STA_STATE_CONNECTED || self->connected);
}

void wifi_sta_update(wifi_sta_t *handle) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)handle;
    if (self == NULL) {
        return;
    }

    switch (self->state) {
        case WIFI_STA_STATE_INITIALIZING:
            if (self->initialized) {
                set_state(self, WIFI_STA_STATE_IDLE);
            }
            break;
        case WIFI_STA_STATE_STOPPING:
            set_state(self, WIFI_STA_STATE_DISCONNECTED);
            break;
        case WIFI_STA_STATE_ERROR:
            set_state(self, WIFI_STA_STATE_DISCONNECTED);
            break;
        default:
            break;
    }
}

void wifi_sta_wait_for_connection(wifi_sta_t *handle, unsigned long timeout_ms) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)handle;
    if (self == NULL) {
        return;
    }

    const unsigned long steps = (timeout_ms + 99) / 100;
    for (unsigned long i = 0; i < steps; ++i) {
        wifi_sta_update(handle);
        if (self->state == WIFI_STA_STATE_CONNECTED) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

wifi_sta_state_t wifi_sta_state(const wifi_sta_t *handle) {
    const wifi_sta_handle_t *self = (const wifi_sta_handle_t *)handle;
    return self != NULL ? self->state : WIFI_STA_STATE_IDLE;
}

void wifi_sta_task_loop(wifi_sta_t *handle) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)handle;
    if (self == NULL) {
        return;
    }

    while (!self->stop_requested) {
        switch (self->state) {
            case WIFI_STA_STATE_IDLE:
                if (self->connection_requested) {
                    esp_err_t err = esp_wifi_connect();
                    if (err != ESP_OK) {
                        set_state(self, WIFI_STA_STATE_ERROR);
                        ESP_LOGE(TAG, "esp_wifi_connect failed: %d", err);
                    }
                    self->connection_requested = false;
                }
                break;
            case WIFI_STA_STATE_INITIALIZING:
                if (self->initialized) {
                    set_state(self, WIFI_STA_STATE_IDLE);
                }
                break;
            case WIFI_STA_STATE_CONNECTING:
                break;
            case WIFI_STA_STATE_CONNECTED:
                wifi_sta_update_ip_info(handle);
                break;
            case WIFI_STA_STATE_DISCONNECTED:
                if (self->connection_requested) {
                    set_state(self, WIFI_STA_STATE_CONNECTING);
                }
                break;
            case WIFI_STA_STATE_STOPPING:
                esp_wifi_disconnect();
                esp_wifi_stop();
                esp_wifi_deinit();
                if (self->instance_any_id != NULL) {
                    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, self->instance_any_id);
                    self->instance_any_id = NULL;
                }
                if (self->instance_got_ip != NULL) {
                    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, self->instance_got_ip);
                    self->instance_got_ip = NULL;
                }
                self->initialized = false;
                self->connected = false;
                self->connection_requested = false;
                self->ssid[0] = '\0';
                self->password[0] = '\0';
                self->netif = NULL;
                set_state(self, WIFI_STA_STATE_DISCONNECTED);
                break;
            case WIFI_STA_STATE_ERROR:
                break;
            default:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    self->task_running = false;
    self->task_handle = NULL;
    vTaskDelete(NULL);
}

void wifi_sta_update_ip_info(wifi_sta_t *handle) {
    wifi_sta_handle_t *self = (wifi_sta_handle_t *)handle;
    if (self == NULL || self->netif == NULL) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(self->netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "LWIP IP: " IPSTR, IP2STR(&ip_info.ip));
    }
}
