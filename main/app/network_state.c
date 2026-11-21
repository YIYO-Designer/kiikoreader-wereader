/* SPDX-License-Identifier: Apache-2.0 */
#include "network_state.h"
#include "cellular_policy.h"
#include "read_pico_transfer.h"
#include "settings.h"

bool app_network_wifi_ready(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    return status.network_ready && status.mode == READ_PICO_TRANSFER_MODE_STA;
}

bool app_network_ready(void) {
    return app_network_wifi_ready() || cellular_ready();
}

bool app_network_cellular_enabled(void) {
#if KIIKO_BOARD_METALIO47
    return cellular_available() && app_settings_cellular_enabled();
#else
    return false;
#endif
}

const char *app_network_offline_text(void) {
    return app_network_cellular_enabled()
        ? "网络未连接，请检查 4G 或连接 Wi-Fi" : "网络未连接，请连接 Wi-Fi";
}

const char *app_network_wait_text(void) {
    return app_network_cellular_enabled() && !app_network_wifi_ready()
        ? "正在连接 4G 网络…" : "正在连接 Wi-Fi…";
}

const char *app_network_failure_text(void) {
    if (app_network_wifi_ready()) return "网络访问失败，请检查 Wi-Fi 或稍后重试";
    if (cellular_ready()) return "4G 网络访问失败，请检查 SIM 网络或改用 Wi-Fi";
    return app_network_offline_text();
}
