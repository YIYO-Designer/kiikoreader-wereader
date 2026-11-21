/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network policy: Wi-Fi first; if a WeRead account exists but no Wi-Fi STA is
 * available, dial the 4G modem so the reader can stay online. Once the modem
 * has an IP, wr_online() accepts it and every HTTP request transparently uses
 * the cellular netif. If Wi-Fi comes back we tear the modem down (Wi-Fi wins).
 */
#include "cellular_policy.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cellular_modem.h"
#include "read_pico_transfer.h"
#include "settings.h"

static const char *TAG = "cellular_policy";

#define POLICY_PERIOD_MS        3000
#define DIAL_RETRY_MIN_MS       60000     /* throttle failed dial attempts */
#define WIFI_GRACE_MS           8000      /* let Wi-Fi settle before dialing 4G */
#define MODEM_DIAL_TIMEOUT_MS   60000

static bool s_cellular_ready;
static bool s_dialing;
static volatile bool s_restart_requested;
/* 暂停截止时刻用秒存：32 位单次写入，跨任务读不会读到撕裂的 64 位值。
 * Seconds, not microseconds: one 32-bit store, so a cross-task read never sees a torn value. */
static volatile uint32_t s_paused_until_s;   /* 0 = 未暂停 / 0 means not paused */
static TaskHandle_t s_policy_task;
static int64_t s_boot_ms;
static int64_t s_last_dial_ms;

bool cellular_ready(void)  { return s_cellular_ready; }
bool cellular_connecting(void) { return s_dialing; }

/* 用户在设置里换了卡槽：先拆掉现有链路，再按新卡槽重新拨号。切槽必须在模块侧做，
 * 生效路径只能是 stop -> start，所以走同一个策略任务，不另开线程。
 * A slot change in settings has to cycle the link: stop, then start with the new slot. */
void cellular_policy_request_restart(void) {
    s_restart_requested = true;
    if (s_policy_task) xTaskNotifyGive(s_policy_task);
}

/* Wi-Fi 扫描要让路：拆链只由策略任务做（模组的 stop/start 不能跨线程并发），这里负责叫醒它
 * 并等链路真的下来。放在同一线程还有第二个好处：射频下电后那几毫秒的 DMA/队列释放不会被
 * 扫描线程踩着。等待上限 2.5 s，超时也放行——扫描自己会报 NO_MEM，日志能看出是拨号中撞上的。
 * Only the policy task may bring the modem down (its stop/start cannot run concurrently), so this
 * wakes that task and waits for the link to actually drop; doing it on one thread also keeps the
 * DMA/queue teardown from being raced by the scanning thread. The wait is capped at 2.5 s and then
 * proceeds: the scan will report NO_MEM itself, and the log shows it collided with a dial.
 * The deadline is only a safety net; cellular_policy_resume() normally releases it first. */
