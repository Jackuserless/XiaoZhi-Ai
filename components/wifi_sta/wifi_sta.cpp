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

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst == nullptr || dst_size == 0) {
        return;
    }

    if (src == nullptr) {
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

WIFI_STA::WIFI_STA()
    : initialized_(false),
      connected_(false),
      connection_requested_(false),
      state_(State::kIdle),
      task_handle_(nullptr),
      task_running_(false),
      stop_requested_(false),
      netif_(nullptr),
      instance_any_id_(nullptr),
      instance_got_ip_(nullptr) {
    ssid_[0] = '\0';
    password_[0] = '\0';
}

WIFI_STA::~WIFI_STA() {
    disconnect();
}

void WIFI_STA::set_state(State new_state) {
    if (state_ != new_state) {
        ESP_LOGI(TAG, "Wi-Fi state: %s -> %s", state_name(state_), state_name(new_state));
        state_ = new_state;
    }
}

const char *WIFI_STA::state_name(State state) const {
    switch (state) {
        case State::kIdle:
            return "IDLE";
        case State::kInitializing:
            return "INITIALIZING";
        case State::kConnecting:
            return "CONNECTING";
        case State::kConnected:
            return "CONNECTED";
        case State::kDisconnected:
            return "DISCONNECTED";
        case State::kStopping:
            return "STOPPING";
        case State::kError:
            return "ERROR";
    }
    return "UNKNOWN";
}

void WIFI_STA::init() {
    if (state_ == State::kInitializing || state_ == State::kConnecting || state_ == State::kConnected ||
        state_ == State::kStopping) {
        return;
    }

    if (initialized_) {
        set_state(State::kIdle);
        return;
    }

    set_state(State::kInitializing);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    netif_ = esp_netif_create_default_wifi_sta();
    if (netif_ == nullptr) {
        set_state(State::kError);
        ESP_LOGE(TAG, "Failed to create default Wi-Fi station netif");
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &WIFI_STA::event_handler,
                                                       this,
                                                       &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP,
                                                       &WIFI_STA::event_handler,
                                                       this,
                                                       &instance_got_ip_));

    ESP_ERROR_CHECK(esp_wifi_start());

    initialized_ = true;
    set_state(State::kIdle);
    ESP_LOGI(TAG, "Wi-Fi station initialized");
}

void WIFI_STA::start() {
    if (task_running_) {
        return;
    }

    stop_requested_ = false;
    BaseType_t ret = xTaskCreatePinnedToCore(&WIFI_STA::task_entry,
                                             "wifi_sta_task",
                                             4096,
                                             this,
                                             5,
                                             &task_handle_,
                                             1);
    if (ret != pdPASS) {
        task_handle_ = nullptr;
        set_state(State::kError);
        ESP_LOGE(TAG, "Failed to create Wi-Fi task");
        return;
    }

    task_running_ = true;
}

void WIFI_STA::stop() {
    stop_requested_ = true;
    set_state(State::kStopping);
    if (!task_running_ && initialized_) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        if (instance_any_id_ != nullptr) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
            instance_any_id_ = nullptr;
        }
        if (instance_got_ip_ != nullptr) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
            instance_got_ip_ = nullptr;
        }
        initialized_ = false;
        connected_ = false;
        connection_requested_ = false;
        ssid_[0] = '\0';
        password_[0] = '\0';
        netif_ = nullptr;
        set_state(State::kDisconnected);
    }
}

void WIFI_STA::connect(const char *ssid, const char *password) {
    if (state_ == State::kConnected) {
        return;
    }

    if (!initialized_) {
        init();
    }

    if (ssid == nullptr || password == nullptr) {
        ESP_LOGE(TAG, "SSID or password is null");
        set_state(State::kError);
        return;
    }

    copy_string(ssid_, sizeof(ssid_), ssid);
    copy_string(password_, sizeof(password_), password);

    wifi_config_t wifi_config = {};
    copy_string((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), ssid_);
    copy_string((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), password_);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    connected_ = false;
    connection_requested_ = true;
    set_state(State::kConnecting);
    start();

    ESP_LOGI(TAG, "Requesting connect to SSID: %s", ssid_);
}

bool WIFI_STA::is_connected() const {
    return state_ == State::kConnected || connected_;
}

void WIFI_STA::update() {
    switch (state_) {
        case State::kInitializing:
            if (initialized_) {
                set_state(State::kIdle);
            }
            break;
        case State::kStopping:
            set_state(State::kDisconnected);
            break;
        case State::kError:
            set_state(State::kDisconnected);
            break;
        default:
            break;
    }
}

void WIFI_STA::wait_for_connection(unsigned long timeout_ms) {
    const unsigned long steps = (timeout_ms + 99) / 100;
    for (unsigned long i = 0; i < steps; ++i) {
        update();
        if (state_ == State::kConnected) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

WIFI_STA::State WIFI_STA::state() const {
    return state_;
}

void WIFI_STA::task_entry(void *arg) {
    WIFI_STA *self = static_cast<WIFI_STA *>(arg);
    self->task_loop();
}

void WIFI_STA::task_loop() {
    while (!stop_requested_) {
        switch (state_) {
            case State::kIdle:
                if (connection_requested_) {
                    esp_err_t err = esp_wifi_connect();
                    if (err != ESP_OK) {
                        set_state(State::kError);
                        ESP_LOGE(TAG, "esp_wifi_connect failed: %d", err);
                    }
                    connection_requested_ = false;
                }
                break;
            case State::kInitializing:
                if (initialized_) {
                    set_state(State::kIdle);
                }
                break;
            case State::kConnecting:
                // wait for events: STA_CONNECTED / GOT_IP / DISCONNECTED
                break;
            case State::kConnected:
                update_ip_info();
                break;
            case State::kDisconnected:
                if (connection_requested_) {
                    set_state(State::kConnecting);
                }
                break;
            case State::kStopping:
                esp_wifi_disconnect();
                esp_wifi_stop();
                esp_wifi_deinit();
                if (instance_any_id_ != nullptr) {
                    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
                    instance_any_id_ = nullptr;
                }
                if (instance_got_ip_ != nullptr) {
                    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
                    instance_got_ip_ = nullptr;
                }
                initialized_ = false;
                connected_ = false;
                connection_requested_ = false;
                ssid_[0] = '\0';
                password_[0] = '\0';
                netif_ = nullptr;
                set_state(State::kDisconnected);
                break;
            case State::kError:
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    task_running_ = false;
    task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void WIFI_STA::update_ip_info() {
    if (netif_ == nullptr) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif_, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "LWIP IP: " IPSTR, IP2STR(&ip_info.ip));
    }
}

void WIFI_STA::event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    WIFI_STA *self = static_cast<WIFI_STA *>(arg);
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                self->set_state(WIFI_STA::State::kIdle);
                break;
            case WIFI_EVENT_STA_CONNECTED:
                self->set_state(WIFI_STA::State::kConnecting);
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                self->connected_ = false;
                self->connection_requested_ = false;
                self->set_state(WIFI_STA::State::kDisconnected);
                ESP_LOGW(TAG, "Wi-Fi disconnected");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        self->connected_ = true;
        self->connection_requested_ = false;
        self->set_state(WIFI_STA::State::kConnected);
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}