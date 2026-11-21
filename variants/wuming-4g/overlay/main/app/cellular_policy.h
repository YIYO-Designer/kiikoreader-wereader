/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network policy: prefer Wi-Fi, fall back to 4G (cellular) when WeRead needs
 * network but no Wi-Fi STA is available. Exposes cellular_ready() used by the
 * WeRead online gate so the whole HTTP stack transparently uses whichever
 * link is up.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the background policy task. Safe to call once, after wr_init(). */
esp_err_t cellular_policy_start(void);

/* True when the cellular link currently has a usable IP. */
bool cellular_ready(void);

/* True while the policy is dialing / modem is registering. */
bool cellular_connecting(void);

/* 换了 SIM 卡槽后调用：策略任务会拆链并按新卡槽重新拨号。
 * Ask the policy task to cycle the modem so a new SIM slot takes effect. */
void cellular_policy_request_restart(void);

/*
 * 扫描 Wi-Fi 邻居前先让 4G 让路：两个射频同时上电既抢电流，也抢内部 RAM——实测 4G 在线时内部
 * 空闲从 65 KB 掉到 47 KB，而 Wi-Fi 驱动最少要 63 KB，扫描必然 NO_MEM。暂停会同步拆掉现有链路
 * 并禁止拨号；ms 只是兜底，正常由 cellular_policy_resume() 提前恢复。
 * Step the 4G link aside before a Wi-Fi scan: two radios at once fight for current and for internal
 * RAM. Measured, having 4G up drops internal free from 65 KB to 47 KB while the Wi-Fi driver needs
 * 63 KB, so the scan can only fail. Pausing tears the live link down and blocks dialing; ms is only
 * a safety net, normal callers release it with cellular_policy_resume().
 */
void cellular_policy_pause(uint32_t ms);
void cellular_policy_resume(void);

#ifdef __cplusplus
}
#endif
