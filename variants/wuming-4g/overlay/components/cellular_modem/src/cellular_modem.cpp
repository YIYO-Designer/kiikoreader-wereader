/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * C facade over the C++ UartEthModem driver. Compiled as C++ so it can
 * instantiate the modem class; exposes a pure C ABI to the rest of the
 * (C-based) firmware.
 */
#include "cellular_modem.h"

#include <string>
#include <atomic>
#include <mutex>

#include "uart_eth_modem.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"

static const char *TAG = "cellular_modem";

/* Default S31 / NT26 wiring (matches official Metalio E-INK4 Plus config.h). */
#define CELLULAR_UART_NUM   (UART_NUM_2)
/* 3 Mbaud：数据面走的是这条 UART，2 Mbaud 只有 ~200 KB/s 的硬上限，实测 4G 大文件已经
 * 顶到 140 KB/s。模块与官方驱动都支持 3 Mbaud，初始化失败会自动退回 2 Mbaud。
 * The data plane rides this UART: 2 Mbaud caps out near 200 KB/s and measured 4G transfers
 * already reach 140 KB/s. 3 Mbaud is supported by the module; init falls back if it fails. */
#define CELLULAR_BAUD       (3000000)
#define CELLULAR_TX_PIN     (GPIO_NUM_58)
#define CELLULAR_RX_PIN     (GPIO_NUM_59)
#define CELLULAR_MRDY_PIN   (GPIO_NUM_60)
#define CELLULAR_SRDY_PIN   (GPIO_NUM_3)

static std::mutex s_mu;
static UartEthModem *s_modem = nullptr;
static std::atomic<bool> s_ready{false};
static std::atomic<bool> s_busy{false};
static cellular_modem_event_cb s_user_cb = nullptr;
static cellular_modem_info_t s_info = {};

static cellular_modem_event_t map_event(UartEthModem::UartEthModemEvent e) {
    using E = UartEthModem::UartEthModemEvent;
    switch (e) {
        case E::Connecting:             return CELLULAR_MODEM_EVENT_CONNECTING;
        case E::Connected:              return CELLULAR_MODEM_EVENT_CONNECTED;
        case E::Disconnected:           return CELLULAR_MODEM_EVENT_DISCONNECTED;
        case E::ErrorNoSim:             return CELLULAR_MODEM_EVENT_NO_SIM;
        case E::ErrorRegistrationDenied:return CELLULAR_MODEM_EVENT_REG_DENIED;
        case E::ErrorInitFailed:        return CELLULAR_MODEM_EVENT_INIT_FAILED;
        default:                        return CELLULAR_MODEM_EVENT_INIT_FAILED;
    }
}

static void on_modem_event(UartEthModem::UartEthModemEvent e, const std::string &detail) {
    switch (e) {
        case UartEthModem::UartEthModemEvent::Connecting:
            s_busy.store(true);
            s_ready.store(false);
            break;
        case UartEthModem::UartEthModemEvent::Connected:
            s_busy.store(false);
            s_ready.store(true);
            break;
        case UartEthModem::UartEthModemEvent::Disconnected:
            s_ready.store(false);
            break;
        default:
            s_busy.store(false);
            s_ready.store(false);
            break;
    }
    cellular_modem_event_cb cb = s_user_cb;
    if (cb) {
        cellular_modem_event_t mapped = map_event(e);
        cb(mapped, detail.c_str());
    }
}

static void refresh_info(UartEthModem *modem) {
    if (!modem) return;
    cellular_modem_info_t info = {};
    std::string v;
    v = modem->GetImei();       strlcpy(info.imei,   v.c_str(), sizeof(info.imei));
    v = modem->GetIccid();      strlcpy(info.iccid,  v.c_str(), sizeof(info.iccid));
    v = modem->GetImsi();       strlcpy(info.imsi,   v.c_str(), sizeof(info.imsi));
    v = modem->GetCarrierName();strlcpy(info.carrier,v.c_str(), sizeof(info.carrier));
    info.csq = modem->GetSignalStrength();
    s_info = info;
}

