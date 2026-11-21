/* SPDX-License-Identifier: Apache-2.0 */
#include "cellular_policy.h"

#if KIIKO_BOARD_METALIO47
#include <stdatomic.h>
#include "cellular_modem.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_transfer.h"
#include "settings.h"

#define POLICY_PERIOD_MS 3000
#define DIAL_RETRY_MIN_MS 60000
#define WIFI_GRACE_MS 8000
#define MODEM_DIAL_TIMEOUT_MS 60000

static atomic_int s_capability = CELLULAR_CAPABILITY_UNKNOWN;
static atomic_bool s_started, s_restart_requested, s_dialing, s_pause_ack;
static atomic_uint s_paused_until_s;
static TaskHandle_t s_policy_task;

cellular_capability_t cellular_capability(void) { return atomic_load(&s_capability); }
bool cellular_available(void) { return cellular_capability() == CELLULAR_CAPABILITY_PRESENT; }
bool cellular_ready(void) { return cellular_available() && cellular_modem_is_ready(); }
bool cellular_connecting(void) { return cellular_available() && atomic_load(&s_dialing); }

static void notify_policy(void) {
    // Published only after a successful probe; thereafter the task never exits.
    if (cellular_available() && s_policy_task) xTaskNotifyGive(s_policy_task);
}

void cellular_policy_request_restart(void) {
    if (!cellular_available()) return;
    atomic_store(&s_restart_requested, true);
    cellular_modem_cancel();
    notify_policy();
}

bool cellular_policy_pause(uint32_t ms) {
    atomic_store(&s_pause_ack, false);
    atomic_store(&s_paused_until_s, (uint32_t)(esp_timer_get_time()/1000000) +
                 (ms ? ms/1000 + (ms%1000 != 0) : 1));
    cellular_modem_cancel();
    notify_policy();
    for (int i=0; i<25; ++i) {
        if (atomic_load(&s_pause_ack) || !atomic_load(&s_started) ||
            cellular_capability()==CELLULAR_CAPABILITY_NOT_DETECTED) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

void cellular_policy_resume(void) {
    atomic_store(&s_paused_until_s, 0);
    notify_policy();
}

static bool wifi_online(void) {
    read_pico_transfer_status_t st;
    read_pico_transfer_get_status(&st);
    return st.network_ready && st.mode == READ_PICO_TRANSFER_MODE_STA;
}

static void policy_task(void *arg) {
    (void)arg;
    // Delay to let storage/display startup settle; never wait on the UI thread.
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_err_t result;
    for (;;) {
        uint32_t pause_until=atomic_load(&s_paused_until_s);
        if (pause_until && (uint32_t)(esp_timer_get_time()/1000000)<pause_until) {
            atomic_store(&s_pause_ack, true);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (pause_until) atomic_compare_exchange_strong(&s_paused_until_s, &pause_until, 0);
        atomic_store(&s_pause_ack, false);
        cellular_modem_stop(); // clears a cancellation from an earlier pause
        result=cellular_modem_init_default();
        if (result==ESP_OK) result=cellular_modem_probe();
        if (atomic_load(&s_paused_until_s)) {
            cellular_modem_deinit();
            continue; // a cancelled probe cannot identify a Wi-Fi-only device
        }
        break;
    }
    if (result != ESP_OK) {
        // No background dial loop or UART/DMA allocation on a Wi-Fi-only unit.
        // An inconclusive boot probe is not persisted as a device identity.
        cellular_modem_deinit();
        atomic_store(&s_capability, CELLULAR_CAPABILITY_NOT_DETECTED);
        ESP_LOGI("cellular_policy", "No modem response this boot (%s); Wi-Fi mode",
                 esp_err_to_name(result));
        vTaskDelete(NULL);
        return;
    }
    s_policy_task = xTaskGetCurrentTaskHandle();
    atomic_store(&s_capability, CELLULAR_CAPABILITY_PRESENT);
    ESP_LOGI("cellular_policy", "Cellular hardware detected independently of SIM/network");
    const int64_t boot_ms = esp_timer_get_time()/1000;
    int64_t last_dial_ms = -DIAL_RETRY_MIN_MS;
    bool was_paused = false;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLICY_PERIOD_MS));
        const int64_t now = esp_timer_get_time()/1000;
        const uint32_t pause_until = atomic_load(&s_paused_until_s);
        const bool paused = pause_until && (uint32_t)(now/1000) < pause_until;
        if (paused) {
            cellular_modem_stop();
            atomic_store(&s_pause_ack, true);
            was_paused = true;
            continue;
        }
        atomic_store(&s_pause_ack, false);
        if (pause_until) atomic_store(&s_paused_until_s, 0);
        if (was_paused) { last_dial_ms = -DIAL_RETRY_MIN_MS; was_paused = false; }
        if (atomic_exchange(&s_restart_requested, false)) {
            cellular_modem_stop();
            last_dial_ms = -DIAL_RETRY_MIN_MS;
        }
        if (!app_settings_cellular_enabled() || wifi_online()) {
            cellular_modem_stop();
            last_dial_ms = -DIAL_RETRY_MIN_MS;
            continue;
        }
        if (cellular_ready()) continue;
        if (now-boot_ms < WIFI_GRACE_MS || now-last_dial_ms < DIAL_RETRY_MIN_MS) continue;
        last_dial_ms = now;
        cellular_modem_stop();
        cellular_modem_set_sim_mode((int)app_settings_sim_slot());
        atomic_store(&s_dialing, true);
        result = cellular_modem_start(MODEM_DIAL_TIMEOUT_MS);
        atomic_store(&s_dialing, false);
        // Requests may arrive while start() blocks; apply them before exposing
        // a live connection for another policy interval.
        if (!app_settings_cellular_enabled() || wifi_online() ||
            atomic_load(&s_paused_until_s) || atomic_load(&s_restart_requested)) {
            cellular_modem_stop();
        }
        ESP_LOGI("cellular_policy", "Dial result: %s", esp_err_to_name(result));
    }
}

esp_err_t cellular_policy_start(void) {
    if (atomic_exchange(&s_started, true)) return ESP_OK;
    if (xTaskCreate(policy_task, "cell_policy", 4096, NULL, 2, NULL) != pdPASS) {
        atomic_store(&s_started, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#else
cellular_capability_t cellular_capability(void) { return CELLULAR_CAPABILITY_NOT_DETECTED; }
bool cellular_available(void) { return false; }
bool cellular_ready(void) { return false; }
bool cellular_connecting(void) { return false; }
esp_err_t cellular_policy_start(void) { return ESP_OK; }
void cellular_policy_request_restart(void) {}
bool cellular_policy_pause(uint32_t ms) { (void)ms; return true; }
void cellular_policy_resume(void) {}
#endif
