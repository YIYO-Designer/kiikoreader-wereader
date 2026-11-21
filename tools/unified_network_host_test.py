#!/usr/bin/env python3
"""Exercise real network gates, visible settings mapping and boot policy with fake hardware."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
from home_cover_cache_host_test import function

ROOT = Path(__file__).resolve().parents[1]
COMMON = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define pdPASS 1
typedef void* TaskHandle_t;
enum { READ_PICO_TRANSFER_MODE_AP, READ_PICO_TRANSFER_MODE_STA };
typedef struct { bool network_ready; int mode; } read_pico_transfer_status_t;
static bool wifi, cell, enabled;
static int mode = READ_PICO_TRANSFER_MODE_STA;
static void read_pico_transfer_get_status(read_pico_transfer_status_t *s) {
    *s=(read_pico_transfer_status_t){wifi,mode};
}
static bool app_settings_cellular_enabled(void) { return enabled; }
static int app_settings_sim_slot(void) { return 2; }
'''

NETWORK = r'''
static bool present;
static bool cellular_available(void) { return present; }
static bool cellular_ready(void) { return present && cell; }
#include "app/network_state.c"
int main(void) {
    for (int p=0;p<2;p++) for (int e=0;e<2;e++)
    for (int w=0;w<2;w++) for (int c=0;c<2;c++) for (int ap=0;ap<2;ap++) {
        present=p;enabled=e;wifi=w;cell=c;
        mode=ap?READ_PICO_TRANSFER_MODE_AP:READ_PICO_TRANSFER_MODE_STA;
        assert(app_network_wifi_ready()==(w&&!ap));
        assert(app_network_ready()==((w&&!ap)||(p&&c)));
        assert(app_network_cellular_enabled()==(KIIKO_BOARD_METALIO47&&p&&e));
        bool mentions_cell=strstr(app_network_offline_text(),"4G")!=NULL;
        assert(mentions_cell==app_network_cellular_enabled());
        if (!present) {
            assert(!strstr(app_network_wait_text(),"4G"));
            assert(!strstr(app_network_failure_text(),"4G"));
        }
        if (app_network_wifi_ready()) assert(!strstr(app_network_failure_text(),"4G"));
    }
    puts("PASS: network capability/switch/link/AP matrix and Wi-Fi-only prompts");
}
'''

POLICY = r'''
static int scenario, probes, deinits, starts, stops, iterations, notifications;
static bool create_ok=true, fail_dial;
static int64_t now_us;
static jmp_buf finished;
static void (*task_fn)(void*);
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(unsigned ms) { now_us+=(int64_t)ms*1000; }
static void vTaskDelete(void *unused) { (void)unused;longjmp(finished,1); }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void*)1; }
static int xTaskCreate(void(*fn)(void*),const char*n,unsigned stack,void*a,int p,void*h) {
    (void)n;(void)stack;(void)a;(void)p;(void)h;task_fn=fn;return create_ok;
}
static void xTaskNotifyGive(TaskHandle_t h) { assert(h==(void*)1);++notifications; }
static unsigned ulTaskNotifyTake(int clear,unsigned ms);
static esp_err_t cellular_modem_init_default(void) { return scenario==5?ESP_ERR_NO_MEM:ESP_OK; }
static esp_err_t cellular_modem_probe(void) { ++probes;return scenario==0?ESP_FAIL:ESP_OK; }
static void cellular_modem_cancel(void) {}
static void cellular_modem_deinit(void) { ++deinits;cell=false; }
static bool cellular_modem_is_ready(void) { return cell; }
static void cellular_modem_stop(void) { ++stops;cell=false; }
static void cellular_modem_set_sim_mode(int slot) { assert(slot==2); }
static esp_err_t cellular_modem_start(uint32_t ms) {
    assert(ms==60000);++starts;cell=!fail_dial;return fail_dial?ESP_ERR_TIMEOUT:ESP_OK;
}
#include "app/cellular_policy.c"
static unsigned ulTaskNotifyTake(int clear,unsigned ms) {
    (void)clear;now_us+=(int64_t)ms*1000;
    ++iterations;
    if (iterations==8 && scenario==4) {
        assert(cellular_available() && cellular_ready());
        wifi=true;
    }
    if (iterations>12) longjmp(finished,1);
    return 0;
}
int main(int argc,char **argv) {
    assert(argc==2);scenario=atoi(argv[1]);enabled=scenario!=1;fail_dial=scenario==3;
    if (scenario==6) {
        create_ok=false;assert(cellular_policy_start()==ESP_ERR_NO_MEM);
        create_ok=true;
    }
    assert(cellular_policy_start()==ESP_OK);
    assert(cellular_policy_start()==ESP_OK);
    if (!setjmp(finished)) task_fn(NULL);
    if (scenario==0 || scenario==5) {
        assert(!cellular_available() && !starts && deinits==1);
        assert(cellular_capability()==CELLULAR_CAPABILITY_NOT_DETECTED);
        assert(probes==(scenario==0?1:0));
    } else {
        assert(cellular_available() && probes==1 && !deinits);
        if (scenario==1) assert(!starts && !cellular_ready());
        else if (scenario==3) assert(starts==1 && !cellular_ready());
        else if (scenario==4) assert(starts==1 && !cellular_ready() && stops);
        else assert(starts==1 && cellular_ready());
        cellular_policy_request_restart();assert(notifications==1);
    }
    printf("PASS: real boot policy scenario %d\n",scenario);
}
'''


def run():
    cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
    if not cc:
        raise RuntimeError('Host C compiler required; this test also runs in GitHub Actions')
    with tempfile.TemporaryDirectory(prefix='kiiko-network-') as folder:
        folder = Path(folder)
        stubs = folder / 'stubs'
        stubs.mkdir()
        (stubs / 'fixture.h').write_text(COMMON)
        for name in ('esp_err.h', 'settings.h', 'read_pico_transfer.h', 'cellular_modem.h',
                     'esp_log.h', 'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/task.h'):
            path = stubs / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "fixture.h"\n')
        # Header resolves normally; the fake cellular implementation comes from NETWORK.
        (stubs / 'cellular_policy.h').write_text('#include "fixture.h"\n')

        def compile_case(name, text, board=1):
            source = folder / (name + '.c')
            source.write_text('#include "fixture.h"\n' + text, encoding='utf-8')
            binary = folder / name
            subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-function',
                            '-Wno-unused-variable', '-Werror', f'-DKIIKO_BOARD_METALIO47={board}',
                            '-I', str(stubs), '-I', str(ROOT / 'main'),
                            str(source), '-o', str(binary)], check=True)
            return binary

        for board in (0, 1):
            binary = compile_case(f'network-{board}', NETWORK, board)
            subprocess.run([str(binary)], check=True)

        binary = compile_case('policy', POLICY)
        for scenario in range(7):
            subprocess.run([str(binary), str(scenario)], check=True)

        settings = ROOT / 'main/apps/app_device_settings.c'
        fixture = '''
typedef enum { CONNECTION_WIFI, CONNECTION_CELLULAR, CONNECTION_SIM,
               CONNECTION_BLUETOOTH, CONNECTION_TIME } connection_item_t;
static bool s_main_cellular;
'''
        fixture += function('settings_connection_count', settings)
        fixture += function('settings_connection_item', settings)
        fixture += '''
int main(void) {
    for (int p=0;p<2;p++) {
        s_main_cellular=p;
        assert(settings_connection_count()==(p?5:3));
        assert(settings_connection_item(0)==CONNECTION_WIFI);
        assert(settings_connection_item(settings_connection_count()-2)==CONNECTION_BLUETOOTH);
        assert(settings_connection_item(settings_connection_count()-1)==CONNECTION_TIME);
        if (p) {
            assert(settings_connection_item(1)==CONNECTION_CELLULAR);
            assert(settings_connection_item(2)==CONNECTION_SIM);
        }
    }
    puts("PASS: actual settings row mapping, both capability layouts");
}
'''
        subprocess.run([str(compile_case('settings', fixture))], check=True)


if __name__ == '__main__':
    run()
