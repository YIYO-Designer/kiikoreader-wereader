#include "weread_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "book_cover.h"
#include "book_epub.h"
#include "book_font_size.h"
#include "book_progress.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "read_pico_sd_access.h"
#include "zip_reader.h"
#include "zlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct { FILE *file, *directory; uint16_t count; } epub_writer_t;

/* 下载重叠开关：本章的 tar 资源包（15 MB，带宽受限）下载期间，用第二条连接把下一章的
 * reader 页 + e_0/e_1/e_3（每章约 1.4 s，延迟受限）抓完。
 * 依据：psvts 藏在 reader 页末尾，每次都得下满 38~43 KB（full_response=1），32 章共 28.1 s；
 * e_0/e_1/e_3 每片才 200 ms 却要串三片，共 19.6 s。这两段都是"等包"而不是"吃带宽"，
 * 串在 tar 前面等于让链路空转。实测串行 96.7 s 里 93 s 全是 HTTP，重叠是唯一还有空间的改法。
 * 两条流走不同主机（reader/e_* 走 weread.qq.com，tar 走 res.weread.qq.com），连接池按 host
 * 分槽且带锁，不会互抢；TLS 与内容缓冲都在 PSRAM（MBEDTLS_EXTERNAL_MEM_ALLOC），不吃内部 RAM。
 * Chapter tarballs are bandwidth-bound; the reader page plus shards of the next chapter are
 * latency-bound. Running them back to back leaves the link idle during latency. The two streams
 * use different hosts, so the locked per-host connection pool keeps them apart.
 * Flip to 0 to fall back to the fully serial path for A/B measurement. */
#ifndef WR_PREFETCH_CHAPTER
#define WR_PREFETCH_CHAPTER 1
#endif

/* 合成 psvts 的快路：实测服务端会校验 ps 的真实性（两次都被拒），默认关闭，留作
 * 协议变化时的备用开关。连续两次被拒也会自动关闭。
 * Synthetic-psvts fast path: the server validates ps (two rejections on hardware), so it
 * stays off by default and is kept only as a switch for protocol changes. Two rejections
 * in a row also turn it off automatically. */
/* 真实 reader 页的 ps 复用开关，默认关闭：实测 32 章里只有 2 次命中，其余章拿上一章的
 * ps 都被服务端拒；而拒一次不是快速报错，是连接一直挂到 7.4 s 超时再退回真实 reader 页，
 * 两次拒就亏掉 14.8 s，净收益是负的（139 s 那轮也一样：命中 5 次约省 5 s，被拒 2 次亏 15 s）。
 * 留着这个开关是因为协议变化时它仍然是最省一次 reader 页的办法。
 * Real-ps reuse is off by default: across 32 chapters only 2 hits, and a rejection is not a
 * fast error - the connection hangs until the 7.4 s connect timeout before falling back to a
 * real reader page, so two rejections cost 14.8 s and the net effect is negative. The switch
 * stays because it is still the cheapest way to skip a reader page if the protocol changes. */
static char s_reuse_book[WR_ID];
static char s_reuse_ps[128];
static unsigned s_reuse_hits, s_reuse_misses, s_reuse_rejects;
static bool s_reuse_ok = false;

static char *cache_fetch_chapter(const wr_book_t *book, const wr_chapter_t *chapter,
                                 bool *plain, size_t *size, unsigned index) {
    int64_t started = esp_timer_get_time();
    wr_reader_t reader;
    char *content = NULL;
    bool reader_ready = false, reused = false;
    for (unsigned attempt = 0; attempt < 3 && !content && !wr_cancelled(); ++attempt) {
        /* 同一本书里复用上一章真实的 ps，跳过这次的 reader 页；失败才补抓。
         * Reuse the real ps of the previous chapter in the same book to skip this
         * chapter's reader page, fetching it only when the reuse is rejected. */
        if (!reader_ready) {
            if (s_reuse_ok && s_reuse_ps[0] && !strcmp(s_reuse_book, book->id))
                reused = wr_reader_from_ps(book->id, chapter->id, s_reuse_ps, &reader);
            reader_ready = reused || wr_reader(book->id, chapter->id, &reader);
            if (reader_ready && !reused) {
                snprintf(s_reuse_ps, sizeof(s_reuse_ps), "%s", reader.ps);
                snprintf(s_reuse_book, sizeof(s_reuse_book), "%s", book->id);
                s_reuse_misses = 0;
            }
        }
        if (reader_ready) content = wr_chapter_content(book->id, chapter->id, &reader, plain, size);
        if (!content && reused) {
            /* 服务端不认这个 ps：立刻改抓真实 reader 页，并计入失败预算。
             * The server rejected the reused ps: fetch the real page and count the miss. */
            ESP_LOGW("WEREAD", "cache reader reuse rejected index=%u error=%s",
                     index + 1, wr_error_message());
            ++s_reuse_rejects;
            if (++s_reuse_misses >= 2) s_reuse_ok = false;
            reused = false;
            reader_ready = wr_reader(book->id, chapter->id, &reader);
            if (reader_ready) {
                snprintf(s_reuse_ps, sizeof(s_reuse_ps), "%s", reader.ps);
                snprintf(s_reuse_book, sizeof(s_reuse_book), "%s", book->id);
                content = wr_chapter_content(book->id, chapter->id, &reader, plain, size);
            }
        } else if (content && reused) {
            ++s_reuse_hits;
        }
        if (!content && attempt < 2) {
            /* 记下每次重试的失败原因与退避：连撞三次同一个错误说明服务端在拒绝，
             * 而不是网络抖一下。 / Log why each retry happens.
             * Three identical failures mean the server is refusing, not a blip. */
            ESP_LOGW("WEREAD", "cache retry index=%u attempt=%u delay_ms=%u error=%s",
                     index + 1, attempt + 1, 400u * (attempt + 1), wr_error_message());
            vTaskDelay(pdMS_TO_TICKS(400u * (attempt + 1)));
        }
    }
    if (!content) {
        if (!wr_cancelled() && !strcmp(wr_error_message(), "操作失败，请检查网络后重试"))
            wr_error_set("缓存失败：章节下载失败（已自动重试 3 次）");
        ESP_LOGW("WEREAD", "cache fetch failed index=%u chapter=%s error=%s",
                 index + 1, chapter->id, wr_error_message());
        return NULL;
    }
    ESP_LOGI("WEREAD", "cache fetch index=%u bytes=%u elapsed_ms=%lld result=1",
             index + 1, (unsigned)*size,
             (long long)((esp_timer_get_time() - started) / 1000));
    return content;
}

#if WR_PREFETCH_CHAPTER
/* 一个常驻的预取任务：抓 next 章的 reader 页与正片，主任务在 tar 下载期间把它喂满。
 * 只在 job/done 两个二值信号量上同步，交接的字段都由 done 保证可见性，不再加锁。
 * 任务常驻不销毁——每本书建一次再删会在收尾处和正在退出的任务抢信号量。
 * One resident prefetch task hands the next chapter's reader page and content back over two
 * binary semaphores. It is never torn down per book: deleting it at the end of a run races
 * the task against semaphore release. */
