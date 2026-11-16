#include "weread_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "book_cover.h"
#include "book_epub.h"
#include "book_progress.h"
#include "book_title.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "read_pico_sd.h"
#include "read_pico_sd_access.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "zlib.h"

#define WR_MAX_SHELF 8192
#define WR_INDEX WR_DIR "/shelf.bin"
#define WR_INDEX_TEMP WR_DIR "/shelf.part"
#define WR_CACHED WR_DIR "/cached.bin"
#define WR_SD_SCOPE(path) __attribute__((cleanup(read_pico_sd_access_scope_end))) \
    read_pico_sd_access_scope_t scope = read_pico_sd_access_scope(path)

typedef struct { char owner[WR_ID], token[384]; uint32_t library; } wr_account_t;
typedef struct { uint32_t day, today; uint64_t total; } wr_stats_t;
typedef struct { char owner[WR_ID]; uint32_t count; } wr_shelf_header_t;
typedef struct {
    wr_job_t kind;
    wr_book_t book;
    unsigned first, count;
} wr_work_t;
typedef struct {
    char book_id[WR_ID], owner[WR_ID];
    uint32_t library;
    unsigned seconds;
    unsigned failures;
    int64_t retry_at;
} wr_report_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
enum { WR_WORKER_STACK_BYTES = 16 * 1024, WR_HTTP_URL_BYTES = 2080 };
// 缓存和上报都会写闪存，任务栈必须位于内部 RAM。/ Both cache and report write flash, so the task stack must reside in internal RAM.
static DRAM_ATTR StackType_t s_worker_stack[WR_WORKER_STACK_BYTES / sizeof(StackType_t)];
static DRAM_ATTR StaticTask_t s_worker_tcb;
static TaskHandle_t s_worker;
static wr_work_t s_work;
static wr_report_t s_reports[8];
static bool s_work_pending;
static bool s_processing_report;
static bool s_reader_scanning;
static int64_t s_reader_scan_end;
static struct {
    wr_report_t account_book;
    char chapter[WR_ID];
    wr_reader_t fields;
    int64_t expires;
} s_report_fields;
// Each HTTP request owns redirect and cookie-capture state, so bounded cache
// prefetch requests can overlap without corrupting one another.
typedef struct {
    char absolute[WR_HTTP_URL_BYTES];
    char redirected[WR_HTTP_URL_BYTES];
    char location[WR_HTTP_URL_BYTES];
    char current[WR_HTTP_URL_BYTES];
    char cookie_snapshot[1024];
    bool capture_service_cookies;
    /// 本次请求是否新建了 TCP/TLS 连接；复用池中连接时保持 false。
    /// Whether this request opened a new TCP/TLS connection; stays false on a pooled one.
    bool connected;
} wr_http_context_t;
static StaticSemaphore_t s_cookie_mutex_storage;
static SemaphoreHandle_t s_cookie_mutex;
static wr_result_t s_result;
static wr_job_t s_active_job;
static atomic_bool s_cancel;
static unsigned s_done, s_total;
static char s_qr[180];
static wr_account_t s_http_account;
static char s_cookies[1024];
static _Atomic bool s_expired;
static _Atomic bool s_http_open_failed;
static _Atomic(const char *) s_error_message = "操作失败，请检查网络后重试";
static const char *s_result_message = "操作失败，请检查网络后重试";
/* 阅读会话归属于开书时的账号：统计与上报都用这份快照，切号后不会写到新账号。
 * A reading session belongs to the account that opened the book; reports and stats use
 * this snapshot so a later account switch can never bill the new account. */
static struct {
    wr_book_t book;
    char owner[WR_ID];
    uint32_t library;
    int64_t last;
    uint32_t ms;
    bool attached;
} s_reading;
static bool s_radio_held;

static void restore_session_cookie(void);
static bool wr_url_allowed(const char *url, bool service);
static bool cover_redirect_url(const char *current, const char *location,
                               char out[WR_HTTP_URL_BYTES]);

const char *wr_error_message(void) { return atomic_load(&s_error_message); }
void wr_error_set(const char *message) { if (message && *message) atomic_store(&s_error_message, message); }
const char *wr_result_message(void) {
    portENTER_CRITICAL(&s_lock);
    const char *message = s_result_message;
    portEXIT_CRITICAL(&s_lock);
    return message;
}

static bool read_account(wr_account_t *account) {
    memset(account, 0, sizeof(*account));
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = sizeof(*account);
    esp_err_t result = nvs_get_blob(nvs, "account", account, &size);
    nvs_close(nvs);
    return result == ESP_OK && size == sizeof(*account) &&
        memchr(account->owner, 0, sizeof(account->owner)) && memchr(account->token, 0, sizeof(account->token));
}

static bool save_account(const wr_account_t *account) {
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t result = nvs_set_blob(nvs, "account", account, sizeof(*account));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}

static void save_session_cookies(void) {
    if (!s_cookies[0]) return;
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_str(nvs, "cookies", s_cookies) == ESP_OK) (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static void cookie_lock(void) {
    if (s_cookie_mutex) (void)xSemaphoreTake(s_cookie_mutex, portMAX_DELAY);
}

static void cookie_unlock(void) {
    if (s_cookie_mutex) (void)xSemaphoreGive(s_cookie_mutex);
}

static bool clear_session_cookies(void) {
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t result = nvs_erase_key(nvs, "cookies");
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    memset(s_cookies, 0, sizeof(s_cookies));
    return result == ESP_OK;
}

static bool session_cookie_has(const char *name) {
    const size_t length = strlen(name);
    for (const char *item = s_cookies; item && *item;) {
        while (*item == ' ') ++item;
        const char *end = strstr(item, "; ");
        if (!end) end = item + strlen(item);
        if ((size_t)(end - item) > length && item[length] == '=' && !memcmp(item, name, length)) return true;
        item = *end ? end + 2 : end;
    }
    return false;
}

static bool session_cookie_copy(const char *name, char *out, size_t size) {
    if (!out || !size) return false;
    out[0] = 0;
    const size_t length = strlen(name);
    for (const char *item = s_cookies; item && *item;) {
        while (*item == ' ') ++item;
        const char *end = strstr(item, "; ");
        if (!end) end = item + strlen(item);
        if ((size_t)(end - item) > length && item[length] == '=' && !memcmp(item, name, length)) {
            size_t value_length = (size_t)(end - item) - length - 1;
            if (!value_length || value_length >= size) return false;
            memcpy(out, item + length + 1, value_length);
            out[value_length] = 0;
            return true;
        }
        item = *end ? end + 2 : end;
    }
    return false;
}

static void stats_key(const char *owner, char key[15]) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *owner; ++owner) hash = (hash ^ (unsigned char)*owner) * UINT64_C(1099511628211);
    snprintf(key, 15, "s%013llx", (unsigned long long)(hash & UINT64_C(0xfffffffffffff)));
}

static uint32_t current_day(void) {
    time_t now = time(NULL);
    return now >= 1704067200 ? (uint32_t)((now + 8 * 3600) / 86400) : 0;
}

static wr_stats_t read_stats(nvs_handle_t nvs, const char *key) {
    wr_stats_t stats = {0};
    size_t size = sizeof(stats);
    if (nvs_get_blob(nvs, key, &stats, &size) != ESP_OK || size != sizeof(stats)) stats = (wr_stats_t){0};
    if (stats.day != current_day()) { stats.day = current_day(); stats.today = 0; }
    return stats;
}

bool wr_account(char id[WR_ID], uint32_t *today, uint64_t *total) {
    wr_account_t account;
    const bool logged_in = read_account(&account) && account.token[0];
    if (id) snprintf(id, WR_ID, "%s", account.owner);
    wr_stats_t stats = {0};
    nvs_handle_t nvs;
    if (account.owner[0] && (today || total) && nvs_open("weread", NVS_READONLY, &nvs) == ESP_OK) {
        char key[15];
        stats_key(account.owner, key);
        stats = read_stats(nvs, key);
        nvs_close(nvs);
    }
    if (today) *today = stats.today;
    if (total) *total = stats.total;
    return logged_in;
}

static bool add_stats(const char *owner, unsigned seconds) {
    char key[15];
    stats_key(owner, key);
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READWRITE, &nvs) != ESP_OK) return false;
    wr_stats_t stats = read_stats(nvs, key);
    stats.today = seconds > UINT32_MAX-stats.today ? UINT32_MAX : stats.today+seconds;
    stats.total = seconds > UINT64_MAX-stats.total ? UINT64_MAX : stats.total+seconds;
    esp_err_t result = nvs_set_blob(nvs, key, &stats, sizeof(stats));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result == ESP_OK;
}

bool wr_online(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    return status.network_ready && status.mode == READ_PICO_TRANSFER_MODE_STA;
}

bool wr_safe(const char *value) {
    if (!value || !*value) return false;
    for (; *value; ++value) if (!isalnum((unsigned char)*value) && !strchr("-_.~", *value)) return false;
    return true;
}

bool wr_value(const cJSON *value, char *out, size_t size) {
    if (cJSON_IsString(value)) return strlcpy(out, value->valuestring, size) < size;
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > 9007199254740991.0) return false;
    int n = snprintf(out, size, "%.0f", value->valuedouble);
    return n > 0 && (size_t)n < size;
}

bool wr_cancelled(void) { return !s_processing_report && atomic_load(&s_cancel); }
void wr_cancel(void) { atomic_store(&s_cancel, true); }

/* 阅读会话期间保持接收唤醒，退出时恢复原有省电策略。 */
static void radio_hold(bool on) {
    if (on == s_radio_held) return;
    s_radio_held = on;
    (void)read_pico_transfer_radio_hold(on);
}

static bool account_is(const char *owner, uint32_t library) {
    wr_account_t current;
    return read_account(&current) && current.token[0] &&
        !strcmp(current.owner, owner) && current.library == library;
}

bool wr_reader_fields_cached(const char *book, const char *chapter, wr_reader_t *reader) {
    if (!book || !chapter || !reader || s_report_fields.expires <= esp_timer_get_time() ||
        strcmp(s_http_account.owner, s_report_fields.account_book.owner) ||
        s_http_account.library != s_report_fields.account_book.library ||
        strcmp(book, s_report_fields.account_book.book_id) ||
        strcmp(chapter, s_report_fields.chapter)) return false;
    *reader = s_report_fields.fields;
    ESP_LOGI("WEREAD", "reader fields reused book=%s", book);
    return true;
}

static esp_err_t capture_cookie(esp_http_client_event_t *event) {
    wr_http_context_t *context = event->user_data;
    if (!context) return ESP_OK;
    /* ON_CONNECTED 只在真正新建 socket 时派发，是判断连接复用的唯一可靠信号。 */
    /* ON_CONNECTED fires only for a freshly opened socket, so it is the one reliable
     * signal that tells a reused keep-alive connection from a new handshake. */
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        context->connected = true;
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value) return ESP_OK;
    if (!strcasecmp(event->header_key,"Location")) {
        size_t n = strnlen(event->header_value, WR_HTTP_URL_BYTES);
        if (n && n < WR_HTTP_URL_BYTES) {
            memcpy(context->location,event->header_value,n);
            context->location[n]=0;
        }
        return ESP_OK;
    }
    if (strcasecmp(event->header_key,"Set-Cookie") ||
        !context->capture_service_cookies) return ESP_OK;
    const char *header = event->header_value;
    const char *equals = strchr(header,'=');
    if (!equals || strncmp(header,"wr_",3) || equals-header > 24) return ESP_OK;
    size_t key_len = equals-header, len = strcspn(header,";\r\n");
    if (len <= key_len+1 || len > 440) return ESP_OK;
    for (size_t i = 0; i < len; ++i) {
        if ((unsigned char)header[i] <= ' ' || (unsigned char)header[i] >= 127) return ESP_OK;
    }
    cookie_lock();
    char *p = s_cookies;
    while (*p) {
        char *end = strstr(p,"; ");
        if (!end) end = p+strlen(p);
        if ((size_t)(end-p) > key_len && p[key_len]=='=' && !strncmp(p,header,key_len)) {
            if (*end) memmove(p,end+2,strlen(end+2)+1);
            else if (p>s_cookies) p[-2]=0;
            else *p=0;
            break;
        }
        p = *end ? end+2 : end;
    }
    size_t used = strlen(s_cookies);
    if (used+len+3 <= sizeof(s_cookies)) {
        if (used) { memcpy(s_cookies+used,"; ",2); used+=2; }
        memcpy(s_cookies+used,header,len);
        s_cookies[used+len]=0;
        save_session_cookies();
    }
    cookie_unlock();
    return ESP_OK;
}

