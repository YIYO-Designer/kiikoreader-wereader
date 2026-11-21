/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

/* Shared Internet-facing state; an AP used for local file transfer is not
 * considered an Internet connection. Capability differs from enabled/online. */
bool app_network_wifi_ready(void);
bool app_network_ready(void);
bool app_network_cellular_enabled(void);
const char *app_network_offline_text(void);
const char *app_network_wait_text(void);
const char *app_network_failure_text(void);