typedef struct {
    SemaphoreHandle_t job, done;
    const wr_book_t *book;
    const wr_chapter_t *chapter;
    char *content;
    size_t size;
    bool plain, ok;
} wr_prefetch_t;
static wr_prefetch_t s_prefetch;
static bool s_prefetch_armed;

static void prefetch_task(void *arg) {
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_prefetch.job, portMAX_DELAY) != pdTRUE) continue;
        char *content = NULL;
        size_t size = 0;
        bool plain = false;
        wr_reader_t reader;
        /* 取消时 wr_reader 内部会提前断流，这里不必额外判断，失败就走串行兜底。 */
        if (wr_reader(s_prefetch.book->id, s_prefetch.chapter->id, &reader))
            content = wr_chapter_content(s_prefetch.book->id, s_prefetch.chapter->id, &reader, &plain, &size);
        s_prefetch.content = content;
        s_prefetch.size = size;
        s_prefetch.plain = plain;
        s_prefetch.ok = content != NULL;
        xSemaphoreGive(s_prefetch.done);
    }
}

static bool prefetch_start(void) {
    if (s_prefetch.job) return true;
    static StaticSemaphore_t job_storage, done_storage;
    s_prefetch.job = xSemaphoreCreateBinaryStatic(&job_storage);
    s_prefetch.done = xSemaphoreCreateBinaryStatic(&done_storage);
    if (!s_prefetch.job || !s_prefetch.done) return false;
    /* 栈放 PSRAM：内部 RAM 只剩 40 KB 左右，还要留给 Wi-Fi 的连续 DMA 块。
     * Stack lives in PSRAM; internal RAM is needed for the Wi-Fi contiguous DMA pool. */
    TaskHandle_t task = NULL;
    if (xTaskCreateWithCaps(prefetch_task, "wrpref", 12 * 1024, NULL, 2, &task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS || !task) {
        ESP_LOGW("WEREAD", "prefetch task create failed, falling back to serial fetch");
        vSemaphoreDelete(s_prefetch.job);
        vSemaphoreDelete(s_prefetch.done);
        s_prefetch.job = s_prefetch.done = NULL;
        return false;
    }
    ESP_LOGI("WEREAD", "cache prefetch task ready");
    return true;
}

/* 取回上一轮预取的结果；没有预取或失败都返回 NULL，调用方回落到串行抓取。 */
static char *prefetch_take(bool *plain, size_t *size) {
    if (!s_prefetch_armed) return NULL;
    s_prefetch_armed = false;
    if (xSemaphoreTake(s_prefetch.done, portMAX_DELAY) != pdTRUE) return NULL;
    if (!s_prefetch.ok) {
        ESP_LOGW("WEREAD", "cache prefetch miss error=%s", wr_error_message());
        return NULL;
    }
    *plain = s_prefetch.plain;
    *size = s_prefetch.size;
    return s_prefetch.content;
}

static void prefetch_arm(const wr_book_t *book, const wr_chapter_t *chapter) {
    if (!prefetch_start()) return;
    /* 正常情况下没有上一轮遗留；万一有（取消、出错）先收掉，避免两章内容串位。 */
    if (s_prefetch_armed) {
        size_t size = 0;
        bool plain = false;
        free(prefetch_take(&plain, &size));
    }
    s_prefetch.book = book;
    s_prefetch.chapter = chapter;
    s_prefetch_armed = true;
    xSemaphoreGive(s_prefetch.job);
}
#endif

static void le16(uint8_t *dest, unsigned value) {
    dest[0] = value;
    dest[1] = value >> 8;
}

static void le32(uint8_t *dest, uint32_t value) {
    le16(dest, value);
    le16(dest+2, value >> 16);
}

static bool zip_entry(epub_writer_t *zip, const char *name, const void *data, size_t size) {
    if (zip->count >= ZIP_ENTRY_MAX || size > ZIP_OUTPUT_MAX) return false;
    uint8_t local[30] = {0}, central[46] = {0};
    long offset = ftell(zip->file);
    if (offset < 0) return false;
    unsigned name_size = strlen(name);
    uint32_t crc = crc32(0, data, size);
    le32(local, 0x04034b50);
    le16(local+4, 20);
    le32(local+14, crc);
    le32(local+18, size);
    le32(local+22, size);
    le16(local+26, name_size);
    le32(central, 0x02014b50);
    le16(central+4, 20);
    memcpy(central+6, local+4, 24);
    le32(central+42, offset);
    ++zip->count;
    return fwrite(local, 1, sizeof(local), zip->file) == sizeof(local) &&
        fwrite(name, 1, name_size, zip->file) == name_size && fwrite(data, 1, size, zip->file) == size &&
        fwrite(central, 1, sizeof(central), zip->directory) == sizeof(central) &&
        fwrite(name, 1, name_size, zip->directory) == name_size;
}

static bool zip_finish(epub_writer_t *zip) {
    long start = ftell(zip->file), size = ftell(zip->directory);
    if (start < 0 || size < 0 || fseek(zip->directory, 0, SEEK_SET)) return false;
    uint8_t buffer[1024];
    for (long remaining = size; remaining > 0;) {
        size_t n = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        if (fread(buffer, 1, n, zip->directory) != n || fwrite(buffer, 1, n, zip->file) != n) return false;
        remaining -= n;
    }
    uint8_t end[22] = {0};
    le32(end, 0x06054b50);
    le16(end+8, zip->count);
    le16(end+10, zip->count);
    le32(end+12, size);
    le32(end+16, start);
    return fwrite(end, 1, sizeof(end), zip->file) == sizeof(end);
}

static bool write_xml(FILE *file, const char *text, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const char *escaped = NULL;
        switch (text[i]) {
        case '&': escaped = "&amp;"; break;
        case '<': escaped = "&lt;"; break;
        case '>': escaped = "&gt;"; break;
        case '"': escaped = "&quot;"; break;
        case '\'': escaped = "&apos;"; break;
        default: break;
        }
        if (escaped ? fputs(escaped, file) == EOF : fputc((unsigned char)text[i], file) == EOF) return false;
    }
    return true;
}

static bool file_entry(epub_writer_t *zip, const char *name, FILE *entry, size_t max) {
    long end = ftell(entry);
    if (end < 0 || (size_t)end > max || fseek(entry, 0, SEEK_SET)) return false;
    char *data = heap_caps_malloc((size_t)end+1, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!data) return false;
    bool ok = fread(data, 1, end, entry) == (size_t)end && zip_entry(zip, name, data, end);
    free(data);
    return ok;
}

static const char *image_extension(const char *data, size_t length, const char **mime) {
    if (length >= 3 && !memcmp(data, "\xff\xd8\xff", 3)) { *mime = "image/jpeg"; return "jpg"; }
    if (length >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) { *mime = "image/png"; return "png"; }
    if (book_webp_is((const uint8_t *)data, length)) { *mime = "image/webp"; return "webp"; }
    if (book_gif_is((const uint8_t *)data, length)) { *mime = "image/gif"; return "gif"; }
    return NULL;
}

/* ---------------- 章节图片并行预取 ----------------
 * 图片下载是缓存耗时的绝对大头，而主线程在两张图之间只是在拼 zip。所以让一个
 * 后台任务与主线程各下一张：主线程仍按文档顺序取用、仍独占 zip 写入，后台任务
 * 只比它多走一步，于是两路下载始终并行，文件结构完全不受影响。
 * Images dominate cache time, and the main thread only assembles zip between two of
 * them. A background task downloads one image while the main thread downloads another;
 * the main thread still consumes in document order and stays the only zip writer. */
