#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WR_ID 48
#define WR_PATH 288

typedef struct {
    char id[WR_ID], title[160], author[96], cover[384];
    uint32_t updated;
} wr_book_t;

typedef struct {
    char id[WR_ID], title[128];
    /* 章节资源包地址（chapterInfos 的 tar 字段）：一章一个 tar，包里就是这一章的全部插图。
     * 逐张图去请求 wrepub/epub_<book>_<n> 是 60 次往返，改成一次拿一包。
     * Per-chapter resource package from the catalog `tar` field: one request carries
     * every illustration of that chapter instead of one request per image. */
    char tar[192];
    unsigned index;
} wr_chapter_t;

typedef enum { WR_LOGIN, WR_SYNC, WR_CACHE, WR_UPLOAD, WR_DOWNLOAD, WR_REPORT, WR_CHECK } wr_job_t;
typedef enum { WR_IDLE, WR_BUSY, WR_OK, WR_FAILED, WR_EXPIRED, WR_CANCELLED } wr_result_t;

bool wr_online(void);
void wr_init(void);
/* 释放连接池里所有空闲长连接：Wi-Fi 驱动 deinit 后这些 socket 已经是野句柄，
 * 而且内部 RAM 要腾给射频初始化。 / Drop idle keep-alive sockets before the
 * radio is re-initialised: they are wild handles afterwards and their internal
 * RAM is needed by Wi-Fi init. */
void wr_http_release(void);
/* 是否有微信读书网络任务在跑。跑的时候连接池里的 TLS 连接占着内部 RAM，
 * 射频初始化要先等它收尾。 / True while a weread network job runs; its pooled
 * TLS connections hold internal RAM that Wi-Fi init needs. */
bool wr_network_busy(void);
bool wr_account(char id[WR_ID], uint32_t *today, uint64_t *total);
const char *wr_error_message(void);
const char *wr_result_message(void);
bool wr_logout(bool remove_books);
int wr_shelf_count(void);
bool wr_shelf_get(int index, wr_book_t *book);
bool wr_book_path(const wr_book_t *book, char path[WR_PATH]);
bool wr_cover_get(const wr_book_t *book, uint8_t *pixels);
bool wr_start(wr_job_t job, const wr_book_t *book, unsigned first, unsigned count);
wr_result_t wr_status(char *qr, size_t qr_size, unsigned *done, unsigned *total);
void wr_cancel(void);
bool wr_reporting_book(const char *path, wr_book_t *book);
void wr_reading_start(const char *path, bool enabled, int64_t now_ms);
void wr_reading_tick(bool active, int64_t now_ms);
void wr_reading_stop(bool close);
void wr_reading_display(bool scanning);