void cellular_policy_pause(uint32_t ms) {
    const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_paused_until_s = now_s + (ms ? (ms + 999u) / 1000u : 1u);
    if (s_policy_task) xTaskNotifyGive(s_policy_task);
    for (int i = 0; i < 25 && s_cellular_ready; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    s_last_dial_ms = 0;
}

void cellular_policy_resume(void) {
    if (!s_paused_until_s) return;
    s_paused_until_s = 0;
    s_last_dial_ms = 0;
    if (s_policy_task) xTaskNotifyGive(s_policy_task);
    ESP_LOGI(TAG, "4G pause released");
}

static bool wifi_online(void) {
    read_pico_transfer_status_t st;
    read_pico_transfer_get_status(&st);
    return st.network_ready && st.mode == READ_PICO_TRANSFER_MODE_STA;
}

static void policy_task(void *arg) {
    (void)arg;
    s_boot_ms = esp_timer_get_time() / 1000;

    ESP_LOGW(TAG, "policy task running");
    /* Bring up the driver instance up front (no power, no dialing). */
    cellular_modem_init_default();
    ESP_LOGW(TAG, "modem instance initialized");

    for (;;) {
        /* 平时按 3 s 轮询；被通知（暂停/恢复/换卡）就立刻醒来，省掉最多 3 s 的空等——
         * Wi-Fi 扫描前那 2.5 s 等待原本会白白耗在轮询间隔上。
         * Poll every 3 s, but wake immediately on a notify (pause/resume/slot change) so the Wi-Fi
         * scan does not spend its budget waiting out the polling interval. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLICY_PERIOD_MS));
        const int64_t now = esp_timer_get_time() / 1000;
        const bool enabled = app_settings_cellular_enabled();
        const bool wifi = wifi_online();

        /* 扫描 Wi-Fi 期间让路：这里负责把链路真的拆掉（cellular_policy_pause 正等着这一步）。
         * Standing aside for a Wi-Fi scan: this is where the link actually goes down, which is the
         * step cellular_policy_pause() is waiting on. */
        if (s_paused_until_s) {
            const uint32_t now_s = (uint32_t)(now / 1000);
            if (now_s >= s_paused_until_s) {
                s_paused_until_s = 0;
                s_last_dial_ms = 0;
                ESP_LOGI(TAG, "4G pause expired -> resuming policy");
            } else {
                if (s_cellular_ready) {
                    ESP_LOGW(TAG, "pause for Wi-Fi scan -> stopping 4G");
                    cellular_modem_stop();
                    s_cellular_ready = false;
                }
                continue;
            }
        }

        /* Slot change landed while a dial was in flight: wait for it to finish first. */
        if (s_restart_requested && !s_dialing) {
            ESP_LOGW(TAG, "restart requested -> cycling modem for the new SIM slot");
            cellular_modem_stop();
            s_cellular_ready = false;
            s_restart_requested = false;
            s_last_dial_ms = 0;
            s_boot_ms = now - WIFI_GRACE_MS - 1000;  /* allow an immediate dial */
            continue;
        }

        /* Switch OFF: tear down any live link and never dial. WeRead sees no
         * cellular and falls back to prompting for Wi-Fi (see app_weread.c). */
        if (!enabled) {
            if (s_cellular_ready) {
                ESP_LOGI(TAG, "4G disabled in settings -> stopping cellular");
                cellular_modem_stop();
                s_cellular_ready = false;
            }
            continue;
        }

        if (wifi) {
            if (s_cellular_ready) {
                ESP_LOGI(TAG, "Wi-Fi available -> stopping cellular");
                cellular_modem_stop();
                s_cellular_ready = false;
            }
            continue;
        }

        /* No Wi-Fi: dial 4G so the reader stays online. We do not require a
         * logged-in account here -- first-time QR login itself needs network. */
        if (s_cellular_ready) {
            s_cellular_ready = cellular_modem_is_ready();
            if (!s_cellular_ready) ESP_LOGW(TAG, "cellular link lost");
            continue;
        }
        if (s_dialing) continue;
        if (now - s_boot_ms < WIFI_GRACE_MS) continue;              /* early boot grace */
        if (now - s_last_dial_ms < DIAL_RETRY_MIN_MS) continue;     /* retry throttle */

        s_last_dial_ms = now;
        s_dialing = true;
        /* 每次拨号前把当前卡槽选择推给模组驱动，切槽就发生在 start() 的初始化序列里。 */
        cellular_modem_set_sim_mode((int)app_settings_sim_slot());
        ESP_LOGW(TAG, "No Wi-Fi -> dialing 4G (sim=%d)", (int)app_settings_sim_slot());
        esp_err_t err = cellular_modem_start(MODEM_DIAL_TIMEOUT_MS);
        s_dialing = false;
        s_cellular_ready = (err == ESP_OK) && cellular_modem_is_ready();
        ESP_LOGW(TAG, "4G dial result: %s (0x%x)", s_cellular_ready ? "READY" : "FAILED", (int)err);
    }
}

esp_err_t cellular_policy_start(void) {
    static bool started;
    if (started) return ESP_OK;
    started = true;
    s_boot_ms = esp_timer_get_time() / 1000;
    TaskHandle_t task = NULL;
    if (xTaskCreate(policy_task, "cell_policy", 4096, NULL, 2, &task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create policy task");
        return ESP_FAIL;
    }
    s_policy_task = task;   /* pause/resume 靠它立刻叫醒循环 */
    ESP_LOGI(TAG, "policy task started");
    return ESP_OK;
}