#define WR_IMG_SLOTS 12
#define WR_IMG_URL_BYTES 1536
/* 线程栈从内部 RAM 动态分配、每章用完立刻归还：这块板子的内部 RAM 是稀缺资源，
 * Wi-Fi 驱动初始化一次就要几十 KB，常驻的静态栈会让「打开传书页」直接 NO_MEM。
 * The stack comes from internal RAM on demand and is returned after every chapter:
 * internal RAM is scarce here and a resident static stack starves Wi-Fi init. */
#define WR_IMG_STACK_BYTES (8 * 1024)
/* 内部 RAM 低于此值时不开第二个下载流：面板和 Wi-Fi 驱动也要内部 RAM。 */
#define WR_IMG_MIN_INTERNAL (28 * 1024)
/* 章节资源包上限：一本书单章插图最大也就几 MB，超了就退回逐张下载。 */
#define WR_IMG_TAR_LIMIT (8 * 1024 * 1024)
/* 2026-11-16 实测：两条并行 TLS 流会把单流吞吐从 100–300 KB/s 打到 8–40 KB/s
 * （同一本书、同一 AP，单流 476 KB/12 s，双流时 172 KB 的 reader 页要 25 s）。
 * 无线侧只有 6 个静态 TX 缓冲，两条流的 ACK 互相挤占后整段变慢，因此默认串行；
 * 想再试并行把这里改回 1。
 * Measured on 2026-11-16: two concurrent TLS streams drop per-stream throughput from
 * 100–300 KB/s to 8–40 KB/s (same book and AP). The radio has only 6 static TX
 * buffers, so the two ACK streams starve each other. Serial by default; flip to 1 to
 * re-experiment with the parallel prefetch. */
#define WR_IMG_PARALLEL 0

typedef struct {
    char url[WR_IMG_URL_BYTES];
    /* 文件名（去掉目录和查询串）：tar 里的条目名是 `<章>/epub_<book>_<n>`，
     * 正文里是 `https://res.weread.qq.com/wrepub/epub_<book>_<n>`，靠这个对上。
     * Basename only: tar entries are named `<chapter>/epub_<book>_<n>` while the body
     * references `https://res.weread.qq.com/wrepub/epub_<book>_<n>`. */
    char key[96];
    char *data;
    size_t size;
    bool ready;
} wr_img_slot_t;

typedef struct {
    wr_img_slot_t slot[WR_IMG_SLOTS];
    unsigned count, claimed, high_water;
    bool busy;
    volatile bool abort;
} wr_img_job_t;

static EXT_RAM_BSS_ATTR wr_img_job_t s_img_job;
static StaticSemaphore_t s_img_mutex_storage;
static SemaphoreHandle_t s_img_mutex;
static TaskHandle_t s_img_task;

static void img_lock(void) { if (s_img_mutex) (void)xSemaphoreTake(s_img_mutex, portMAX_DELAY); }
static void img_unlock(void) { if (s_img_mutex) (void)xSemaphoreGive(s_img_mutex); }

/* 取路径最后一段并去掉查询串，作为 tar 条目与正文引用的公共键。
 * Last path segment without query string: the key shared by tar entries and body refs. */
static bool asset_key(const char *name, char out[96]) {
    if (!name || !*name) return false;
    const char *base = name;
    for (const char *p = name; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    size_t n = 0;
    while (base[n] && base[n] != '?' && base[n] != '#' && n + 1 < 96) ++n;
    if (!n) return false;
    memcpy(out, base, n);
    out[n] = 0;
    return true;
}

/* 把 HTML 里的图片地址规范成可直接请求的形式：`//` 补协议、`&amp;` 还原成 `&`。
 * Normalise an image reference into a requestable URL. */
static bool image_url(const char *src, size_t size, char *url, size_t cap) {
    if (!size || size >= 2048) return false;
    size_t used = 0;
    if (size >= 2 && !memcmp(src, "//", 2)) { if (cap < 7) return false; memcpy(url, "https:", 6); used = 6; }
    for (size_t i = 0; i < size; ++i) {
        if (used + 2 > cap) return false;
        if (i + 5 <= size && !memcmp(src + i, "&amp;", 5)) { url[used++] = '&'; i += 4; }
        else url[used++] = src[i];
    }
    url[used] = 0;
    return used > 0;
}

/* 下载一张图（含重试）。图片是尽力而为的：失败只影响这一张，不拖垮整章。 */
static char *image_download(const char *url, size_t *length, unsigned id) {
    int64_t started = esp_timer_get_time();
    char *data = NULL;
    for (unsigned attempt = 0; attempt < 3 && !data && !wr_cancelled(); ++attempt) {
        data = wr_http(url, NULL, NULL, 4*1024*1024, length);
        if (!data && !strcmp(wr_error_message(), "微信读书图片地址不受支持")) break;
        if (!data && attempt < 2) vTaskDelay(pdMS_TO_TICKS(350u * (attempt + 1)));
    }
    if (!data)
        ESP_LOGW("WEREAD", "cache image id=%u elapsed_ms=%lld result=0 error=%s", id,
                 (long long)((esp_timer_get_time() - started) / 1000), wr_error_message());
    return data;
}

/* 后台下载线程：认领"比主线程多一格"的图片。 */
static void image_prefetch_task(void *argument) {
    (void)argument;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(40));
        wr_img_job_t *job = &s_img_job;
        unsigned index = WR_IMG_SLOTS;
        img_lock();
        if (!job->abort && job->claimed < job->count && job->claimed < job->high_water + 1) {
            index = job->claimed++;
            job->busy = true;
        }
        img_unlock();
        if (index >= WR_IMG_SLOTS) continue;
        size_t length = 0;
        char *data = image_download(job->slot[index].url, &length, index);
        img_lock();
        job->slot[index].data = data;
        job->slot[index].size = data ? length : 0;
        job->slot[index].ready = true;
        /* 主线程已经放弃这一章时，缓冲交给 image_prefetch_end 统一释放。 */
        job->busy = false;
        img_unlock();
    }
}

/* 取一张图：命中预取就等它交卷，还没人认领就自己下，未收录则同步下载。
 * Take one image: wait for the prefetch if it is in flight, claim it when nobody has,
 * and fall back to a synchronous download for anything the scan did not pick up. */