extern "C" esp_err_t cellular_modem_init(int uart_num, int baud_rate,
                                         int tx_pin, int rx_pin, int mrdy_pin, int srdy_pin) {
    std::lock_guard<std::mutex> lock(s_mu);
    if (s_modem) return ESP_OK;

    /* Prerequisites normally done by the official Board ctor. ESP-IDF APIs are
     * idempotent-ish here: repeated init returns ESP_ERR_INVALID_STATE which we
     * intentionally ignore because the firmware may already have them up. */
    (void)esp_netif_init();
    (void)esp_event_loop_create_default();
    (void)gpio_install_isr_service(0);

    UartEthModem::Config cfg = {};
    cfg.uart_num   = static_cast<uart_port_t>(uart_num);
    cfg.baud_rate  = baud_rate;
    cfg.tx_pin     = static_cast<gpio_num_t>(tx_pin);
    cfg.rx_pin     = static_cast<gpio_num_t>(rx_pin);
    cfg.mrdy_pin   = static_cast<gpio_num_t>(mrdy_pin);
    cfg.srdy_pin   = static_cast<gpio_num_t>(srdy_pin);

    UartEthModem *modem = new (std::nothrow) UartEthModem(cfg);
    if (!modem) {
        ESP_LOGE(TAG, "failed to allocate UartEthModem");
        return ESP_ERR_NO_MEM;
    }
    modem->SetNetworkEventCallback(on_modem_event);
    s_modem = modem;
    ESP_LOGW(TAG, "modem instance created (uart=%d baud=%d tx=%d rx=%d mrdy=%d srdy=%d)",
             uart_num, baud_rate, tx_pin, rx_pin, mrdy_pin, srdy_pin);
    return ESP_OK;
}

extern "C" esp_err_t cellular_modem_init_default(void) {
    return cellular_modem_init(CELLULAR_UART_NUM, CELLULAR_BAUD,
                               CELLULAR_TX_PIN, CELLULAR_RX_PIN,
                               CELLULAR_MRDY_PIN, CELLULAR_SRDY_PIN);
}

extern "C" esp_err_t cellular_modem_start(uint32_t timeout_ms) {
    UartEthModem *modem;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        modem = s_modem;
    }
    if (!modem) return ESP_ERR_INVALID_STATE;

    s_busy.store(true);
    s_ready.store(false);
    esp_err_t err = modem->Start();
    if (err != ESP_OK) {
        s_busy.store(false);
        ESP_LOGE(TAG, "modem Start failed: 0x%x", (int)err);
        return err;
    }
    /* Start() returns after handshake; IP may arrive slightly later. Poll ready. */
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (!s_ready.load() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    refresh_info(modem);
    if (!s_ready.load()) {
        s_busy.store(false);
        ESP_LOGW(TAG, "modem started but no IP within %u ms", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGW(TAG, "CELLULAR NETWORK READY imei=%s carrier=%s csq=%d",
             s_info.imei, s_info.carrier, s_info.csq);
    return ESP_OK;
}

extern "C" void cellular_modem_stop(void) {
    UartEthModem *modem;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        modem = s_modem;
    }
    if (!modem) return;
    modem->Stop();
    s_ready.store(false);
    s_busy.store(false);
}

extern "C" bool cellular_modem_is_ready(void) { return s_ready.load(); }
extern "C" bool cellular_modem_is_busy(void)  { return s_busy.load(); }

extern "C" void cellular_modem_set_event_cb(cellular_modem_event_cb cb) {
    s_user_cb = cb;
}

extern "C" bool cellular_modem_get_info(cellular_modem_info_t *out) {
    if (!out) return false;
    UartEthModem *modem;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        modem = s_modem;
    }
    if (!modem || !modem->IsInitialized()) return false;
    refresh_info(modem);
    *out = s_info;
    return true;
}
