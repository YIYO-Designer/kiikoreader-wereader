/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * C facade over the C++ UartEthModem driver.
 * The 4G module (NT26 / EC801E Cat.1) is attached on UART with MRDY/SRDY
 * flow control and exposed to lwIP as a standard ethernet netif via iot_eth.
 * Once ready, all esp_http_client traffic transparently uses the cellular
 * link -- no changes needed in the WeRead HTTP layer.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CELLULAR_MODEM_EVENT_CONNECTING = 0,
    CELLULAR_MODEM_EVENT_CONNECTED,
    CELLULAR_MODEM_EVENT_DISCONNECTED,
    CELLULAR_MODEM_EVENT_NO_SIM,
    CELLULAR_MODEM_EVENT_REG_DENIED,
    CELLULAR_MODEM_EVENT_INIT_FAILED,
} cellular_modem_event_t;

typedef void (*cellular_modem_event_cb)(cellular_modem_event_t event, const char *detail);

typedef struct {
    char imei[24];
    char iccid[24];
    char imsi[24];
    char carrier[32];
    int  csq;          /* 0..31, 99 = unknown */
} cellular_modem_info_t;

/* Configure pins + baud and create the driver instance. Safe to call once. */
esp_err_t cellular_modem_init(int uart_num, int baud_rate,
                              int tx_pin, int rx_pin, int mrdy_pin, int srdy_pin);

/* Power the module on, run AT init (SIM/registration) and bring up the netif.
 * Blocks until IP_EVENT_ETH_GOT_IP or timeout_ms. */
esp_err_t cellular_modem_start(uint32_t timeout_ms);

/* Stop the modem and release netif/tasks. Idempotent. */
void cellular_modem_stop(void);
/* Signal cancellation; the owning policy task performs resource teardown. */
void cellular_modem_cancel(void);

/* True once the cellular netif has a usable IP address. */
bool cellular_modem_is_ready(void);

/* True while modem init/registration is in progress. */
bool cellular_modem_is_busy(void);

/* Optional event callback for UI / diagnostics. */
void cellular_modem_set_event_cb(cellular_modem_event_cb cb);

/* 双卡选择：0 = 自动（卡托优先），1 = 只用卡托卡，2 = 只用内置物联网卡。
 * 下次 cellular_modem_start() 生效；已拨号时需要先 stop 再 start。
 * Dual SIM preference for the next start(); stop the link first to apply immediately. */
void cellular_modem_set_sim_mode(int mode);
int  cellular_modem_get_sim_mode(void);

/* Cached modem identity; returns false if modem not instantiated. */
bool cellular_modem_get_info(cellular_modem_info_t *out);

/* Convenience: default S31 pins (TX58 RX59 MRDY60 SRDY3, UART2 @2Mbps). */
esp_err_t cellular_modem_init_default(void);

/* Framed AT-only probe. No SIM required; releases transport resources.
 * ESP_ERR_NOT_FINISHED means cancelled, not evidence of absent hardware. */
esp_err_t cellular_modem_probe(void);
void cellular_modem_deinit(void);

#ifdef __cplusplus
}
#endif