static char *image_take(const char *src, size_t size, size_t *length) {
    static EXT_RAM_BSS_ATTR char url[WR_IMG_URL_BYTES];
    static EXT_RAM_BSS_ATTR char key[96];
    *length = 0;
    if (!image_url(src, size, url, sizeof(url)) || !asset_key(url, key)) return NULL;
    unsigned index = WR_IMG_SLOTS;
    bool mine = false, ready = false;
    char *cached = NULL;
    size_t cached_size = 0;
    img_lock();
    for (unsigned i = 0; i < s_img_job.count; ++i)
        if (!strcmp(s_img_job.slot[i].key, key)) { index = i; break; }
    if (index < s_img_job.count) {
        /* 主线程按文档顺序推进水位，后台线程才被允许再往前一格。 */
        s_img_job.high_water = index + 1;
        ready = s_img_job.slot[index].ready;
        if (ready) {
            cached = s_img_job.slot[index].data;
            cached_size = s_img_job.slot[index].size;
            s_img_job.slot[index].data = NULL;
            s_img_job.slot[index].size = 0;
        } else if (s_img_job.claimed <= index) {
            s_img_job.claimed = index + 1;
            mine = true;
        }
    }
    img_unlock();
    if (index >= s_img_job.count) return image_download(url, length, 0);
    if (ready) {
        /* 预取成功就直接用；预取失败（或这张图先前已被取走）就自己下一次。 */
        if (cached) { *length = cached_size; return cached; }
        return image_download(url, length, index);
    }
    if (mine) {
        char *data = image_download(url, length, index);
        img_lock();
        s_img_job.slot[index].data = NULL;
        s_img_job.slot[index].size = 0;
        s_img_job.slot[index].ready = true;
        img_unlock();
        return data;
    }
    for (unsigned waited = 0; !wr_cancelled(); waited += 20) {
        char *data = NULL;
        size_t got = 0;
        bool done;
        img_lock();
        done = s_img_job.slot[index].ready;
        if (done) {
            data = s_img_job.slot[index].data;
            got = s_img_job.slot[index].size;
            s_img_job.slot[index].data = NULL;
            s_img_job.slot[index].size = 0;
        }
        img_unlock();
        if (done) {
            if (data) { *length = got; return data; }
            return image_download(url, length, index);
        }
        if (waited >= 40000) { ESP_LOGW("WEREAD", "cache image wait timeout id=%u", index); return NULL; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return NULL;
}

/* 一章的图片全在一个 POSIX tar 里（名字形如 `<章号>/epub_<book>_<n>`），一次请求就够。
 * 逐张图去请求是 N 次往返，每次都要重新排一次队；RTT 160 ms 时这部分开销比图片本身还贵。
 * A chapter's illustrations ship as one POSIX tar (`<chapter>/epub_<book>_<n>`), so one
 * request replaces N round trips - at 160 ms RTT the request overhead costs more than
 * the bytes. Returns how many slots were filled from the package. */
static unsigned tar_fill_slots(const char *tar_url) {
    int64_t started = esp_timer_get_time();
    size_t total = 0;
    char *blob = wr_http(tar_url, NULL, NULL, WR_IMG_TAR_LIMIT, &total);
    if (!blob) {
        /* 链路偶发停顿会让整包失败；重试一次比退回逐张下载（多次握手）便宜得多。
         * A transient stall can fail the whole bundle; one retry is far cheaper than
         * falling back to per-image requests with their extra handshakes. */
        ESP_LOGW("WEREAD", "image tar retry url=%s error=%s", tar_url, wr_error_message());
        vTaskDelay(pdMS_TO_TICKS(300));
        blob = wr_http(tar_url, NULL, NULL, WR_IMG_TAR_LIMIT, &total);
    }
    if (!blob) {
        ESP_LOGW("WEREAD", "image tar failed url=%s error=%s", tar_url, wr_error_message());
        return 0;
    }
    unsigned filled = 0, entries = 0;
    size_t offset = 0;
    while (offset + 512 <= total) {
        const char *header = blob + offset;
        bool blank = true;
        for (size_t i = 0; i < 512; ++i) if (header[i]) { blank = false; break; }
        if (blank) break;                       /* tar 以两个全零块收尾 */
        char raw[101];
        memcpy(raw, header, 100);
        raw[100] = 0;
        size_t size = 0;
        for (size_t i = 124; i < 136 && header[i] >= '0' && header[i] <= '7'; ++i)
            size = size * 8 + (size_t)(header[i] - '0');
        char type = header[156];
        size_t body = offset + 512;
        offset = body + ((size + 511) & ~(size_t)511);
        if ((type != '0' && type != 0) || !size || body + size > total) continue;
        ++entries;
        char key[96];
        if (!asset_key(raw, key)) continue;
        img_lock();
        for (unsigned i = 0; i < s_img_job.count; ++i) {
            if (s_img_job.slot[i].ready || strcmp(s_img_job.slot[i].key, key)) continue;
            char *copy = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (copy) {
                memcpy(copy, blob + body, size);
                s_img_job.slot[i].data = copy;
                s_img_job.slot[i].size = size;
                s_img_job.slot[i].ready = true;
                ++filled;
            }
            break;
        }
        img_unlock();
    }
    ESP_LOGI("WEREAD", "image tar bytes=%u entries=%u filled=%u/%u elapsed_ms=%lld",
             (unsigned)total, entries, filled, s_img_job.count,
             (long long)((esp_timer_get_time() - started) / 1000));
    free(blob);
    return filled;
}

/* 收掉一章的预取：停住后台线程、释放没被取走的缓冲，再把线程栈还给内部 RAM。
 * Stop the worker, drop unclaimed buffers, and hand the stack back to internal RAM. */
static void image_prefetch_end(void) {
    if (!s_img_task) return;
    img_lock();
    s_img_job.abort = true;
    bool busy = s_img_job.busy;
    img_unlock();
    for (unsigned waited = 0; busy; ++waited) {
        /* 正常情况下一张图最多几秒就结束；卡住的连接由 HTTP 看门狗兜底。 */
        if (waited >= 500) {
            ESP_LOGW("WEREAD", "image prefetch still busy ms=%u keep task", waited * 20);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        img_lock();
        busy = s_img_job.busy;
        img_unlock();
    }
    img_lock();
    for (unsigned i = 0; i < WR_IMG_SLOTS; ++i) {
        free(s_img_job.slot[i].data);
        s_img_job.slot[i].data = NULL;
        s_img_job.slot[i].size = 0;
        s_img_job.slot[i].ready = false;
    }
    s_img_job.count = 0;
    s_img_job.claimed = 0;
    s_img_job.high_water = 0;
    img_unlock();
    TaskHandle_t task = s_img_task;
    s_img_task = NULL;
    if (task) {
        /* 留一条水位日志：TLS 握手在下载线程上跑，栈余量必须可观测。 */
        ESP_LOGI("WEREAD", "image prefetch task stack_hwm=%u",
                 (unsigned)(uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t)));
        vTaskDelete(task);
    }
}

static bool image_entry(epub_writer_t *zip, FILE *opf, const char *src, size_t size, unsigned id, char name[40]) {
    if (!size || size >= 2048) { name[0] = 0; return !wr_cancelled(); }
    int64_t image_started = esp_timer_get_time();
    size_t length = 0;
    char *data = image_take(src, size, &length);
    if (!data) {
        /* CrossMux treats embedded illustrations as best-effort: a transient
         * CDN failure must not discard otherwise complete chapter text. */
        name[0] = 0;
        return !wr_cancelled();
    }
    const char *mime, *ext = image_extension(data, length, &mime);
    if (ext) snprintf(name, 40, "i%u.%s", id, ext);
    bool ok = !ext || (zip_entry(zip, name, data, length) &&
        fprintf(opf, "<item id=\"i%u\" href=\"%s\" media-type=\"%s\"%s/>", id, name, mime,
                id == 0 ? " properties=\"cover-image\"" : "") >= 0);
    ESP_LOGI("WEREAD", "cache image id=%u bytes=%u elapsed_ms=%lld result=%d", id,
             (unsigned)length, (long long)((esp_timer_get_time() - image_started) / 1000), ok);
    if (!ext) name[0] = 0;
    free(data);
    if (!ok) wr_error_set("缓存失败：图片写入 EPUB 失败");
    return ok;
}

static const char *tag_end(const char *start) {
    char quote = 0;
    for (const char *p = start; *p; ++p) {
        if (quote) { if (*p == quote) quote = 0; }
        else if (*p == '\'' || *p == '"') quote = *p;
        else if (*p == '>') return p;
    }
    return NULL;
}

static const char *image_src(const char *p, const char *end, size_t *length) {
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) ++p;
        const char *key = p;
        while (p < end && !strchr("= \r\n\t", *p)) ++p;
        size_t key_size = p-key;
        while (p < end && isspace((unsigned char)*p)) ++p;
        if (p == end || *p != '=') continue;
        do { ++p; } while (p < end && isspace((unsigned char)*p));
        if (p == end) break;
        char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
        const char *value = p;
        while (p < end && (quote ? *p != quote : !isspace((unsigned char)*p))) ++p;
        if ((key_size == 3 && !strncasecmp(key,"src",3)) || (key_size == 9 && !strncasecmp(key,"xlink:href",9))) {
            *length = p-value;
            return value;
        }
        if (quote && p < end) ++p;
    }
    return NULL;
}

