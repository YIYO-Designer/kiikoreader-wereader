/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：按Metalio公开网络文档统一S31运行期策略；不存取网络凭据。
 * English: Apply the documented Metalio S31 runtime profile without touching credentials.
 * Reference: CloudZao/Metalio-E-INK4-Plus@ad8edf4 docs/ap-file-transfer.md (MIT).
 */
#pragma once
#include <stdbool.h>
#include "esp_wifi.h"
#define TRANSFER_S31_MAX_TX_POWER 48
// Short STA sessions (network scan, time sync and low-RAM connection starts) do
// not need an AP receive aggregation window. Keep their DMA-backed RX footprint
// small enough for the S31's 64 KiB internal allocation reserve.
static inline void transfer_s31_constrained_sta_config(wifi_init_config_t *init) {
    if (!init) return;
    if (init->static_rx_buf_num > 3) init->static_rx_buf_num = 3;
    if (init->dynamic_rx_buf_num > 16) init->dynamic_rx_buf_num = 16;
    init->ampdu_rx_enable = false;
}

// 微信读书缓存的主用档，也是实测最快的一档：15.37 MB 章节资源包跑到 351.6 KB/s，已经
// 超过 Mac 跑同一 URL 的 79~91 KB/s，链路吃满，再调射频参数没有空间了。
// 关键只有两条：动态 RX 抬到 64 槽（惰性分配、优先走 PSRAM，初始化不占内部 RAM），
// 让接收端在缓冲周转时有回旋余量；静态 TX 压到 3 槽，因为驱动把静态 TX 池当一整块连续
// DMA 申请，6 槽会先吃掉最大的块，剩下的不够再建静态 RX，直接 "Expected to init 3 rx
// buffer, actual is 0" ESP_ERR_NO_MEM。缓存会话纯下行，用不到 6 个 TX 槽。
// AMPDU 与 6 槽静态 RX 实测是退步（tar 277.2 KB/s、端到端 173.4s，对 351.6 KB/s / 139.1s），
// 已删掉：动态 RX 64 槽才是那个杠杆，聚合窗口只把重排序缓冲压得更紧。
// The primary WeRead cache profile and the fastest measured one: 15.37 MB of chapter tarballs
// at 351.6 KB/s, above what a Mac reaches on the same URL (79~91 KB/s), so the link is already
// saturated. Two settings matter: 64 dynamic RX slots (lazily allocated from PSRAM, so init
// costs no internal RAM) give the receiver headroom, and 3 static TX slots fit because the
// driver requests the static TX pool as one contiguous DMA block. AMPDU plus 6 static RX slots
// measured as a regression (277.2 KB/s, 173.4s end to end) and was removed.
static inline void transfer_s31_thrifty_sta_config(wifi_init_config_t *init) {
    if (!init) return;
    if (init->static_rx_buf_num > 3) init->static_rx_buf_num = 3;
    if (init->static_tx_buf_num > 3) init->static_tx_buf_num = 3;
    if (init->dynamic_rx_buf_num < 64) init->dynamic_rx_buf_num = 64;
    init->ampdu_rx_enable = false;
}

static inline esp_err_t transfer_s31_radio_profile(wifi_interface_t interface) {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) err = esp_wifi_set_protocol(interface,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if (err == ESP_OK) err = esp_wifi_set_max_tx_power(TRANSFER_S31_MAX_TX_POWER);
    return err;
}

// SDK: AMPDU接收池应覆盖BA窗口；只给独占传书补齐，最大16槽，日常STA/BLE不增占用。
// SDK: cover the BA window with static RX buffers; bulk-only, capped at16, retaining normal STA/BLE memory.
static inline void transfer_s31_bulk_receive_config(wifi_init_config_t *init, bool bulk) {
    if (!init || !bulk || !init->ampdu_rx_enable || init->rx_ba_win <= 0 ||
        init->rx_ba_win > 16 ||
        (init->dynamic_rx_buf_num && init->dynamic_rx_buf_num < init->rx_ba_win)) return;
    int window=init->rx_ba_win;
#ifdef CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    // 独占传书统一到SDK建议的16槽；原静态池已经够大也不能跳过BA设置。
    // Bulk PSRAM reception uses the SDK's16-slot recommendation even when the static pool was already large.
    if (!init->dynamic_rx_buf_num || init->dynamic_rx_buf_num >= 16)window=16;
#endif
    init->rx_ba_win=window;
    if(init->static_rx_buf_num<window)init->static_rx_buf_num=window;
}