/* 长连接池：接口域与图片 CDN 是不同主机，单槽会在两类请求交替时被反复驱逐。
 * 池的生命周期跟随一次后台任务，任务结束即释放，空闲期既不占 TLS 上下文和内部
 * RAM，也不让射频为了保活一直停在高功耗状态。
 * A host-keyed keep-alive pool: the API host and the image CDN are different hosts,
 * so a single slot thrashes whenever the two alternate. The pool lives for one worker
 * job and is flushed afterwards, so idle time costs neither TLS memory nor radio power. */
/* 两个下载线程并发时同主机要能各占一条连接，所以槽位数按"并发数 + 接口域"给足。
 * Slots must cover the concurrent downloaders plus the API host, because same-host
 * requests running in parallel each need their own live connection. */
/* 每槽一条活连接，稳定占用几 KB 内部 RAM（收发缓冲 + TLS 上下文），而这块板子的
 * 内部 RAM 还要喂 Wi-Fi 驱动。并发只有「正文/接口 + 图片」两路，三槽足够。
 * Each live slot costs a few KB of internal RAM (buffers plus TLS context) and the
 * Wi-Fi driver competes for the same pool. Concurrency is at most API + CDN, so three
 * slots cover it. */
#define WR_HTTP_POOL_SLOTS 3
/* 空闲超过 20 s 的槽位不再复用：服务端通常 30~75 s 就关掉 keep-alive，而客户端要等到
 * 写进这条死 socket、把超时耗完才发现（实测每次白等 10 s）。重建一条 TLS 约 600 ms，
 * 比赌它还活着便宜得多。
 * Slots idle beyond 20 s are not reused: servers drop keep-alive after 30-75 s while the
 * client only notices after writing into the dead socket and burning the timeout (10 s per
 * occurrence in practice). A fresh handshake costs ~600 ms, far cheaper than betting. */
#define WR_POOL_IDLE_US (20LL * 1000LL * 1000LL)
typedef struct {
    esp_http_client_handle_t handle;
    char host[128];
    int64_t last_us;
    /// 有请求正在使用这条连接；并发下载时不能被别的线程抢走。
    /// A request currently owns this connection; another thread must not take it.
    bool busy;
} wr_http_pool_t;
static wr_http_pool_t s_http_pool[WR_HTTP_POOL_SLOTS];
static StaticSemaphore_t s_pool_mutex_storage;
static SemaphoreHandle_t s_pool_mutex;

static void pool_lock(void) {
    if (s_pool_mutex) (void)xSemaphoreTake(s_pool_mutex, portMAX_DELAY);
}
static void pool_unlock(void) {
    if (s_pool_mutex) (void)xSemaphoreGive(s_pool_mutex);
}

static void http_pool_flush(void) {
    esp_http_client_handle_t doomed[WR_HTTP_POOL_SLOTS] = {0};
    unsigned count = 0;
    pool_lock();
    for (unsigned i = 0; i < WR_HTTP_POOL_SLOTS; ++i) {
        if (s_http_pool[i].handle && !s_http_pool[i].busy) doomed[count++] = s_http_pool[i].handle;
        if (!s_http_pool[i].busy) {
            s_http_pool[i].handle = NULL;
            s_http_pool[i].host[0] = 0;
            s_http_pool[i].last_us = 0;
        }
    }
    pool_unlock();
    /* cleanup 会发 close 并可能阻塞，放在锁外做。 / Cleanup blocks, so run it outside the lock. */
    for (unsigned i = 0; i < count; ++i) esp_http_client_cleanup(doomed[i]);
}

/* 射频要重新初始化前调用：空闲长连接先关掉，别让 socket 越过 Wi-Fi deinit 变成野句柄。
 * Called before the radio re-initialises: close idle sockets so none survives a Wi-Fi deinit. */
void wr_http_release(void) { http_pool_flush(); }

/* keep=true 只更新时间戳，TCP/TLS 会话保持打开；keep=false 直接销毁。
 * keep=true only stamps the slot and leaves the session open; keep=false destroys it. */
static void http_pool_release(esp_http_client_handle_t http, bool keep) {
    if (!http) return;
    bool owned = false, destroy = !keep;
    pool_lock();
    for (unsigned i = 0; i < WR_HTTP_POOL_SLOTS; ++i) {
        if (s_http_pool[i].handle != http) continue;
        owned = true;
        if (keep) {
            /* 句柄留在池里，调用方的请求上下文马上就释放，先摘掉指针避免悬垂。 */
            (void)esp_http_client_set_user_data(http, NULL);
            s_http_pool[i].last_us = esp_timer_get_time();
            s_http_pool[i].busy = false;
            destroy = false;
        } else {
            s_http_pool[i].handle = NULL;
            s_http_pool[i].host[0] = 0;
            s_http_pool[i].last_us = 0;
            s_http_pool[i].busy = false;
        }
        break;
    }
    pool_unlock();
    if (destroy || !owned) esp_http_client_cleanup(http);
}

static bool http_url_host(const char *url, char host[128]) {
    if (!url || strncmp(url, "https://", 8)) return false;
    const char *start = url + 8;
    const char *end = strpbrk(start, "/?#");
    if (!end) end = start + strlen(start);
    size_t length = (size_t)(end - start);
    if (!length || length >= 128) return false;
    memcpy(host, start, length);
    host[length] = 0;
    return true;
}

static esp_http_client_handle_t http_pool_acquire(const char *url, wr_http_context_t *request,
                                                  bool *matched) {
    char host[128];
    if (!http_url_host(url, host)) {
        ESP_LOGW("WEREAD", "pool acquire rejected url='%s'", url);
        return NULL;
    }
    *matched = false;
    esp_http_client_handle_t reuse = NULL;
    esp_http_client_handle_t evict = NULL;
    pool_lock();
    int spare = -1;
    for (unsigned i = 0; i < WR_HTTP_POOL_SLOTS; ++i) {
        if (!s_http_pool[i].handle) { if (!s_http_pool[i].busy && spare < 0) spare = (int)i; continue; }
        if (s_http_pool[i].busy || strcasecmp(host, s_http_pool[i].host)) continue;
        /* 空闲太久的长连接大概率已被对端关掉，别浪费一次超时。 / An idle socket this old is
         * most likely closed by the peer; skip it rather than waste a timeout on it. */
        if (esp_timer_get_time() - s_http_pool[i].last_us > WR_POOL_IDLE_US) {
            ESP_LOGI("WEREAD", "pool drop idle slot=%u idle_ms=%lld url='%s'", i,
                     (long long)((esp_timer_get_time() - s_http_pool[i].last_us) / 1000), url);
            evict = s_http_pool[i].handle;
            s_http_pool[i].handle = NULL;
            s_http_pool[i].host[0] = 0;
            s_http_pool[i].last_us = 0;
            s_http_pool[i].busy = false;
            spare = (int)i;
            break;
        }
        if (esp_http_client_set_url(s_http_pool[i].handle, url) != ESP_OK ||
            esp_http_client_set_event_handler(s_http_pool[i].handle, capture_cookie) != ESP_OK ||
            esp_http_client_set_user_data(s_http_pool[i].handle, request) != ESP_OK) {
            ESP_LOGW("WEREAD", "pool reuse setup failed url='%s' slot=%u", url, i);
            evict = s_http_pool[i].handle;
            s_http_pool[i].handle = NULL;
            s_http_pool[i].host[0] = 0;
            s_http_pool[i].last_us = 0;
            s_http_pool[i].busy = false;
            if (spare < 0) spare = (int)i;
            break;
        }
        s_http_pool[i].last_us = esp_timer_get_time();
        s_http_pool[i].busy = true;
        reuse = s_http_pool[i].handle;
        break;
    }
    if (!reuse) {
        if (spare < 0) {
            /* 槽位已满：淘汰最久未用且空闲的一条，避免堆更多 TLS 上下文。 */
            int64_t oldest = 0;
            int victim = -1;
            for (unsigned i = 0; i < WR_HTTP_POOL_SLOTS; ++i) {
                if (s_http_pool[i].busy || !s_http_pool[i].handle) continue;
                if (victim < 0 || s_http_pool[i].last_us < oldest) {
                    oldest = s_http_pool[i].last_us;
                    victim = (int)i;
                }
            }
            if (victim < 0) { pool_unlock(); ESP_LOGW("WEREAD", "pool exhausted url='%s'", url); return NULL; }
            spare = victim;
            evict = s_http_pool[spare].handle;
            s_http_pool[spare].handle = NULL;
        }
        s_http_pool[spare].busy = true;
        s_http_pool[spare].last_us = esp_timer_get_time();
        snprintf(s_http_pool[spare].host, sizeof(s_http_pool[spare].host), "%s", host);
    }
    pool_unlock();
    if (reuse) { *matched = true; return reuse; }
    if (evict) esp_http_client_cleanup(evict);
    esp_http_client_config_t config = {
        /* Keep the HTTP/TLS working set modest on S31: the panel and Wi-Fi
         * driver need internal RAM while mbedTLS allocates AES/GCM contexts. */
        .url = url, .timeout_ms = 15000, .buffer_size = 4096, .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
        .keep_alive_enable = true, .keep_alive_idle = 30, .keep_alive_interval = 5,
        .keep_alive_count = 3, .user_agent = WR_AGENT,
        .event_handler = capture_cookie, .user_data = request,
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (!http) {
        pool_lock();
        if (spare >= 0 && !s_http_pool[spare].handle) {
            s_http_pool[spare].host[0] = 0;
            s_http_pool[spare].last_us = 0;
            s_http_pool[spare].busy = false;
        }
        pool_unlock();
        ESP_LOGW("WEREAD", "pool init failed url='%s' internal=%u largest=%u",
                 url, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return NULL;
    }
    pool_lock();
    if (spare >= 0 && !s_http_pool[spare].handle) {
        s_http_pool[spare].handle = http;
        s_http_pool[spare].busy = true;
        s_http_pool[spare].last_us = esp_timer_get_time();
        snprintf(s_http_pool[spare].host, sizeof(s_http_pool[spare].host), "%s", host);
    }
    pool_unlock();
    return http;
}

static void restore_session_cookie(void) {
    cookie_lock();
    if (s_cookies[0]) { cookie_unlock(); return; }
    nvs_handle_t nvs;
    if (nvs_open("weread", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(s_cookies);
        if (nvs_get_str(nvs, "cookies", s_cookies, &size) == ESP_OK && s_cookies[0]) {
            nvs_close(nvs);
            cookie_unlock();
            return;
        }
        nvs_close(nvs);
    }
    wr_account_t account;
    if (!read_account(&account) || !account.owner[0] || !account.token[0]) { cookie_unlock(); return; }
    /* CrossMux persists the same three WeRead cookies across reboot.  QR login
     * gives us the vid and skey; rebuild the durable pair before shelf calls. */
    snprintf(s_cookies, sizeof(s_cookies), "wr_vid=%s; wr_skey=%s", account.owner, account.token);
    cookie_unlock();
}

static bool wr_url_allowed(const char *url, bool service) {
    if (!url || strncmp(url,"https://",8)) return false;
    const char *host=url+8, *end=strpbrk(host,"/?#");
    if (!end) end=host+strlen(host);
    const char *port = memchr(host, ':', (size_t)(end-host));
    if (port) {
        if (end-port != 4 || memcmp(port, ":443", 4)) return false;
        end = port;
    }
    size_t n=(size_t)(end-host);
    if (!n || n>=128) return false;
    for (const char *p=host;p<end;++p)
        if (!isalnum((unsigned char)*p) && *p!='.' && *p!='-') return false;
    if (service) return n==strlen("weread.qq.com") && !strncasecmp(host,"weread.qq.com",n);
    static const char *const suffixes[]={".qq.com",".qpic.cn",".qlogo.cn",".myqcloud.com",".gfp.tencent-cloud.com"};
    for (size_t i=0;i<sizeof(suffixes)/sizeof(*suffixes);++i) {
        size_t suffix=strlen(suffixes[i]);
        if (n>suffix && !strncasecmp(end-suffix,suffixes[i],suffix)) return true;
    }
    return false;
}

static void log_rejected_image_url(const char *url) {
    const char *scheme = url ? strstr(url, "://") : NULL;
    const char *host = scheme ? scheme + 3 : (url ? url : "");
    const char *end = strpbrk(host, "/?#");
    if (!end) end = host + strlen(host);
    char safe_host[128];
    size_t length = (size_t)(end - host);
    if (length >= sizeof(safe_host)) length = sizeof(safe_host) - 1;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)host[i];
        safe_host[i] = (isalnum(c) || c == '.' || c == '-' || c == ':') ? (char)c : '?';
    }
    safe_host[length] = 0;
    ESP_LOGW("WEREAD", "image URL rejected scheme=%s host=%s length=%u",
             scheme ? (size_t)(scheme - url) == 5 ? "https" : (size_t)(scheme - url) == 4 ? "http" : "other" : "none",
             safe_host, (unsigned)(url ? strlen(url) : 0));
}

/* Resolve WeRead CDN redirects ourselves. ESP-IDF rejects HTTPS -> HTTP
 * redirects before callers can normalize them, while WeRead sometimes emits
 * exactly that URL form. Upgrade it to HTTPS, then apply the host allowlist. */
static bool cover_redirect_url(const char *current, const char *location,
                               char out[WR_HTTP_URL_BYTES]) {
    if (!current || !location || !*location) return false;
    char resolved[WR_HTTP_URL_BYTES];
    int n;
    if (!strncasecmp(location,"https://",8) || !strncasecmp(location,"http://",7)) {
        n=snprintf(resolved,sizeof(resolved),"%s",location);
    } else if (!strncmp(location,"//",2)) {
        n=snprintf(resolved,sizeof(resolved),"https:%s",location);
    } else {
        const char *authority_end=strpbrk(current+8,"/?#");
        if (!authority_end) authority_end=current+strlen(current);
        if (location[0]=='/') {
            n=snprintf(resolved,sizeof(resolved),"%.*s%s",(int)(authority_end-current),current,location);
        } else {
            const char *last=strrchr(current,'/');
            if (!last || last<authority_end) last=authority_end;
            n=snprintf(resolved,sizeof(resolved),"%.*s/%s",(int)(last-current),current,location);
        }
    }
    if (n<0 || (size_t)n>=sizeof(resolved)) return false;
    if (!strncmp(resolved,"http://",7)) {
        char *authority_end=strpbrk(resolved+7,"/?#");
        if (!authority_end) authority_end=resolved+strlen(resolved);
        char authority[256];
        size_t host_n=(size_t)(authority_end-(resolved+7));
        if (!host_n || host_n>=sizeof(authority)) return false;
        memcpy(authority,resolved+7,host_n); authority[host_n]=0;
        size_t keep=host_n;
        if (keep>3 && !strcmp(authority+keep-3,":80")) { keep-=3; authority[keep]=0; }
        n=snprintf(out,WR_HTTP_URL_BYTES,"https://%s%s",authority,authority_end);
    } else n=snprintf(out,WR_HTTP_URL_BYTES,"%s",resolved);
    return n>0 && (size_t)n<WR_HTTP_URL_BYTES;
}

void wr_reading_display(bool scanning) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_reader_scanning = scanning;
    if (!scanning) s_reader_scan_end = now;
    portEXIT_CRITICAL(&s_lock);
}