static unsigned utf16_at(const char *text, size_t bytes) {
    unsigned count = 0;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned c = (uint8_t)text[i];
        if ((c & 0xc0) != 0x80) count += c >= 0xf0 ? 2 : 1;
    }
    return count;
}

static bool marker(FILE *output, unsigned offset) {
    return fprintf(output,"<!--wr-co:%u-->",offset) >= 0;
}

static bool gif_source(const char *src, size_t size) {
    // 阅读器只渲染 png/jpeg/webp；gif 条目会被跳过，写入反而留下坏图引用。
    // The reader renders only png/jpeg/webp; a gif entry would become a broken reference.
    for (size_t i = 0; i + 4 <= size; ++i) {
        if (!strncasecmp(src+i, ".gif", 4) && (i+4 == size || src[i+4] == '?' || src[i+4] == '#'
                                               || src[i+4] == '&')) return true;
    }
    return false;
}

/* 扫描一章 HTML，把出现的图片地址按文档顺序排进预取队列。扫描规则必须与
 * rewrite_chapter 的正文字典一致，否则预取的结果对不上位置；对不上时取图会
 * 退回同步下载，所以这里保守一点没有风险。
 * Scan the chapter HTML and queue the image URLs in document order. The rules must
 * match the rewrite pass; a mismatch only means the image is fetched synchronously. */
static void image_prefetch_begin(const char *data, bool plain, const char *tar_url) {
    image_prefetch_end();
    if (plain || !data || wr_cancelled()) return;
    if ((!tar_url || !tar_url[0]) &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < WR_IMG_MIN_INTERNAL) {
        ESP_LOGW("WEREAD", "image prefetch skipped internal=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return;
    }
    img_lock();
    for (unsigned i = 0; i < WR_IMG_SLOTS; ++i) {
        s_img_job.slot[i].ready = false;
        s_img_job.slot[i].data = NULL;
        s_img_job.slot[i].size = 0;
        s_img_job.slot[i].key[0] = 0;
    }
    s_img_job.count = 0;
    s_img_job.claimed = 0;
    s_img_job.high_water = 0;
    s_img_job.busy = false;
    s_img_job.abort = false;
    img_unlock();

    const char *p = strcasestr(data, "<body");
    if (p) { p = tag_end(p); if (p) ++p; }
    if (!p) p = data;
    while (*p && !wr_cancelled()) {
        bool image = !strncasecmp(p, "<img", 4) && (isspace((unsigned char)p[4]) || p[4] == '>');
        bool svg = !strncasecmp(p, "<image", 6) && isspace((unsigned char)p[6]);
        if (image || svg) {
            const char *end = tag_end(p);
            if (!end) break;
            size_t length = 0;
            const char *src = image_src(p + (image ? 4 : 6), end, &length);
            if (src && !gif_source(src, length)) {
                img_lock();
                if (s_img_job.count < WR_IMG_SLOTS) {
                    wr_img_slot_t *slot = &s_img_job.slot[s_img_job.count];
                    bool dup = !image_url(src, length, slot->url, WR_IMG_URL_BYTES) ||
                               !asset_key(slot->url, slot->key);
                    for (unsigned i = 0; !dup && i < s_img_job.count; ++i)
                        if (!strcmp(s_img_job.slot[i].key, slot->key)) dup = true;
                    if (dup) slot->key[0] = 0; else ++s_img_job.count;
                }
                img_unlock();
            }
            p = end + 1;
            continue;
        }
        if (*p == '<') {
            const char *end = tag_end(p);
            if (!end) break;
            p = end + 1;
        } else {
            p += strcspn(p, "<");
        }
    }
    if (!s_img_job.count) return;
    /* 有资源包就先整包拿走：一次请求换掉本章所有图片请求。包不完整时缺的那几张
     * 仍按老路子逐张下，所以这里失败不会丢图。
     * Prefer the package: one request replaces every image request of this chapter.
     * Slots the package does not cover still fall back to individual downloads. */
    if (tar_url && tar_url[0]) {
        unsigned filled = tar_fill_slots(tar_url);
        if (filled >= s_img_job.count) return;
        ESP_LOGW("WEREAD", "image tar partial filled=%u/%u fallback=1", filled, s_img_job.count);
    }
    if (s_img_job.count < 2) return;   /* 只有一张图时并行没有收益，省掉一个任务 */
#if !WR_IMG_PARALLEL
    /* 串行模式：队列仍然建好（image_take 靠它按文档顺序取图），但不起下载线程。
     * Serial mode: keep the queue for ordering, but do not start a second stream. */
    ESP_LOGI("WEREAD", "image prefetch queued=%u internal=%u serial=1", s_img_job.count,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return;
#endif
    if (!s_img_task) {
        if (!s_img_mutex) s_img_mutex = xSemaphoreCreateMutexStatic(&s_img_mutex_storage);
        if (!s_img_mutex) return;
        if (xTaskCreate(image_prefetch_task, "wr-img", WR_IMG_STACK_BYTES, NULL, 2, &s_img_task) != pdPASS) {
            ESP_LOGW("WEREAD", "image prefetch task failed internal=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            s_img_task = NULL;
            s_img_job.count = 0;
            return;
        }
    }
    ESP_LOGI("WEREAD", "image prefetch queued=%u internal=%u", s_img_job.count,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static bool rewrite_chapter_body(FILE *output, FILE *opf, epub_writer_t *zip, const char *data, bool plain, unsigned *image_id) {
    if (fputs("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>", output) == EOF) return false;
    if (plain) {
        const char *line = data;
        unsigned offset = 0;
        while (*line && !wr_cancelled()) {
            size_t length = strcspn(line, "\r\n");
            if (fputs("<p>", output) == EOF || !marker(output,offset)) return false;
            for (size_t i = 0; i < length; ++i) {
                unsigned c = (uint8_t)line[i];
                bool escape = c < 128 && strchr("&<>\"'",c);
                if (escape && !marker(output,offset)) return false;
                if (!write_xml(output,line+i,1)) return false;
                if ((c & 0xc0) != 0x80) offset += c >= 0xf0 ? 2 : 1;
                if (escape && !marker(output,offset)) return false;
            }
            line += length;
            while (*line == '\r' || *line == '\n') { ++line; ++offset; }
            if (fputs("</p>",output) == EOF) return false;
        }
    } else {
        const char *p = strcasestr(data, "<body");
        if (p) { p = tag_end(p); if (p) ++p; }
        if (!p) p = data;
        unsigned offset = utf16_at(data,p-data);
        while (*p && !wr_cancelled()) {
            if (!strncasecmp(p, "</body", 6)) break;
            bool image = !strncasecmp(p,"<img",4) && (isspace((unsigned char)p[4]) || p[4] == '>');
            bool svg = !strncasecmp(p,"<image",6) && isspace((unsigned char)p[6]);
            if (image || svg) {
                const char *end = tag_end(p);
                if (!end) return false;
                size_t length;
                const char *src = image_src(p+(image?4:6), end, &length);
                char name[40];
                if (src && gif_source(src, length)) {
                    offset += utf16_at(p,end+1-p);
                    p = end+1;
                    continue;
                }
                if (src) {
                    if (!image_entry(zip, opf, src, length, (*image_id)++, name)) return false;
                    if (name[0] && fprintf(output, "<img src=\"%s\"/>", name) < 0) return false;
                }
                offset += utf16_at(p,end+1-p);
                p = end+1;
            } else if (*p == '<') {
                const char *end = tag_end(p);
                if (!end || fwrite(p,1,end+1-p,output) != (size_t)(end+1-p)) return false;
                offset += utf16_at(p,end+1-p);
                p = end+1;
            } else {
                size_t n = strcspn(p,"<");
                if (!marker(output,offset) || fwrite(p,1,n,output) != n) return false;
                offset += utf16_at(p,n);
                p += n;
            }
        }
    }
    return !wr_cancelled() && fputs("</body></html>", output) != EOF;
}

/* 预取队列的生命周期与一章的改写严格对齐，任何提前返回都要在这里收尾。 */
static bool rewrite_chapter(FILE *output, FILE *opf, epub_writer_t *zip, const char *data,
                            bool plain, unsigned *image_id, const char *tar_url) {
    image_prefetch_begin(data, plain, tar_url);
    bool ok = rewrite_chapter_body(output, opf, zip, data, plain, image_id);
    image_prefetch_end();
    return ok;
}

static const cJSON *chapter_array(const cJSON *root, const char *book_id, unsigned depth) {
    if (!root || depth > 5) return NULL;
    if (cJSON_IsArray(root)) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, root) {
            if (cJSON_GetObjectItemCaseSensitive(entry, "chapterUid")) return root;
            char entry_id[WR_ID];
            if (wr_value(cJSON_GetObjectItemCaseSensitive(entry, "bookId"), entry_id, sizeof(entry_id)) &&
                strcmp(entry_id, book_id)) continue;
            const cJSON *chapters = chapter_array(entry, book_id, depth + 1);
            if (chapters) return chapters;
        }
    } else if (cJSON_IsObject(root)) {
        static const char *const keys[] = {"chapterInfos", "updated", "data"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
            const cJSON *chapters = chapter_array(cJSON_GetObjectItemCaseSensitive(root, keys[i]), book_id, depth + 1);
            if (chapters) return chapters;
        }
    }
    return NULL;
}

static struct {
    char owner[WR_ID];
    char book[WR_ID];
    wr_chapter_t *items;
    unsigned count;
    int64_t expires;
} s_chapter_cache;

static wr_chapter_t *get_chapters(const wr_book_t *book, unsigned *count) {
    int64_t started = esp_timer_get_time();
    char query[100];
    *count = 0;
    char owner[WR_ID] = {0};
    (void)wr_account(owner, NULL, NULL);
    if (s_chapter_cache.items && s_chapter_cache.expires > started &&
        !strcmp(owner, s_chapter_cache.owner) && !strcmp(book->id, s_chapter_cache.book)) {
        wr_chapter_t *copy = heap_caps_malloc(s_chapter_cache.count * sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy) {
            memcpy(copy, s_chapter_cache.items, s_chapter_cache.count * sizeof(*copy));
            *count = s_chapter_cache.count;
            ESP_LOGI("WEREAD", "chapter list cache hit book=%s chapters=%u", book->id, *count);
            return copy;
        }
    }
    snprintf(query, sizeof(query), "{\"bookIds\":[\"%s\"]}", book->id);
    cJSON *toc = wr_json("/web/book/chapterInfos", query);
    if (!toc) return NULL;
    const cJSON *chapters = chapter_array(toc, book->id, 0);
    int n = cJSON_GetArraySize(chapters);
    wr_chapter_t *result = n > 0 && n <= ZIP_ENTRY_MAX - 4
        ? heap_caps_calloc(n, sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!result) {
        ESP_LOGW("WEREAD", "cache toc unavailable count=%d updated_type=%d data_type=%d", n,
                 cJSON_GetObjectItemCaseSensitive(toc, "updated") ? cJSON_GetObjectItemCaseSensitive(toc, "updated")->type : 0,
                 cJSON_GetObjectItemCaseSensitive(toc, "data") ? cJSON_GetObjectItemCaseSensitive(toc, "data")->type : 0);
        wr_error_set(n == 0 ? "缓存失败：微信读书未返回可用章节目录" :
                     n > ZIP_ENTRY_MAX - 4 ? "缓存失败：章节数量超出 EPUB 支持范围" :
                     "缓存失败：内存不足，无法载入章节目录");
    }
    const cJSON *entry;
    if (result) cJSON_ArrayForEach(entry, chapters) {
        wr_chapter_t *item = &result[*count];
        if (!wr_value(cJSON_GetObjectItemCaseSensitive(entry,"chapterUid"), item->id, sizeof(item->id)) || !wr_safe(item->id)) {
            ESP_LOGW("WEREAD", "cache toc invalid chapter at index=%u", *count);
            wr_error_set("缓存失败：章节目录中有无效的章节标识");
            free(result);
            result = NULL;
            break;
        }
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(entry, "chapterIdx");
        item->index = cJSON_IsNumber(index) && index->valuedouble >= 0 && index->valuedouble <= UINT32_MAX ? (uint32_t)index->valuedouble : *count;
        (void)wr_value(cJSON_GetObjectItemCaseSensitive(entry,"title"), item->title, sizeof(item->title));
        /* tar 是本章的图片资源包；目录接口一次就把全书每章的地址都给全了。
         * `tar` is this chapter's image package and the catalog hands out the whole list at once. */
        item->tar[0] = 0;
        if (wr_value(cJSON_GetObjectItemCaseSensitive(entry,"tar"), item->tar, sizeof(item->tar))) {
            /* 目录里的 tar 可能是绝对地址、协议相对地址或站内路径，统一补全。
             * The catalog may return an absolute, protocol-relative or site-relative URL. */
            const char *prefix = !strncmp(item->tar, "//", 2) ? "https:"
                               : item->tar[0] == '/' ? "https://weread.qq.com" : NULL;
            if (prefix) {
                size_t head = strlen(prefix), tail = strlen(item->tar);
                if (head + tail < sizeof(item->tar)) {
                    memmove(item->tar + head, item->tar, tail + 1);
                    memcpy(item->tar, prefix, head);
                } else {
                    item->tar[0] = 0;
                }
            }
            if (strncasecmp(item->tar, "https://", 8)) item->tar[0] = 0;
        }
        ++*count;
    }
    cJSON_Delete(toc);
    if (result && owner[0]) {
        wr_chapter_t *saved = heap_caps_malloc(*count * sizeof(*saved), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (saved) {
            memcpy(saved, result, *count * sizeof(*saved));
            free(s_chapter_cache.items);
            snprintf(s_chapter_cache.owner, sizeof(s_chapter_cache.owner), "%s", owner);
            snprintf(s_chapter_cache.book, sizeof(s_chapter_cache.book), "%s", book->id);
            s_chapter_cache.items = saved;
            s_chapter_cache.count = *count;
            s_chapter_cache.expires = started + 600000000;
            ESP_LOGI("WEREAD", "chapter list cache updated book=%s chapters=%u ttl_s=600", book->id, *count);
        }
    }
    ESP_LOGI("WEREAD", "cache toc book=%s chapters=%u elapsed_ms=%lld", book->id, *count,
             (long long)((esp_timer_get_time()-started)/1000));
    return result;
}

static bool package_book(const wr_book_t *book, FILE *target, const wr_chapter_t *chapters, unsigned count) {
    s_reuse_book[0] = s_reuse_ps[0] = 0;
    s_reuse_hits = s_reuse_misses = s_reuse_rejects = 0;
    s_reuse_ok = false;
    static const char container[] = "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"book.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    static const char type[] = "application/epub+zip";
    FILE *central = fopen(WR_DIR "/central.part", "w+b");
    FILE *opf = fopen(WR_DIR "/opf.part", "w+b");
    FILE *nav = fopen(WR_DIR "/nav.part", "w+b");
    if (central) (void)setvbuf(central, NULL, _IOFBF, 4096);
    if (opf) (void)setvbuf(opf, NULL, _IOFBF, 2048);
    if (nav) (void)setvbuf(nav, NULL, _IOFBF, 2048);
    epub_writer_t zip = {.file=target, .directory=central};
    bool ok = central && opf && nav && zip_entry(&zip, "mimetype", type, sizeof(type)-1) &&
        zip_entry(&zip, "META-INF/container.xml", container, sizeof(container)-1);
    if (ok) ok = fputs("<package version=\"3.0\" xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"book\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"book\">", opf) != EOF &&
        write_xml(opf, book->id, strlen(book->id)) && fputs("</dc:identifier><dc:title>", opf) != EOF &&
        write_xml(opf, book->title, strlen(book->title)) && fputs("</dc:title><dc:creator>", opf) != EOF &&
        write_xml(opf, book->author, strlen(book->author)) &&
        fputs("</dc:creator><dc:language>zh</dc:language></metadata><manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>", opf) != EOF &&
        fputs("<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body><nav epub:type=\"toc\"><ol>", nav) != EOF;
    if (ok && book->cover[0]) {
        char name[40];
        ok = image_entry(&zip, opf, book->cover, strlen(book->cover), 0, name);
    }
    unsigned image_id = 1;
    for (unsigned i = 0; ok && i < count && !wr_cancelled(); ++i) {
        int64_t chapter_started = esp_timer_get_time();
        size_t size = 0;
        bool plain = false;
        char *content = NULL;
#if WR_PREFETCH_CHAPTER
        /* 上一章 tar 下载期间已经抓好的本章内容；取不到就回落到串行（带重试）。 */
        if (i > 0) content = prefetch_take(&plain, &size);
#endif
        const bool prefetched = content != NULL;
        if (!content) content = cache_fetch_chapter(book, &chapters[i], &plain, &size, i);
        if (!content) {
            if (!wr_cancelled() && !strcmp(wr_error_message(),"操作失败，请检查网络后重试"))
                wr_error_set("缓存失败：章节下载失败（已自动重试 3 次）");
            ESP_LOGE("WEREAD","cache chapter failed book=%s chapter=%s title=%s error=%s",book->id,chapters[i].id,chapters[i].title,wr_error_message());
            ok = false;
            break;
        }
        if (size >= 4 && !memcmp(content, "PK\003\004", 4)) {
            ok = !i && !fseek(target, 0, SEEK_SET) && fwrite(content, 1, size, target) == size &&
                !fflush(target) && !ftruncate(fileno(target), size);
            free(content);
            goto cleanup;
        }
        char name[40];
        snprintf(name, sizeof(name), "c%u.xhtml", i);
        FILE *chapter = fopen(WR_DIR "/chapter.part", "w+b");
        if (chapter) (void)setvbuf(chapter, NULL, _IOFBF, 4096);
#if WR_PREFETCH_CHAPTER
        /* 现在开始占带宽（本章 tar），让预取任务同时去填延迟（下一章的 reader 页与分片）。
         * Bandwidth work starts here; hand the latency work for the next chapter to the task. */
        if (chapter && i + 1 < count) prefetch_arm(book, &chapters[i + 1]);
#endif
        ok = chapter && !memchr(content, 0, size) && rewrite_chapter(chapter, opf, &zip, content, plain, &image_id, chapters[i].tar) &&
            file_entry(&zip, name, chapter, HTML_TEXT_MAX_BYTES) &&
            fprintf(opf,"<item id=\"c%u\" href=\"%s\" media-type=\"application/xhtml+xml\"/>",i,name) >= 0 &&
            fprintf(nav,"<li><a href=\"%s\">",name) >= 0 && write_xml(nav,chapters[i].title,strlen(chapters[i].title)) &&
            fputs("</a></li>",nav) != EOF;
        if (chapter) fclose(chapter);
        ESP_LOGI("WEREAD", "cache chapter book=%s index=%u/%u html_bytes=%u elapsed_ms=%lld prefetched=%d result=%d",
                 book->id, i+1, count, (unsigned)size,
                 (long long)((esp_timer_get_time()-chapter_started)/1000), (int)prefetched, ok);
        if (!ok && !wr_cancelled() && !strcmp(wr_error_message(),"操作失败，请检查网络后重试"))
            wr_error_set("缓存失败：章节内容处理或图片下载失败");
        if (!ok) ESP_LOGE("WEREAD","cache package failed book=%s chapter=%u title=%s error=%s",book->id,i,chapters[i].title,wr_error_message());
        free(content);
        wr_work_progress(i+1, count);
        /* Let idle/UI and watchdog service run between CPU-heavy chapter conversions. */
        vTaskDelay(1);
    }
    if (ok && !wr_cancelled()) {
        ok = fputs("</manifest><spine>", opf) != EOF;
        for (unsigned i = 0; ok && i < count; ++i) ok = fprintf(opf, "<itemref idref=\"c%u\"/>", i) >= 0;
        ok = ok && fputs("</spine></package>", opf) != EOF && fputs("</ol></nav></body></html>", nav) != EOF &&
            file_entry(&zip, "book.opf", opf, HTML_TEXT_MAX_BYTES) &&
            file_entry(&zip, "nav.xhtml", nav, HTML_TEXT_MAX_BYTES) && zip_finish(&zip);
    } else ok = false;
cleanup:
    ESP_LOGI("WEREAD", "cache reader reuse hits=%u rejects=%u enabled=%d",
             s_reuse_hits, s_reuse_rejects, (int)s_reuse_ok);
#if WR_PREFETCH_CHAPTER
    /* 提前收尾时预取任务可能还握着一章内容，必须取回来释放，否则下次启动就是两章串位。
     * An early exit may leave one prefetched chapter in flight; drain and free it so the next
     * run cannot pick up a stale chapter. */
    if (s_prefetch_armed) {
        size_t stale_size = 0;
        bool stale_plain = false;
        free(prefetch_take(&stale_plain, &stale_size));
    }
#endif
    if (nav) fclose(nav);
    if (opf) fclose(opf);
    if (central) fclose(central);
    unlink(WR_DIR "/chapter.part");
    unlink(WR_DIR "/central.part");
    unlink(WR_DIR "/opf.part");
    unlink(WR_DIR "/nav.part");
    return ok;
}

bool wr_cache(const wr_book_t *book, const char *path) {
    if (access(path, F_OK) == 0) {
        book_epub_t *existing=NULL;
        bool valid=book_epub_open(path,&existing)==ESP_OK && book_epub_chapter_count(existing)>0;
        book_epub_close(existing);
        if (valid) return true;
        ESP_LOGW("WEREAD","remove invalid cached epub book=%s path=%s",book->id,path);
        if (unlink(path) && errno!=ENOENT) { wr_error_set("缓存失败：旧缓存损坏且无法从 SD 卡删除"); return false; }
    }
    unsigned count;
    wr_chapter_t *chapters = get_chapters(book, &count);
    if (!chapters) {
        if (strcmp(wr_error_message(), "操作失败，请检查网络后重试")) return false;
        wr_error_set("缓存失败：章节目录获取失败");
        return false;
    }
    char temp[WR_PATH+8];
    snprintf(temp, sizeof(temp), "%s.part", path);
    FILE *file = fopen(temp, "w+b");
    if (file) (void)setvbuf(file, NULL, _IOFBF, 8192);
    if (!file) { wr_error_set("缓存失败：无法在 SD 卡创建临时书籍文件"); ESP_LOGE("WEREAD","cache temp open failed book=%s path=%s errno=%d",book->id,temp,errno); }
    bool ok = file && package_book(book, file, chapters, count) && !fflush(file) && !fsync(fileno(file));
    if (!ok && strcmp(wr_error_message(), "操作失败，请检查网络后重试") && !wr_cancelled()) {
        /* Preserve a more precise HTTP/image error supplied by a lower layer. */
    } else if (!ok && !wr_cancelled()) wr_error_set(file ? "缓存失败：章节处理或 SD 卡写入失败" : "缓存失败：无法在 SD 卡创建临时书籍文件");
    if (file && fclose(file)) ok = false;
    if (ok && !wr_cancelled()) {
        book_epub_t *epub = NULL;
        ok = book_epub_open(temp, &epub) == ESP_OK && book_epub_chapter_count(epub) == count;
        book_epub_close(epub);
        if (ok && rename(temp, path) != 0) { ok = false; wr_error_set("缓存失败：SD 卡无法保存书籍文件"); }
        else if (!ok && !wr_cancelled()) wr_error_set("缓存失败：下载后的 EPUB 校验失败");
    } else ok = false;
    if (!ok) unlink(temp);
    free(chapters);
    return ok;
}

static bool map_position(const char *path, unsigned chapter, bool upload, unsigned offset, unsigned *mapped) {
    zip_reader_t *zip = NULL;
    if (zip_open(path,&zip) != ESP_OK) return false;
    char name[32];
    snprintf(name,sizeof(name),"c%u.xhtml",chapter);
    int entry = zip_find(zip,name);
    size_t size = zip_entry_size(zip,entry);
    char *data = size && size <= HTML_TEXT_MAX_BYTES ? heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) : NULL;
    bool ok = data && zip_extract(zip,entry,data,size) == ESP_OK && html_weread_position(data,size,upload,offset,mapped);
    free(data);
    zip_close(zip);
    // Chapter starts also work with complete EPUBs delivered by WeRead.
    if (!ok && !offset) { *mapped = 0; return true; }
    return ok;
}

bool wr_progress(const wr_book_t *book, const char *path, bool upload, const char **why) {
    if (why) *why = NULL;
    wr_error_set("操作失败，请检查网络后重试");
    struct stat st;
    book_progress_t local = {0};
    if (stat(path,&st) || st.st_size <= 0 || st.st_size > UINT32_MAX) {
        if (why) *why = "本地书籍文件不可用，请重新缓存";
        return false;
    }
    if (upload && !book_progress_load(path,(uint32_t)st.st_size,&local)) {
        if (why) *why = "这本书还没有本地阅读进度，先读一段再上传";
        return false;
    }
    unsigned count, offset = 0, pct = 0;
    wr_chapter_t remote;
    wr_chapter_t *chapters = get_chapters(book,&count);
    if (!chapters) {
        const char *detail = wr_error_message();
        if (why) *why = detail && *detail ? detail : "无法获取云端章节列表";
        return false;
    }
    unsigned index = local.chapter;
    if (!upload) {
        if (!wr_remote_position_with_chapters(book->id,&remote,&offset,&pct,chapters,count)) {
            free(chapters);
            const char *detail = wr_error_message();
            if (why) *why = detail && *detail ? detail : "微信读书里还没有这本书的阅读进度";
            return false;
        }
        index = count;
        for (unsigned i = 0; i < count; ++i) if (!strcmp(chapters[i].id,remote.id)) { index = i; break; }
        if (index >= count) {
            free(chapters);
            if (why) *why = "云端进度所在章节与本地版本不一致，无法定位";
            return false;
        }
    }
    unsigned mapped = 0;
    bool ok = index < count && map_position(path,index,upload,upload?local.byte_off:offset,&mapped);
    if (!ok && why) *why = upload ? "本地位置无法映射到云端章节，翻一页后再试"
                                 : "云端位置无法映射到本地章节，请重新缓存这本书";
    if (ok && upload) {
        ok = wr_send_position(book->id,&chapters[index],mapped,local.pct);
        if (!ok && why) *why = "上传进度失败，请检查网络后重试";
    } else if (ok) {
        (void)book_progress_load(path, (uint32_t)st.st_size, &local);
        if (local.px < BOOK_READER_PX_MIN || local.px > BOOK_READER_PX_MAX)
            local.px = BOOK_READER_PX_DEFAULT;
        local.file_size = (uint32_t)st.st_size;
        local.chapter = (uint16_t)index;
        local.byte_off = mapped;
        local.pct = (uint8_t)pct;
        esp_err_t save_err = wr_cancelled() ? ESP_ERR_INVALID_STATE : book_progress_save(path,&local);
        ok = save_err == ESP_OK;
        if (!ok) ESP_LOGW("WEREAD", "download cloud progress local save failed err=%s", esp_err_to_name(save_err));
        if (!ok && why) *why = "云端进度已获取，但设备保存本地阅读位置失败";
    }
    free(chapters);
    return ok;
}