static bool report_display_busy(void) {
    portENTER_CRITICAL(&s_lock);
    bool busy = s_reader_scanning;
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

/* 响应体阶段的单次读超时，以及滴流看门狗的窗口/最低字节数。
 * Per-read timeout while streaming a body, plus the trickle watchdog window. */
#define WR_HTTP_BODY_TIMEOUT_MS  8000
#define WR_HTTP_STALL_WINDOW_US  6000000
#define WR_HTTP_STALL_MIN_BYTES  24576
/* reader 页读完字段后最多再花多久把剩余字节读干（约 4 s / 1 MB）。 */
#define WR_HTTP_DRAIN_US         4000000
#define WR_HTTP_DRAIN_LIMIT      (1024 * 1024)

/* 把响应体的剩余字节读干净，让连接可以回到池中复用。返回连接是否可以保留：
 * 只有读到"完整接收"才算数，超时或用完预算都按不可复用处理。
 * Drain the rest of the body so the connection can return to the pool. Only a fully
 * received body counts as reusable; timeouts and exhausted budget are not. */
static bool drain_response(esp_http_client_handle_t http, int64_t deadline_us) {
    char scratch[512];
    size_t dropped = 0;
    bool reusable = false;
    (void)esp_http_client_set_timeout_ms(http, 3000);
    while (!wr_cancelled() && dropped < WR_HTTP_DRAIN_LIMIT) {
        int n = esp_http_client_read(http, scratch, sizeof(scratch));
        if (n < 0) break;
        dropped += (size_t)n;
        if (esp_http_client_is_complete_data_received(http)) { reusable = true; break; }
        if (!n || esp_timer_get_time() >= deadline_us) break;
    }
    /* 排空用的是短超时，句柄回到池里之前恢复默认值，免得下一次请求开局就被 3 s 掐掉。 */
    (void)esp_http_client_set_timeout_ms(http, 15000);
    return reusable;
}

/* zlib 默认的 32 KB 解压窗口若走内部 RAM 会分配失败，这里全部改走 PSRAM。
 * zlib's default 32 KB window would come from internal RAM and fail there; use PSRAM. */
static voidpf gz_alloc(voidpf opaque, uInt items, uInt size) {
    (void)opaque;
    return heap_caps_malloc((size_t)items * size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void gz_release(voidpf opaque, voidpf address) { (void)opaque; free(address); }

/* 把一段 gzip 字节喂给字段扫描回调；返回 true 表示回调已经取到需要的字段。
 * Feed one gzip block to the field scanner; true means the scanner has what it needs. */
static bool gz_scan(z_stream *stream, const char *input, size_t length, char *out, size_t out_size,
                    wr_http_chunk_fn chunk, void *context, bool *error) {
    stream->next_in = (Bytef *)input;
    stream->avail_in = (uInt)length;
    while (stream->avail_in) {
        stream->next_out = (Bytef *)out;
        stream->avail_out = (uInt)out_size;
        int status = inflate(stream, Z_SYNC_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) { *error = true; return false; }
        size_t produced = out_size - stream->avail_out;
        if (produced && chunk(out, produced, context)) return true;
        if (status == Z_STREAM_END) return false;      /* 一条响应只有一条 gzip 流 */
        if (!produced && status == Z_BUF_ERROR) return false;
    }
    return false;
}

static char *http_request_once(const char *url, const char *body, const char *referer, size_t limit,
                               size_t *length, wr_http_chunk_fn chunk, void *context,
                               bool *fields_received, bool *post_started, bool *rode_reused) {
    const int64_t request_started_us = esp_timer_get_time();
    s_http_open_failed = false;
    if (wr_cancelled()) { s_error_message = "操作已取消"; return NULL; }
    if (!wr_online()) { http_pool_flush(); s_error_message = "Wi-Fi 未连接或网络尚未就绪"; return NULL; }
    if (url[0] == '/') restore_session_cookie();
    wr_http_context_t *request = heap_caps_calloc(1, sizeof(*request), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request) { s_error_message = "内存不足，无法创建微信读书请求"; return NULL; }
    char *absolute = request->absolute;
    const bool service = url[0] == '/';
    request->capture_service_cookies = service;
    bool upgrade = !service && !strncmp(url, "http://", 7);
    const char *prefix = service ? WR_HOST : upgrade ? "https://" : "";
    int url_size = snprintf(absolute, WR_HTTP_URL_BYTES, "%s%s", prefix, upgrade ? url + 7 : url);
    if (url_size < 0 || url_size >= WR_HTTP_URL_BYTES) {
        s_error_message = "微信读书请求地址过长";
        free(request);
        return NULL;
    }
    if (!service && !wr_url_allowed(absolute, false)) {
        log_rejected_image_url(absolute);
        s_error_message="微信读书图片地址不受支持";
        free(request);
        return NULL;
    }
    for (const char *p = absolute; *p; ++p) if ((unsigned char)*p <= ' ' || *p == '\\') { free(request); return NULL; }
    cookie_lock();
    memcpy(request->cookie_snapshot, s_cookies, sizeof(request->cookie_snapshot));
    request->cookie_snapshot[sizeof(request->cookie_snapshot)-1] = 0;
    cookie_unlock();
    /* 同一主机的请求复用同一条 TCP/TLS 长连接，不再每次重新握手。只有把响应体
     * 完整读完的连接才归还池中；提前停止、超限或出错的一律销毁，避免残留字节被
     * 当成下一个响应的开头（这正是上一版 perform 实验返回异常的主因）。
     * Same-host requests reuse one TCP/TLS connection instead of a full handshake.
     * Only a connection whose body was fully drained returns to the pool; early exits,
     * over-limit bodies and errors are destroyed so stale bytes can never be parsed as
     * the head of the next response. */
    esp_http_client_handle_t http = NULL;
    bool pooled = false;
    char *data = NULL;
    size_t used = 0, capacity = 0;
    int64_t scan_wait_until = 0;
    int64_t reader_deadline = chunk && s_processing_report ? esp_timer_get_time() + 90000000 : 0;
    bool complete = false;
    /* 服务端用 Connection: close 结束会话时，这条连接不能回池：下一个请求会写进一条
     * 已经半死的 socket，白等满 8 s 才失败（实测每轮 2 次，合计 16 s）。
     * A response that ends the session must not return to the pool: the next request would
     * write into a half-dead socket and burn the full 8 s timeout (2 x per run in practice). */
    bool no_keep = false;
    bool allocation_failed = false, read_failed = false;
    /* 只有大响应体（图片、书架 JSON）才需要滴流看门狗，小接口不必背这个判断。 */
    const bool stall_body = !chunk && limit > 65536;
    bool stalled = false;
    int64_t stall_mark_us = esp_timer_get_time();
    size_t stall_mark_used = 0;
    /* reader 页走 gzip：解压器只在真的收到压缩响应时才建立。 */
    z_stream gz_stream;
    memset(&gz_stream, 0, sizeof(gz_stream));
    bool gz_live = false, gz_broken = false, gz_tried = false;
    char *gz_out = NULL;
    // 提前声明：open 失败会 goto cleanup，那里要读 code 决定提示语。
    // Declared up front: the open failure jumps to cleanup, which reads code to pick a message.
    int code = 0;
    int body_size = body ? (int)strlen(body) : 0;
    int64_t declared = -1;
    snprintf(request->current, sizeof(request->current), "%s", absolute);
    for (unsigned redirects = 0, reopened = 0;;) {
        request->location[0]=0;
        if (!http) {
            bool matched = false;
            http = http_pool_acquire(absolute, request, &matched);
            if (!http) {
                s_error_message = "无法创建网络请求";
                ESP_LOGW("WEREAD", "request setup failed url=%s absolute=%s", url, absolute);
                goto cleanup;
            }
            pooled = matched;
            request->connected = false;
            esp_http_client_delete_all_headers(http);
            esp_http_client_reset_redirect_counter(http);
            /* Host/User-Agent 只在 esp_http_client_init 时写入，delete_all_headers 会把
             * 它们一并删掉；HTTP/1.1 缺 Host 会被服务端直接拒绝，必须按当前 URL 重建。
             * Host/User-Agent are only defaulted at init time and delete_all_headers drops
             * them too; HTTP/1.1 without Host is rejected outright, so rebuild both. */
            char host[128];
            if (!http_url_host(absolute, host)) { s_error_message = "微信读书请求地址无效"; goto cleanup; }
            esp_http_client_set_header(http, "Host", host);
            esp_http_client_set_header(http, "User-Agent", WR_AGENT);
            esp_http_client_set_method(http, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
            /* reader 页是 170 KB 的 HTML，gzip 能把它压到五分之一；其余路径（图片、
             * tar 资源包、JSON）本身是二进制，压缩没有收益还要多一层解码。
             * The reader page is ~170 KB of HTML and gzip cuts it to a fifth. Other paths
             * (images, tar bundles, JSON) are binary, so compression only adds a layer. */
            esp_http_client_set_header(http, "Accept-Encoding", chunk ? "gzip" : "identity");
            esp_http_client_set_header(http, "Referer", referer ? referer : WR_HOST "/");
            if (service) {
                esp_http_client_set_header(http,"Accept","application/json, text/plain, */*");
                esp_http_client_set_header(http,"Origin",WR_HOST);
            }
            /* 章节资源包（tar）在 res.weread.qq.com 上，和接口共用同一套 Cookie；
             * 纯图片 CDN 不带也无妨，所以只对 weread 域补。
             * The chapter resource package lives on res.weread.qq.com and needs the same
             * cookies as the API; plain image CDN requests do not, so only weread hosts. */
            if ((service || !strncasecmp(host, "res.weread.qq.com", 18)) && request->cookie_snapshot[0])
                esp_http_client_set_header(http,"Cookie",request->cookie_snapshot);
            if (body) esp_http_client_set_header(http, "Content-Type", "application/json;charset=UTF-8");
            /* 必须每次都调用：复用的连接上一轮可能是 POST，要清掉残留的 post_data/post_len。
             * Must run every time: a reused handle may still carry the previous POST body.
             * 但 GET（body==NULL）时 IDF 会走 set_header("Content-Type", NULL) 删除头，头不存在就返回
             * ESP_ERR_NOT_FOUND——这只说明"本来就没有要删的头"，对 GET 而言正是想要的结果，不能当失败。
             * For GET, IDF deletes the Content-Type header and returns ESP_ERR_NOT_FOUND when it does not
             * exist; that is exactly the intended state, so it is not a failure. */
            esp_err_t post_error = esp_http_client_set_post_field(http, body, body_size);
            if (post_error != ESP_OK && !(post_error == ESP_ERR_NOT_FOUND && !body)) {
                s_error_message = "无法准备微信读书请求";
                ESP_LOGW("WEREAD", "set_post_field failed url=%s body=%d err=%s", url, (int)(body != NULL),
                         esp_err_to_name(post_error));
                goto cleanup;
            }
        }
        int64_t open_start = esp_timer_get_time();
        esp_err_t open_error = esp_http_client_open(http, body_size);
        if (open_error != ESP_OK) {
            int tls_code = 0, tls_flags = 0;
            esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(http, &tls_code, &tls_flags);
            ESP_LOGW("WEREAD", "HTTP open failed url=%s err=%s ms=%lld tls=%s code=%d flags=0x%x reused=%d internal=%u largest=%u",
                     url, esp_err_to_name(open_error), (long long)((esp_timer_get_time()-open_start)/1000),
                     esp_err_to_name(tls_error), tls_code, (unsigned)tls_flags, (int)pooled,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            /* 池里的长连接可能已被服务端或 NAT 静默回收：换一条新连接重试一次。
             * A pooled connection may already be gone; retry once on a fresh one. */
            if (pooled && !request->connected && reopened++ == 0) {
                http_pool_release(http, false);
                http = NULL;
                pooled = false;
                continue;
            }
            s_http_open_failed = true;
            s_error_message = "微信读书网络连接失败，请检查 Wi-Fi 后重试";
            goto cleanup;
        }
        if (post_started && body_size) *post_started = true;
        for (int sent = 0; sent < body_size;) {
            int n = esp_http_client_write(http, body + sent, body_size - sent);
            if (n <= 0) { s_error_message = "网络请求发送失败"; goto cleanup; }
            sent += n;
        }
        declared = esp_http_client_fetch_headers(http);
        code = esp_http_client_get_status_code(http);
        ESP_LOGI("WEREAD", "HTTP %s code=%d declared=%lld chunked=%d reused=%d", url, code, (long long)declared,
                 (int)esp_http_client_is_chunked_response(http), (int)pooled);
        /* 头名大小写不敏感，IDF 的查找是敏感的，所以两个都试。
         * Header lookup in IDF is case-sensitive, so try both spellings. */
        char *conn = NULL;
        if (esp_http_client_get_header(http, "Connection", &conn) != ESP_OK || !conn)
            (void)esp_http_client_get_header(http, "connection", &conn);
        if (conn && !strcasecmp(conn, "close")) {
            no_keep = true;
            /* 只在这条连接本来会被复用时才值得记一行：这就是下一次请求卡死的原因。 */
            /* Only worth a line when this connection would have been pooled: that is
             * exactly what makes the *next* request stall. */
            if (pooled) ESP_LOGI("WEREAD", "HTTP close header on reused connection url=%s", url);
        }
        /* 是否压缩看响应体的 gzip 魔数，而不是 Content-Encoding 头：IDF 的
         * esp_http_client_get_header 区分大小写，服务端发的是小写头名，按名字查会漏。
         * Detect compression from the gzip magic bytes rather than the Content-Encoding
         * header: IDF header lookup is case-sensitive and the server sends lowercase. */
        bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
        char *redirected = request->redirected;
        if (!redirect) break;
        if (redirects >= 5) { s_error_message="微信读书封面重定向次数过多"; goto cleanup; }
        if (!service) {
            if (!cover_redirect_url(request->current,request->location,redirected)) {
                ESP_LOGW("WEREAD","cover redirect invalid location='%s'",request->location);
                s_error_message = "微信读书图片重定向地址不受支持";
                goto cleanup;
            }
            if (!wr_url_allowed(redirected,false) || esp_http_client_set_url(http,redirected)!=ESP_OK) {
                ESP_LOGW("WEREAD","cover redirect target rejected location='%s' target='%s'",request->location,redirected);
                s_error_message = "微信读书图片重定向地址不受支持";
                goto cleanup;
            }
        } else {
            if (esp_http_client_set_redirection(http) != ESP_OK ||
                esp_http_client_get_url(http,redirected,WR_HTTP_URL_BYTES) != ESP_OK) {
                s_error_message="微信读书接口重定向失败";
                goto cleanup;
            }
        }
        if (!wr_url_allowed(redirected, service)) {
            ESP_LOGW("WEREAD","redirect target rejected service=%d target='%s'",service,redirected);
            s_error_message = "微信读书图片重定向地址不受支持";
            goto cleanup;
        }
        ESP_LOGI("WEREAD", "HTTP redirect %s -> %s", url, redirected);
        snprintf(request->current,sizeof(request->current),"%s",redirected);
        if (code == 301 || code == 302 || code == 303) {
            body_size = 0;
            esp_http_client_set_method(http, HTTP_METHOD_GET);
        }
        esp_http_client_close(http);
        ++redirects;
    }
    if (service && (code == 401 || code == 403)) { s_expired = true; s_error_message = "登录已失效，请重新扫码"; }
    else if (code < 200 || code >= 300) s_error_message = "微信读书接口返回错误";
    if ((declared < 0 && !esp_http_client_is_chunked_response(http)) ||
        (declared > 0 && (uint64_t)declared > limit) || code != 200) goto cleanup;
    if (declared < 0) declared = 0;
    /* 响应体阶段用更短的 socket 超时：一次 recv 卡住 15 s 会让整章空等，
     * 而正常 Wi-Fi 上读一块数据远用不到 8 s。
     * A shorter socket timeout while streaming the body: a single stuck recv would
     * otherwise waste 15 s per attempt, far beyond what Wi-Fi needs for one block. */
    if (!chunk) esp_http_client_set_timeout_ms(http, WR_HTTP_BODY_TIMEOUT_MS);
    while (!wr_cancelled()) {
        if (chunk) {
            if (reader_deadline && esp_timer_get_time() >= reader_deadline) {
                ESP_LOGW("WEREAD", "report reader response deadline bytes=%u", (unsigned)used);
                goto cleanup;
            }
            if (s_processing_report && report_display_busy()) {
                if (!scan_wait_until) scan_wait_until = esp_timer_get_time() + 3000000;
                if (esp_timer_get_time() >= scan_wait_until) {
                    ESP_LOGI("WEREAD", "reader fields deferred during display scan bytes=%u", (unsigned)used);
                    goto cleanup;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            scan_wait_until = 0;
            if (!data) {
                data = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!data) { allocation_failed = true; goto cleanup; }
            }
            size_t remaining = limit - used;
            if (!remaining) break;
            int n = esp_http_client_read(http, data, (int)(remaining < 2048 ? remaining : 2048));
            if (n < 0) { read_failed = true; goto cleanup; }
            used += (size_t)n;
            bool fields = false;
            if (n) {
                if (!gz_tried) {
                    gz_tried = true;
                    if (n >= 2 && (uint8_t)data[0] == 0x1f && (uint8_t)data[1] == 0x8b) {
                        gz_stream.zalloc = gz_alloc;
                        gz_stream.zfree = gz_release;
                        if (inflateInit2(&gz_stream, 16 + MAX_WBITS) != Z_OK ||
                            !(gz_out = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
                            gz_broken = true;
                            s_error_message = "内存不足，无法解压微信读书响应";
                            ESP_LOGW("WEREAD", "gzip setup failed url=%s internal=%u", url,
                                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                            goto cleanup;
                        }
                        gz_live = true;
                    }
                    ESP_LOGI("WEREAD", "reader body magic=%02x%02x gzip=%d declared=%lld",
                             (unsigned)(uint8_t)data[0], n >= 2 ? (unsigned)(uint8_t)data[1] : 0u,
                             (int)gz_live, (long long)declared);
                }
                if (gz_live) {
                    fields = gz_scan(&gz_stream, data, (size_t)n, gz_out, 2048, chunk, context, &gz_broken);
                    if (gz_broken) { read_failed = true; goto cleanup; }
                } else {
                    fields = chunk(data, (size_t)n, context);
                }
            }
            if (fields) {
                if (fields_received) *fields_received = true;
                /* 拿到字段就停会让响应体残留在 socket 里，连接只能销毁，下一章的
                 * 第一个分片又得重新握手。这里把剩余字节读干，连接就能回到池中。
                 * 阅读时长上报走的是同一段扫描，但用户在翻页，不为它多等。
                 * Stopping at the fields leaves the rest of the body in the socket, so
                 * the connection must be destroyed and the next chapter pays another
                 * handshake. Drain the remainder and the connection returns to the pool.
                 * Reading reports share this scan but the user is turning pages, so they
                 * do not wait for the drain. */
                if (!s_processing_report) complete = drain_response(http, esp_timer_get_time() + WR_HTTP_DRAIN_US);
                break;
            }
            if (!n || esp_http_client_is_complete_data_received(http)) {
                complete = esp_http_client_is_complete_data_received(http);
                break;
            }
            if (s_processing_report) vTaskDelay(1);
            continue;
        }
        if (used == capacity) {
            if (used == limit) { complete = esp_http_client_is_complete_data_received(http); break; }
            size_t next = capacity ? capacity * 2 : declared ? (size_t)declared : 4096;
            if (next > limit) next = limit;
            char *grown = heap_caps_realloc(data, next + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!grown) { allocation_failed = true; goto cleanup; }
            data = grown;
            capacity = next;
        }
        int n = esp_http_client_read(http, data + used, (int)(capacity - used));
        if (n < 0) { read_failed = true; goto cleanup; }
        used += (size_t)n;
        if (!n || esp_http_client_is_complete_data_received(http)) {
            complete = esp_http_client_is_complete_data_received(http);
            break;
        }
        /* 慢速滴流看门狗：窗口内拿不到足够字节就主动断线重试。服务端"滴流"时每次
         * recv 都能在超时前挤几个字节，单次超时永远不触发，于是一次请求白占一分钟。
         * Slow-trickle watchdog: too few bytes per window means abort and retry. A
         * dripping server keeps every single recv under the timeout, so the per-recv
         * timeout alone never fires and one request burns a full minute. */
        if (stall_body) {
            int64_t now_us = esp_timer_get_time();
            if (used - stall_mark_used >= WR_HTTP_STALL_MIN_BYTES) {
                stall_mark_used = used;
                stall_mark_us = now_us;
            } else if (now_us - stall_mark_us >= WR_HTTP_STALL_WINDOW_US) {
                stalled = true;
                ESP_LOGW("WEREAD", "HTTP stall abort url=%s bytes=%u/%lld elapsed_ms=%lld reused=%d",
                         url, (unsigned)used, (long long)declared,
                         (long long)((esp_timer_get_time() - request_started_us) / 1000), (int)pooled);
                goto cleanup;
            }
        }
    }
cleanup:
    if (gz_live) inflateEnd(&gz_stream);
    free(gz_out);
    /* 只有完整读完响应体、且服务端没说 close 的连接才值得复用。 */
    /* Reuse only fully drained connections the server did not ask to close. */
    http_pool_release(http, complete && !no_keep && !wr_cancelled());
    /* 这次请求真正沿用了池中的长连接：命中了同主机槽位，且没有新建 socket。
     * The request genuinely rode a pooled connection: same-host slot hit and no new socket. */
    const bool reused = pooled && !request->connected;
    if (rode_reused) *rode_reused = reused;
    free(request);
    if (chunk && (complete || (fields_received && *fields_received)) && !wr_cancelled()) {
        if (fields_received) *fields_received = true;
        ESP_LOGI("WEREAD", "reader HTML wire_bytes=%u gzip=%d full_response=%d elapsed_ms=%lld reused=%d", (unsigned)used,
                 (int)gz_live, (int)complete, (long long)((esp_timer_get_time()-request_started_us)/1000), (int)reused);
        free(data);
        return NULL;
    }
    if (!complete || !data || wr_cancelled()) {
        if (allocation_failed) s_error_message = "内存不足，无法接收微信读书响应";
        else if (read_failed) s_error_message = "读取微信读书响应失败";
        else if (stalled) s_error_message = "微信读书下载停滞";
        else if (wr_cancelled()) s_error_message = "操作已取消";
        else if (code != 200) { /* Preserve the more specific HTTP/authentication message. */ }
        else s_error_message = "微信读书响应未完整接收";
        /* code=0 表示连响应头都没拿到（请求阶段就失败了）；code≠0 才是"连上但读不完整"。
         * code=0 means the request never reached a response head; otherwise the body was truncated. */
        ESP_LOGW("WEREAD", "HTTP read failed url=%s code=%d declared=%lld complete=%d bytes=%u reused=%d stalled=%d",
                 url, code, (long long)declared, (int)complete, (unsigned)used, (int)reused, (int)stalled);
        free(data); return NULL;
    }
    data[used] = 0;
    ESP_LOGI("WEREAD", "HTTP complete url=%s bytes=%u elapsed_ms=%lld connection_reused=%d", url, (unsigned)used,
             (long long)((esp_timer_get_time()-request_started_us)/1000), (int)reused);
    if (length) *length = used;
    return data;
}

/* 连接池里的长连接可能已经被服务端悄悄关掉：客户端只有写进这条半死 socket、等满
 * 超时才失败（实测 7 次 `Connection timed out before data was ready`，全部 reused=1，
 * 单次白等 10 s，其中一次直接把第 13 章判成失败、整轮 143 s 作废）。这里在非流式
 * 请求上补一层"丢掉池里所有空闲连接、重新握手再试一次"，流式请求（reader 页要边收
 * 边扫字段）不重试，靠池的空闲过期来规避。
 * A pooled keep-alive socket may already be closed by the peer: the client only notices
 * after writing into the half-dead socket and burning the full timeout (7 x "Connection
 * timed out before data was ready", all reused=1, 10 s each, one of which failed chapter 13
 * and threw away a 143 s run). Non-streaming requests therefore flush the pool and retry
 * once on a fresh handshake. Streaming requests (the reader page scans fields as it arrives)
 * are not retried; the pool's idle expiry keeps them off stale sockets instead. */
static char *http_request(const char *url, const char *body, const char *referer, size_t limit,
                          size_t *length, wr_http_chunk_fn chunk, void *context,
                          bool *fields_received, bool *post_started) {
    bool reused = false;
    char *data = http_request_once(url, body, referer, limit, length, chunk, context,
                                   fields_received, post_started, &reused);
    if (data || !reused || chunk != NULL || wr_cancelled() || !wr_online()) return data;
    ESP_LOGW("WEREAD", "HTTP retry on fresh connection url=%s", url);
    http_pool_flush();
    return http_request_once(url, body, referer, limit, length, chunk, context,
                             fields_received, post_started, NULL);
}

char *wr_http(const char *url, const char *body, const char *referer, size_t limit, size_t *length) {
    return http_request(url, body, referer, limit, length, NULL, NULL, NULL, NULL);
}

bool wr_http_reader_fields(const char *url, size_t limit, wr_http_chunk_fn chunk, void *context) {
    bool received = false;
    http_request(url, NULL, WR_HOST "/", limit, NULL, chunk, context, &received, NULL);
    return received;
}

char *wr_http_report(const char *body, const char *referer, bool *post_started) {
    return http_request("/web/book/read", body, referer, 16384, NULL, NULL, NULL, NULL, post_started);
}

static void *json_alloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static bool renew_session(void) {
    if (!session_cookie_has("wr_rt")) {
        ESP_LOGW("WEREAD", "session renewal unavailable refresh_cookie=0");
        return false;
    }
    static const char body[] = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";
    ESP_LOGI("WEREAD", "session renewal start refresh_cookie=1");
    s_expired = false;
    char *raw = wr_http("/web/login/renewal", body, NULL, 16384, NULL);
    if (!raw) {
        ESP_LOGW("WEREAD", "session renewal network/http failure expired=%d", s_expired);
        return false;
    }
    cJSON *json = cJSON_ParseWithOpts(raw, NULL, true);
    free(raw);
    if (!json) {
        s_expired = true;
        ESP_LOGW("WEREAD", "session renewal response parse failed");
        return false;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsObject(data)) data = json;
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(data, "succeed");
    if (!success) success = cJSON_GetObjectItemCaseSensitive(json, "succeed");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "errcode");
    if (!error) error = cJSON_GetObjectItemCaseSensitive(json, "errCode");
    const bool no_error = !error || (cJSON_IsNumber(error) && error->valueint == 0) ||
        (cJSON_IsString(error) && !strcmp(error->valuestring, "0"));
    const bool succeeded = cJSON_IsTrue(success) || (cJSON_IsNumber(success) && success->valueint == 1) ||
        (cJSON_IsString(success) && (!strcmp(success->valuestring, "1") || !strcmp(success->valuestring, "true")));
    bool ok = no_error && succeeded;
    cJSON_Delete(json);
    if (!ok) {
        s_expired = true;
        ESP_LOGW("WEREAD", "session renewal rejected");
        return false;
    }
    char token[sizeof(s_http_account.token)];
    if (s_http_account.token[0] && session_cookie_copy("wr_skey", token, sizeof(token)) &&
        strcmp(s_http_account.token, token)) {
        memcpy(s_http_account.token, token, sizeof(s_http_account.token));
        (void)save_account(&s_http_account);
    }
    s_expired = false;
    ESP_LOGI("WEREAD", "session renewal confirmed refresh_cookie=%d", session_cookie_has("wr_rt"));
    return true;
}

static cJSON *wr_json_limit(const char *url, const char *body, size_t limit) {
    for (unsigned attempt = 0; attempt < 2 && !wr_cancelled(); ++attempt) {
        char *data = wr_http(url, body, NULL, limit, NULL);
        if (!data) {
            if (attempt == 0 && s_expired && renew_session()) continue;
            return NULL;
        }
        // cJSON hooks are global; the other firmware clients can parse concurrently.
        cJSON *json = cJSON_ParseWithOpts(data, NULL, true);
        free(data);
        if (!json) { s_error_message = "书架数据解析失败"; return NULL; }
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "errcode");
        if (!error) error = cJSON_GetObjectItemCaseSensitive(json, "errCode");
        if ((cJSON_IsNumber(error) && error->valueint == -2012) ||
            (cJSON_IsString(error) && !strcmp(error->valuestring, "-2012"))) s_expired = true;
        if (attempt == 0 && s_expired && renew_session()) { cJSON_Delete(json); continue; }
        return json;
    }
    return NULL;
}

cJSON *wr_json(const char *url, const char *body) {
    return wr_json_limit(url, body, 3 * 1024 * 1024);
}

static FILE *open_shelf(const char *owner, wr_shelf_header_t *header) {
    if (!read_pico_sd_io_ready() || !owner[0]) return NULL;
    FILE *file = fopen(WR_INDEX, "rb");
    if (!file) file = fopen(WR_DIR "/shelf.prev", "rb");
    if (file && (fread(header, sizeof(*header), 1, file) != 1 ||
                 strncmp(header->owner, owner, sizeof(header->owner)) || header->count > WR_MAX_SHELF)) {
        fclose(file);
        file = NULL;
    }
    return file;
}

static bool owned_path(const char *owner, const char *id, char path[WR_PATH]) {
    if (!wr_safe(owner) || !wr_safe(id)) return false;
    wr_account_t account;
    if (!read_account(&account) || strcmp(account.owner,owner)) return false;
    // 直接落在书架根目录：放子目录会被扫描登记成「分类」，缓存完还要多点一层。
    // Land on the shelf root: a subdirectory turns into a shelf category and hides the book.
    const char *root = app_settings_books_dir();
    int n = snprintf(path, WR_PATH, "%s/wr-%08lx+%s.epub", root, (unsigned long)account.library, id);
    return n > 0 && n < WR_PATH;
}

static FILE *open_cached(const char *owner, const char *mode) {
    FILE *file = fopen(WR_CACHED,mode);
    if (!file) return NULL;
    wr_account_t account;
    uint32_t library;
    if (read_account(&account) && !strcmp(account.owner,owner) &&
        fread(&library,sizeof(library),1,file) == 1 && library == account.library) return file;
    fclose(file);
    return NULL;
}

static bool cached_record(const char *owner, const char *id) {
    FILE *file = open_cached(owner,"rb");
    if (!file) return false;
    wr_book_t item;
    bool found = false;
    for (unsigned i = 0; i < WR_MAX_SHELF && fread(&item,sizeof(item),1,file) == 1; ++i) {
        if (!strncmp(item.id,id,sizeof(item.id))) { found = true; break; }
    }
    fclose(file);
    return found;
}

static bool remember_cached(const wr_book_t *book) {
    if (cached_record(s_http_account.owner,book->id)) return true;
    FILE *file = open_cached(s_http_account.owner,"r+b");
    bool ok = true;
    if (!file) {
        file = fopen(WR_CACHED,"w+b");
        ok = file && fwrite(&s_http_account.library,sizeof(s_http_account.library),1,file) == 1;
    }
    ok = ok && !fseek(file,0,SEEK_END) && fwrite(book,sizeof(*book),1,file) == 1 && !fflush(file) && !fsync(fileno(file));
    if (file && fclose(file)) ok = false;
    return ok;
}

bool wr_book_path(const wr_book_t *book, char path[WR_PATH]) {
    char owner[WR_ID];
    wr_account(owner, NULL, NULL);
    if (!owned_path(owner, book->id, path)) return false;
    WR_SD_SCOPE(path);
    struct stat st;
    return scope.allowed && cached_record(owner,book->id) && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static int visible_shelf(int requested, wr_book_t *book) {
    WR_SD_SCOPE(WR_DIR);
    if (!scope.allowed) return 0;
    char owner[WR_ID];
    bool logged = wr_account(owner, NULL, NULL);
    wr_shelf_header_t header;
    FILE *file = logged ? open_shelf(owner,&header) : open_cached(owner,"rb");
    if (!file) return 0;
    int result = 0;
    if (logged && requested < 0) result = (int)header.count;
    else if (logged && requested < (int)header.count &&
             !fseek(file, (long)(sizeof(header) + (size_t)requested*sizeof(*book)), SEEK_SET)) {
        result = fread(book, sizeof(*book), 1, file) == 1;
    } else if (!logged) {
        wr_book_t record;
        char path[WR_PATH];
        struct stat st;
        bool found = false;
        for (unsigned i = 0; i < WR_MAX_SHELF && fread(&record, sizeof(record), 1, file) == 1; ++i) {
            if (!owned_path(owner, record.id, path) || stat(path, &st) || st.st_size <= 0) continue;
            if (requested == result) { *book = record; found = true; break; }
            ++result;
        }
        if (requested >= 0) result = found;
    }
    fclose(file);
    return result;
}

int wr_shelf_count(void) { return visible_shelf(-1, NULL); }
bool wr_shelf_get(int index, wr_book_t *book) { return index >= 0 && visible_shelf(index, book) == 1; }

static bool cover_path(const char *owner, const wr_book_t *book, char out[WR_PATH]) {
    if (!wr_safe(owner) || !wr_safe(book->id)) return false;
    return snprintf(out, WR_PATH, WR_DIR "/%s+%s.gray", owner, book->id) < WR_PATH;
}

bool wr_cover_get(const wr_book_t *book, uint8_t *pixels) {
    char owner[WR_ID], path[WR_PATH];
    wr_account(owner, NULL, NULL);
    if (!cover_path(owner, book, path)) return false;
    WR_SD_SCOPE(path);
    if (!scope.allowed) return false;
    struct stat st;
    if (stat(path, &st) || st.st_size != BOOK_COVER_W * BOOK_COVER_H) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fread(pixels, BOOK_COVER_W * BOOK_COVER_H, 1, file) == 1;
    fclose(file);
    return ok;
}

static bool ensure_directory(const char *path) {
    struct stat st;
    if (stat(path,&st)==0) {
        if (S_ISDIR(st.st_mode)) return true;
        errno = ENOTDIR;
        return false;
    }
    if (errno!=ENOENT) return false;
    for (unsigned attempt=0;attempt<3;++attempt) {
        if (mkdir(path,0775)==0) return true;
        int saved=errno;
        if (stat(path,&st)==0 && S_ISDIR(st.st_mode)) return true;
        if (saved!=EBUSY && saved!=EAGAIN && saved!=EINTR && saved!=ENOENT) { errno=saved; return false; }
        vTaskDelay(pdMS_TO_TICKS(40u*(attempt+1)));
    }
    return stat(path,&st)==0 && S_ISDIR(st.st_mode);
}

static void remove_previews(const char *owner) {
    if (!wr_safe(owner)) return;
    DIR *dir = opendir(WR_DIR);
    if (!dir) return;
    char prefix[WR_ID+2], path[WR_PATH];
    size_t prefix_size = snprintf(prefix,sizeof(prefix),"%s+",owner);
    struct dirent *item;
    while ((item = readdir(dir))) {
        size_t n = strlen(item->d_name);
        if (n > prefix_size+5 && !memcmp(item->d_name,prefix,prefix_size) && !strcmp(item->d_name+n-5,".gray") &&
            snprintf(path,sizeof(path),WR_DIR "/%s",item->d_name) < sizeof(path)) unlink(path);
    }
    closedir(dir);
}

static bool write_file(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    (void)setvbuf(file, NULL, _IOFBF, 4096);
    bool ok = fwrite(data, 1, size, file) == size && !fflush(file) && !fsync(fileno(file));
    if (fclose(file)) ok = false;
    if (!ok) unlink(path);
    return ok;
}

static bool preview_from_epub(const char *path, uint8_t *pixels) {
    uint8_t *data = NULL;
    size_t size;
    bool png;
    bool ok = book_epub_cover_bounded(path,&data,&size,&png,2*1024*1024) == ESP_OK &&
        book_cover_thumbnail(data,size,png,pixels);
    free(data);
    return ok;
}

static void sync_covers(unsigned first, unsigned count) {
    uint8_t *pixels = json_alloc(BOOK_COVER_W * BOOK_COVER_H);
    if (!pixels) return;
    unsigned downloaded = 0;
    for (unsigned i = 0; i < count && !wr_cancelled(); ++i) {
        int64_t cover_start = esp_timer_get_time();
        wr_book_t book;
        char path[WR_PATH], url[sizeof(book.cover)+8];
        if (!wr_shelf_get((int)(first+i), &book)) break;
        if (!cover_path(s_http_account.owner, &book, path)) continue;
        struct stat st;
        if (!stat(path,&st) && st.st_size == BOOK_COVER_W * BOOK_COVER_H) {
            wr_work_progress(++downloaded,count);
            continue;
        }
        if (!stat(path,&st)) unlink(path); /* discard a partial file left by an interrupted write */
        // 已缓存的书先用本地 EPUB 封面，避免同步时重复请求同一本书的图片。
        // Cached books use their EPUB cover first, avoiding another image request during sync.
        char epub[WR_PATH];
        bool local = wr_book_path(&book,epub);
        int64_t local_start = esp_timer_get_time();
        bool ok = local && preview_from_epub(epub,pixels);
        int64_t local_ms = (esp_timer_get_time() - local_start) / 1000;
        if (!ok && local) ESP_LOGW("WEREAD","cached EPUB has no usable cover book=%s",book.id);
        snprintf(url, sizeof(url), "%s%s", !strncmp(book.cover, "//", 2) ? "https:" : "", book.cover);
        size_t length=0;
        char *image=NULL;
        int64_t net_start = esp_timer_get_time();
        for (unsigned attempt=0;!ok && book.cover[0] && attempt<3 && !image && !wr_cancelled();++attempt) {
            image=wr_http(url,NULL,NULL,4*1024*1024,&length);
            if (!image && attempt<2) vTaskDelay(pdMS_TO_TICKS(300u*(attempt+1)));
        }
        int64_t net_ms = (esp_timer_get_time() - net_start) / 1000;
        int64_t decode_start = esp_timer_get_time();
        if (!ok) ok = image && book_cover_thumbnail((uint8_t *)image,length,length>=8 && (uint8_t)image[0]==0x89,pixels);
        int64_t decode_ms = (esp_timer_get_time() - decode_start) / 1000;
        int64_t write_start = esp_timer_get_time();
        if (ok) {
            char temp[WR_PATH];
            int n=snprintf(temp,sizeof(temp),"%s.part",path);
            ok=n>0 && (size_t)n<sizeof(temp) && write_file(temp,pixels,BOOK_COVER_W*BOOK_COVER_H) &&
                (unlink(path)==0 || errno==ENOENT) && rename(temp,path)==0;
            if (!ok && n>0 && (size_t)n<sizeof(temp)) unlink(temp);
        }
        int64_t write_ms = (esp_timer_get_time() - write_start) / 1000;
        ESP_LOGI("WEREAD", "cover stage book=%s local_ms=%lld net_ms=%lld bytes=%u decode_ms=%lld write_ms=%lld total_ms=%lld result=%d",
                 book.id, (long long)local_ms, (long long)net_ms, (unsigned)length,
                 (long long)decode_ms, (long long)write_ms,
                 (long long)((esp_timer_get_time()-cover_start)/1000), ok);
        if (!ok) ESP_LOGW("WEREAD","cover unavailable book=%s url=%s error=%s",book.id,book.cover,wr_error_message());
        else ++downloaded;
        free(image);
        wr_work_progress(downloaded, count);
    }
    free(pixels);
}

static bool sync_shelf(void) {
    /* Keep the shelf request independent from the small chapter JSON budget;
     * large libraries may exceed several megabytes.  The SAX adapter is being
     * introduced separately, so this compatibility path uses PSRAM-backed
     * storage with a generous firmware-wide ceiling. */
    cJSON *json = NULL;
    for (unsigned attempt = 0; attempt < 2 && !wr_cancelled(); ++attempt) {
        json = wr_json_limit("/web/shelf/sync", NULL, 16u * 1024u * 1024u);
        if (json || !s_http_open_failed || attempt != 0 || wr_cancelled()) break;
        ESP_LOGW("WEREAD", "shelf sync connect failed before HTTP response; retrying once");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!json) return false;
    cJSON *books = cJSON_GetObjectItemCaseSensitive(json, "books");
    if (!cJSON_IsArray(books) && cJSON_IsObject(json)) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (cJSON_IsObject(data)) books = cJSON_GetObjectItemCaseSensitive(data, "books");
    }
    ESP_LOGI("WEREAD", "sync response json=%d books=%d", cJSON_IsObject(json), cJSON_IsArray(books));
    int count = cJSON_GetArraySize(books);
    WR_SD_SCOPE(WR_DIR);
    if (!scope.allowed) { s_error_message = "SD 卡不可用"; cJSON_Delete(json); return false; }
    if (!cJSON_IsArray(books)) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(json,"errcode");
        if (!error) error = cJSON_GetObjectItemCaseSensitive(json,"errCode");
        if (cJSON_IsNumber(error) && error->valueint == -2012) {
            s_expired = true; s_error_message = "登录已失效，请重新扫码";
        } else s_error_message = "书架响应中缺少书籍列表";
        cJSON_Delete(json); return false;
    }
    if (count > WR_MAX_SHELF) { s_error_message = "书架书籍数量超出支持范围"; cJSON_Delete(json); return false; }
    if (!ensure_directory(WR_DIR)) { s_error_message = "无法创建书架索引目录"; cJSON_Delete(json); return false; }
    wr_book_t *records = json_alloc((size_t)(count+1)*sizeof(*records));
    wr_shelf_header_t header = {0};
    snprintf(header.owner, sizeof(header.owner), "%s", s_http_account.owner);
    cJSON *entry;
    if (records) cJSON_ArrayForEach(entry, books) {
        wr_book_t *record = &records[header.count];
        memset(record, 0, sizeof(*record));
        if (!wr_value(cJSON_GetObjectItemCaseSensitive(entry, "bookId"), record->id, sizeof(record->id)) || !wr_safe(record->id)) {
            continue;
        }
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry, "title"), record->title, sizeof(record->title));
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry, "author"), record->author, sizeof(record->author));
        if (!wr_value(cJSON_GetObjectItemCaseSensitive(entry, "cover"), record->cover, sizeof(record->cover))) record->cover[0] = 0;
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(entry, "readUpdateTime");
        record->updated = cJSON_IsNumber(date) && date->valuedouble >= 0 && date->valuedouble <= UINT32_MAX ? (uint32_t)date->valuedouble : 0;
        ++header.count;
    }
    cJSON_Delete(json);
    if (!records) { s_error_message = "内存不足，无法读取书架"; return false; }
    wr_work_progress(0, header.count);
    /* WeRead's response may include pinned/unfinished entries in a different
     * order from its visible recent-reading shelf. Sort by its update timestamp,
     * descending; stable ties retain server order. */
    for (unsigned i=1;i<header.count;++i) {
        wr_book_t value=records[i]; unsigned j=i;
        while (j && records[j-1].updated < value.updated) { records[j]=records[j-1]; --j; }
        records[j]=value;
    }
    bool ok = records && !wr_cancelled();
    if (ok) {
        FILE *file = fopen(WR_INDEX_TEMP, "wb");
        ok = file && fwrite(&header, sizeof(header), 1, file) == 1;
        for (unsigned i = 0; ok && i < header.count; ++i) ok = fwrite(&records[i], sizeof(*records), 1, file) == 1;
        if (file) {
            if (fflush(file) || fsync(fileno(file))) ok = false;
            if (fclose(file)) ok = false;
        }
        if (ok && !wr_cancelled()) {
            // FAT rename cannot replace an existing destination.
            const char *backup = WR_DIR "/shelf.prev";
            unlink(backup);
            bool moved = rename(WR_INDEX, backup) == 0;
            ok = (moved || access(WR_INDEX, F_OK) != 0) && rename(WR_INDEX_TEMP, WR_INDEX) == 0;
            if (ok) unlink(backup);
            else if (moved) rename(backup, WR_INDEX);
        } else ok = false;
        if (!ok) unlink(WR_INDEX_TEMP);
    }
    free(records);
    if (!ok && !wr_cancelled()) s_error_message = "书架索引写入 SD 卡失败";
    /* Cover network I/O belongs to explicit shelf sync only. Keep the reader
     * and shelf-entry paths local-only, and report successful cached covers. */
    if (ok && header.count) sync_covers(0, header.count);
    return ok;
}

static bool check_session(void) {
    int64_t started = esp_timer_get_time();
    char path[192];
    int path_size = snprintf(path, sizeof(path), "/web/shelf/sync?onlyBookid=1&cbcount=1&userVid=%s",
                             s_http_account.owner);
    if (path_size < 0 || (size_t)path_size >= sizeof(path) || !wr_safe(s_http_account.owner)) {
        s_error_message = "无法验证微信读书登录状态，请稍后重试";
        return false;
    }
    cJSON *response = wr_json(path, NULL);
    if (!response) return false;
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(response, "data");
    if (!cJSON_IsObject(payload)) payload = response;
    const char *const index_keys[] = {"bookIds", "bookids", "books", "updated"};
    bool valid = false;
    for (size_t i = 0; i < sizeof(index_keys) / sizeof(index_keys[0]); ++i) {
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(payload, index_keys[i]);
        if (cJSON_IsArray(index) || cJSON_IsObject(index)) { valid = true; break; }
    }
    cJSON_Delete(response);
    if (valid) {
        ESP_LOGI("WEREAD", "session check mode=light elapsed_ms=%lld", (long long)((esp_timer_get_time()-started)/1000));
        return true;
    }
    if (s_expired) return false;
    ESP_LOGW("WEREAD", "session check light response incompatible; falling back to full shelf");
    response = wr_json("/web/shelf/sync", NULL);
    if (!response) return false;
    const cJSON *books = cJSON_GetObjectItemCaseSensitive(response, "books");
    if (!cJSON_IsArray(books)) {
        payload = cJSON_GetObjectItemCaseSensitive(response, "data");
        books = cJSON_GetObjectItemCaseSensitive(payload, "books");
    }
    valid = cJSON_IsArray(books);
    cJSON_Delete(response);
    if (valid) ESP_LOGI("WEREAD", "session check mode=full elapsed_ms=%lld", (long long)((esp_timer_get_time()-started)/1000));
    else if (!s_expired) s_error_message = "无法验证微信读书登录状态，请稍后重试";
    return valid;
}

static bool login(void) {
    s_http_account.token[0] = 0;
    /* Start a new QR login without replaying a stale account's refresh cookie. */
    if (!clear_session_cookies()) {
        s_error_message = "无法清理旧登录态，请重试";
        return false;
    }
    cJSON *json = wr_json("/api/auth/getLoginUid", NULL);
    if (!json) return false;
    char uid[128], path[220];
    const cJSON *uid_field = cJSON_GetObjectItemCaseSensitive(json,"uid");
    if (!uid_field) uid_field = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json,"data"),"uid");
    bool ok = wr_value(uid_field, uid, sizeof(uid)) && wr_safe(uid);
    cJSON_Delete(json);
    if (!ok) return false;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_qr, sizeof(s_qr), WR_HOST "/web/confirm?uid=%s", uid);
    portEXIT_CRITICAL(&s_lock);
    snprintf(path, sizeof(path), "/api/auth/getLoginInfo?uid=%s&otp=", uid);
    for (int i = 0; i < 120 && !wr_cancelled(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        json = wr_json(path, NULL);
        if (!json) {
            if (s_expired) return false;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (!cJSON_IsObject(data)) data = json;
        const cJSON *success = cJSON_GetObjectItemCaseSensitive(data, "succeed");
        if (!success) success = cJSON_GetObjectItemCaseSensitive(json, "succeed");
        if (cJSON_IsTrue(success) || (cJSON_IsNumber(success) && success->valueint == 1) ||
            (cJSON_IsString(success) && (!strcmp(success->valuestring,"1") || !strcmp(success->valuestring,"true")))) {
            wr_account_t account = {0};
            const cJSON *vid = cJSON_GetObjectItemCaseSensitive(data, "webLoginVid");
            if (!vid) vid = cJSON_GetObjectItemCaseSensitive(data, "vid");
            if (!vid) vid = cJSON_GetObjectItemCaseSensitive(data, "userVid");
            if (!vid) vid = cJSON_GetObjectItemCaseSensitive(data, "user_vid");
            ok = wr_value(vid, account.owner, sizeof(account.owner)) && wr_safe(account.owner) &&
                wr_value(cJSON_GetObjectItemCaseSensitive(data, "accessToken"), account.token, sizeof(account.token)) &&
                wr_safe(account.token) && !wr_cancelled();
            account.library = strcmp(account.owner,s_http_account.owner) ? esp_random() : s_http_account.library;
            if (ok) ok = save_account(&account);
            cJSON_Delete(json);
            if (!ok) return false;
            if (strcmp(account.owner, s_http_account.owner)) {
                WR_SD_SCOPE(WR_DIR);
                if (scope.allowed) {
                    // 只清「微信读书书架」的索引；旧书转普通本地书，正文与封面都留下。
                    // Drop only the WeRead shelf index: the books become plain local books,
                    // so the EPUB and its cached cover stay on the card.
                    unlink(WR_INDEX);
                    unlink(WR_DIR "/shelf.prev");
                    unlink(WR_CACHED);
                }
                // 换账号：旧会话立即失效，避免把旧账号的时长记到新账号。
                // Account switched: retire the old session so its time can never bill the new one.
                radio_hold(false);
                memset(&s_reading, 0, sizeof(s_reading));
            }
            s_http_account = account;
            return true;
        }
        cJSON_Delete(json);
    }
    return false;
}

bool wr_logout(bool remove_books) {
    if (wr_status(NULL, 0, NULL, NULL) == WR_BUSY) return false;
    wr_account_t account;
    if (!read_account(&account)) return true;
    if (remove_books) {
        WR_SD_SCOPE(WR_DIR);
        if (!scope.allowed || !wr_safe(account.owner)) return false;
        char path[WR_PATH];
        FILE *files = open_cached(account.owner,"rb");
        bool ok = true;
        wr_book_t item;
        while (files && fread(&item,sizeof(item),1,files) == 1) {
            if (!owned_path(account.owner,item.id,path) || (unlink(path) && errno != ENOENT)) { ok = false; break; }
            (void)book_progress_forget(path);
            (void)book_title_clear(path);
        }
        if (files) { ok = ok && !ferror(files); fclose(files); }
        if (!ok) return false;
        remove_previews(account.owner);
        if (unlink(WR_INDEX) && errno != ENOENT) return false;
        if (unlink(WR_DIR "/shelf.prev") && errno != ENOENT) return false;
        if (unlink(WR_CACHED) && errno != ENOENT) return false;
    }
    memset(account.token, 0, sizeof(account.token));
    if (!save_account(&account)) return false;
    return clear_session_cookies();
}

static void run_work(const wr_work_t *work) {
    char path[WR_PATH];
    const int64_t work_started_us = esp_timer_get_time();
    bool ok = false;
    const bool cache_radio_boost = work->kind == WR_CACHE && wr_online();
    if (cache_radio_boost) {
        radio_hold(true);
        ESP_LOGI("WEREAD", "cache radio performance mode enabled");
    }
    if (work->kind == WR_LOGIN) s_report_fields.expires = 0;
    ESP_LOGI("WEREAD", "work start kind=%d internal_free=%u largest=%u", work->kind,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_expired = false;
    s_error_message = "操作失败，请检查网络后重试";
    read_account(&s_http_account);
    s_cookies[0] = 0;
    restore_session_cookie();
    if (!s_cookies[0] && s_http_account.token[0])
        snprintf(s_cookies,sizeof(s_cookies),"wr_vid=%s; wr_skey=%s",s_http_account.owner,s_http_account.token);
    ESP_LOGI("WEREAD", "session cookies restored vid=%d skey=%d refresh=%d",
             session_cookie_has("wr_vid"), session_cookie_has("wr_skey"), session_cookie_has("wr_rt"));
    if (time(NULL) < 1704067200 && wr_online()) {
        uint32_t utc;
        (void)read_pico_transfer_sync_time_online(&utc);
    }
    if (work->kind == WR_LOGIN) ok = login();
    else if (s_http_account.token[0] && !wr_cancelled()) {
        switch (work->kind) {
        case WR_CHECK: ok = check_session(); break;
        case WR_SYNC: ok = sync_shelf(); break;
        case WR_CACHE: {
            WR_SD_SCOPE(WR_DIR);
            if (!scope.allowed) { s_error_message="缓存失败：SD 卡当前不可用"; ok=false; }
            else if (!ensure_directory(WR_DIR)) { s_error_message="缓存失败：无法创建缓存目录"; ok=false; }
            else if (!owned_path(s_http_account.owner,work->book.id,path)) { s_error_message="缓存失败：书籍标识无效"; ok=false; }
            else if (!ensure_directory(app_settings_books_dir())) { s_error_message="缓存失败：无法创建书籍保存目录"; ok=false; }
            else ok=wr_cache(&work->book,path);
            if (!ok) ESP_LOGE("WEREAD","cache failed book=%s phase=download-or-package error=%s errno=%d",work->book.id,s_error_message,errno);
            if (ok) {
                ok = remember_cached(&work->book);
                if (ok && work->book.title[0]) (void)book_title_set(path,work->book.title);
                if (!ok) s_error_message="缓存失败：书籍已下载，但本地缓存索引写入失败";
            }
            break;
        }
        case WR_UPLOAD: case WR_DOWNLOAD: {
            const char *why = NULL;
            if (!wr_book_path(&work->book, path)) {
                s_error_message = "这本书还没有缓存，无法同步进度";
                break;
            }
            ok = wr_progress(&work->book, path, work->kind == WR_UPLOAD, &why);
            // 需求要求区分「没有本地进度」「无法定位章节」，不再统一报网络错误。
            if (!ok && why) s_error_message = why;
            break;
        }
        default: break;
        }
    } else if (!wr_cancelled()) {
        s_error_message = "登录已失效，请重新扫码";
        s_expired = true;
    }
    if (cache_radio_boost) {
        radio_hold(false);
        ESP_LOGI("WEREAD", "cache radio performance mode restored");
    }
    if (s_expired && s_http_account.token[0]) {
        memset(s_http_account.token, 0, sizeof(s_http_account.token));
        (void)save_account(&s_http_account);
    }
    memset(&s_http_account, 0, sizeof(s_http_account));
    memset(s_cookies, 0, sizeof(s_cookies));
    ESP_LOGI("WEREAD","work done kind=%d ok=%d canceled=%d elapsed_ms=%lld stack_hwm=%u internal_free=%u psram_free=%u",
             work->kind,ok,wr_cancelled(),(long long)((esp_timer_get_time()-work_started_us)/1000),
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    portENTER_CRITICAL(&s_lock);
    s_qr[0] = 0;
    s_result_message = s_error_message;
    s_result = s_expired ? WR_EXPIRED : wr_cancelled() ? WR_CANCELLED : ok ? WR_OK : WR_FAILED;
    portEXIT_CRITICAL(&s_lock);
}

void wr_work_progress(unsigned done, unsigned total) {
    portENTER_CRITICAL(&s_lock);
    s_done = done;
    s_total = total;
    portEXIT_CRITICAL(&s_lock);
}

bool wr_start(wr_job_t job, const wr_book_t *book, unsigned first, unsigned count) {
    portENTER_CRITICAL(&s_lock);
    bool accepted = s_worker && s_result != WR_BUSY;
    if (accepted) {
        s_work.kind = job;
        s_work.first = first;
        s_work.count = count;
        if (book) s_work.book = *book;
        else memset(&s_work.book, 0, sizeof(s_work.book));
        s_work_pending = true;
        atomic_store(&s_cancel, false);
        s_result = WR_BUSY;
        s_active_job = job;
        s_done = s_total = 0;
        s_qr[0] = 0;
    } else if (!s_worker) {
        s_result_message = "微信读书后台未初始化，请重启设备";
    }
    portEXIT_CRITICAL(&s_lock);
    if (accepted) xTaskNotifyGive(s_worker);
    return accepted;
}

wr_result_t wr_status(char *qr, size_t qr_size, unsigned *done, unsigned *total) {
    portENTER_CRITICAL(&s_lock);
    wr_result_t result = s_result;
    if (qr && qr_size) {
        size_t n = strnlen(s_qr, qr_size-1);
        memcpy(qr, s_qr, n);
        qr[n] = 0;
    }
    if (done) *done = s_done;
    if (total) *total = s_total;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

/* 射频初始化前要确认这里没有活：任务在跑时池里的 TLS 连接会占住几 KB 内部 RAM。 */
/* Check this before arming the radio: a running job holds pooled TLS connections and their internal RAM. */
bool wr_network_busy(void) {
    portENTER_CRITICAL(&s_lock);
    bool busy = s_work_pending || s_result == WR_BUSY;
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

bool wr_reporting_book(const char *path, wr_book_t *book) {
    char expected[WR_PATH], prefix[16];
    wr_account_t account;
    if (!read_account(&account) || !account.token[0]) return false;
    const char *base = strrchr(path, '/');
    if (!base) return false;
    snprintf(prefix, sizeof(prefix), "/wr-%08lx+", (unsigned long)account.library);
    if (strncmp(base, prefix, strlen(prefix))) return false;
    const char *id = base + strlen(prefix), *ext = strrchr(id, '.');
    if (!ext || strcmp(ext, ".epub") || ext == id || (size_t)(ext-id) >= WR_ID) return false;
    memset(book, 0, sizeof(*book));
    memcpy(book->id, id, ext-id);
    return owned_path(account.owner, book->id, expected) && !strcmp(expected, path) && cached_record(account.owner,book->id);
}

typedef enum { REPORT_RETRY, REPORT_CONFIRMED, REPORT_UNKNOWN, REPORT_ACCOUNT_CHANGED } report_result_t;

static bool same_report_book(const wr_report_t *a, const wr_report_t *b) {
    return a->library == b->library && !strcmp(a->owner, b->owner) && !strcmp(a->book_id, b->book_id);
}

static report_result_t run_report(const wr_report_t *job, const char **stage) {
    *stage = "account";
    if (!account_is(job->owner, job->library)) return REPORT_ACCOUNT_CHANGED;
    *stage = "wifi";
    if (!wr_online()) return REPORT_RETRY;
    wr_chapter_t chapter;
    unsigned offset, pct;
    *stage = "get_progress";
    if (!wr_remote_position(job->book_id, &chapter, &offset, &pct)) return REPORT_RETRY;

    bool cached = s_report_fields.expires > esp_timer_get_time() &&
        same_report_book(job, &s_report_fields.account_book) && !strcmp(chapter.id, s_report_fields.chapter);
    ESP_LOGI("WEREAD", "report reader fields cache_hit=%d seconds=%u", cached, job->seconds);
    if (!cached) {
        *stage = "reader_fields";
        if (report_display_busy()) return REPORT_RETRY;
        s_report_fields.expires = 0;
        if (!wr_reader(job->book_id, chapter.id, &s_report_fields.fields)) return REPORT_RETRY;
        s_report_fields.account_book = *job;
        memcpy(s_report_fields.chapter, chapter.id, sizeof(chapter.id));
        s_report_fields.expires = esp_timer_get_time() + 300000000;
    }

    *stage = "check_progress";
    wr_chapter_t current;
    unsigned current_offset, current_pct;
    if (!wr_remote_position(job->book_id, &current, &current_offset, &current_pct) ||
        strcmp(current.id, chapter.id) || current_offset != offset || current_pct != pct)
        return REPORT_RETRY;
    if (!account_is(job->owner, job->library)) return REPORT_ACCOUNT_CHANGED;
    *stage = "submit";
    if (report_display_busy()) return REPORT_RETRY;
    bool post_started = false;
    if (!wr_send_duration(job->book_id, &chapter, offset, pct, job->seconds,
                          &s_report_fields.fields, &post_started)) {
        if (post_started) s_report_fields.expires = 0;
        return post_started ? REPORT_UNKNOWN : REPORT_RETRY;
    }
    if (account_is(job->owner, job->library) && !add_stats(job->owner, job->seconds))
        ESP_LOGW("WEREAD", "report confirmed but local stats write failed seconds=%u", job->seconds);
    return REPORT_CONFIRMED;
}

static void process_report(unsigned slot, const wr_report_t *job) {
    const char *stage;
    s_processing_report = true;
    report_result_t result = run_report(job, &stage);
    s_processing_report = false;
    memset(s_cookies, 0, sizeof(s_cookies));
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    wr_report_t *pending = &s_reports[slot];
    if (result == REPORT_RETRY) {
        if (pending->failures < 5) ++pending->failures;
        pending->retry_at = now + (2000000LL << pending->failures);
    } else {
        pending->seconds -= job->seconds;
        pending->retry_at = 0;
        pending->failures = 0;
        if (result == REPORT_ACCOUNT_CHANGED) pending->seconds = 0;
    }
    unsigned remaining = pending->seconds;
    portEXIT_CRITICAL(&s_lock);
    const char *outcome = result == REPORT_CONFIRMED ? "confirmed" :
        result == REPORT_RETRY ? "retry" : result == REPORT_UNKNOWN ? "unknown" : "account_changed";
    ESP_LOGI("WEREAD", "report result=%s stage=%s seconds=%u pending=%u",
             outcome, stage, job->seconds, remaining);
}

static void worker_task(void *arg) {
    (void)arg;
    for (;;) {
        int64_t now = esp_timer_get_time();
        int slot = -1;
        bool pending = false;
        wr_report_t report;
        portENTER_CRITICAL(&s_lock);
        bool work = s_work_pending;
        if (work) s_work_pending = false;
        if (!work) {
            bool reader_idle = !s_reader_scanning && (!s_reader_scan_end || now - s_reader_scan_end >= 1200000);
            for (unsigned i = 0; i < sizeof(s_reports)/sizeof(s_reports[0]); ++i) {
                if (!s_reports[i].seconds) continue;
                pending = true;
                if (reader_idle && s_reports[i].retry_at <= now &&
                    (slot < 0 || s_reports[i].retry_at < report.retry_at)) {
                    slot = (int)i;
                    report = s_reports[i];
                }
            }
        }
        portEXIT_CRITICAL(&s_lock);

        if (work) {
            // 发布完成前，WR_BUSY 保证任务参数不能被覆盖。/ WR_BUSY protects the job until its result is published.
            run_work(&s_work);
            /* 任务结束就释放长连接：空闲期不再占 TLS 上下文，也不让射频为保活常驻。 */
            /* Drop keep-alive sockets once the job ends: no idle TLS memory, no radio keep-alive. */
            http_pool_flush();
        } else if (slot >= 0) {
            process_report((unsigned)slot, &report);
            http_pool_flush();
        } else {
            ulTaskNotifyTake(pdTRUE, pending ? pdMS_TO_TICKS(500) : portMAX_DELAY);
        }
    }
}

void wr_init(void) {
    if (s_worker) return;
    if (!s_cookie_mutex) s_cookie_mutex = xSemaphoreCreateMutexStatic(&s_cookie_mutex_storage);
    if (!s_pool_mutex) s_pool_mutex = xSemaphoreCreateMutexStatic(&s_pool_mutex_storage);
    if (!s_cookie_mutex) {
        ESP_LOGE("WEREAD", "cookie mutex initialization failed");
        return;
    }
    s_worker = xTaskCreateStatic(worker_task, "weread", sizeof(s_worker_stack),
                                 NULL, 2, s_worker_stack, &s_worker_tcb);
    if (s_worker) ESP_LOGI("WEREAD", "worker ready stack=%u", (unsigned)sizeof(s_worker_stack));
    else ESP_LOGE("WEREAD", "worker initialization failed");
}

void wr_reading_start(const char *path, bool enabled, int64_t now_ms) {
    wr_reading_stop(true);
    if (!enabled || !wr_reporting_book(path, &s_reading.book)) return;
    wr_account_t account;
    if (!read_account(&account) || !account.token[0]) { memset(&s_reading, 0, sizeof(s_reading)); return; }
    snprintf(s_reading.owner, sizeof(s_reading.owner), "%s", account.owner);
    s_reading.library = account.library;
    s_reading.attached = true;
    s_reading.last = now_ms;
    // 需求：这次阅读的上报期间保持 Wi-Fi 启用。
    radio_hold(true);
}

static void report_reading(void) {
    const unsigned seconds = s_reading.ms / 1000;
    if (!seconds || !s_reading.owner[0] || !s_worker) return;
    wr_report_t job = { .library = s_reading.library, .seconds = seconds };
    memcpy(job.book_id, s_reading.book.id, sizeof(job.book_id));
    memcpy(job.owner, s_reading.owner, sizeof(job.owner));
    portENTER_CRITICAL(&s_lock);
    wr_report_t *empty = NULL, *entry = NULL;
    for (unsigned i = 0; i < sizeof(s_reports)/sizeof(s_reports[0]); ++i) {
        if (!s_reports[i].seconds) {
            if (!empty) empty = &s_reports[i];
        } else if (same_report_book(&job, &s_reports[i])) {
            entry = &s_reports[i];
            break;
        }
    }
    bool accepted = false;
    if (entry && seconds <= UINT32_MAX - entry->seconds) {
        entry->seconds += seconds;
        accepted = true;
    } else if (!entry && empty) {
        *empty = job;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!accepted) return;
    s_reading.ms %= 1000;
    xTaskNotifyGive(s_worker);
}

void wr_reading_tick(bool active, int64_t now_ms) {
    if (!s_reading.attached) return;
    if (!s_reading.last) { s_reading.last = now_ms; return; }
    int64_t elapsed = now_ms - s_reading.last;
    s_reading.last = now_ms;
    // 断网只停发送不停计时：连接恢复后把这段一起补上（需求 4「暂停发送，恢复后继续」）。
    if (active && elapsed > 0 && elapsed <= 5000) s_reading.ms += (unsigned)elapsed;
    if (!active) radio_hold(false);
    else if (wr_online()) radio_hold(true);
    if (s_reading.ms >= 60000) report_reading();
}

void wr_reading_stop(bool close) {
    if (s_reading.attached) report_reading();
    if (close && s_reading.ms >= 1000)
        ESP_LOGW("WEREAD", "report queue unavailable at close seconds=%u", s_reading.ms / 1000);
    radio_hold(false);
    s_reading.last = 0;
    if (close) memset(&s_reading, 0, sizeof(s_reading));
}
