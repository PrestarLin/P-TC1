/**
 ******************************************************************************
 * @file    httpd_priv.h
 * @author  QQ DING
 * @version V1.0.0
 * @date    1-September-2015
 * @brief   This file is httpd_ssi.c and httpd_sys.c and httpd_wsgi.c header files
 ******************************************************************************
 *
 *  The MIT License
 *  Copyright (c) 2014 MXCHIP Inc.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is furnished
 *  to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
 *  IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 ******************************************************************************
 */

#ifndef __HTTPD_PRIV_H__
#define __HTTPD_PRIV_H__

#include "httpd.h"
#include "mico.h"

//#define CONFIG_HTTPD_DEBUG 

#ifdef CONFIG_HTTPD_DEBUG
#define httpd_d(M, ...)				\
	custom_log("httpd", M, ##__VA_ARGS__)
#else
#define httpd_d(...)
#endif /* ! CONFIG_HTTPD_DEBUG */


int httpd_test_setup(void);
int httpd_wsgi_init(void);

/** Set the httpd-wide error message
 *
 * Often, when something fails, a 500 Internal Server Error will be emitted.
 * This is especially true when the reason is something arbitrary, like the
 * maximum header line that the httpd can handle is exceeded.  When this
 * happens, a function can set the error string to be passed back to the user.
 * This facilitates debugging.  Note that the error message will also be
 * httpd_d'd, so no need to add extra lines of code for that.
 *
 * Note that this function is not re-entrant.
 *
 * Note that at most HTTPD_MAX_ERROR_STRING characters will be stored.
 *
 * Note: no need to have a \r\n on the end of the error message.
 */
#define HTTPD_MAX_ERROR_STRING 256
void httpd_set_error(const char *fmt, ...);

/* ---- 诊断计数器(telnet `httpd` 命令使用): 区分"线程卡死"与"线程活着但不服务" ---- */
typedef struct
{
    volatile uint32_t loops;        /* 主循环迭代 */
    volatile uint32_t ticks;        /* select 超时(1s tick) */
    volatile uint32_t sel;          /* select 调用次数 */
    volatile uint32_t selerr;       /* select 返回错误 */
    volatile uint32_t acc;          /* accept 成功 */
    volatile uint32_t accfail;      /* accept 未成功(含非阻塞 EAGAIN) */
    volatile uint32_t srv;          /* 请求处理完成(kNoErr) */
    volatile uint32_t srvfail;      /* 请求处理失败/连接关闭 */
    volatile uint32_t reap;         /* 空闲回收的连接数 */
    volatile uint32_t evict;        /* 连接满时驱逐的次数 */
    volatile uint32_t in_select;    /* 1=当前正阻塞在 select 内 */
    volatile uint32_t in_accept;    /* 1=当前正阻塞在 accept 内 */
    volatile uint32_t in_handle;    /* 1=当前正在处理请求 */
    volatile uint32_t handle_ms;    /* 最近一次请求处理耗时(ms) */
    volatile int32_t  last_status;  /* 最近一次 httpd_handle_message 返回值 */
    volatile uint32_t suspend_site; /* 0=未挂起 1=select失败 2=stop请求 3=启动失败 */
    /* 处理管线阶段: 1=读请求行 2=解析请求行 3=分发 4=执行handler 5=发响应头
     * 6=发响应体 7=404清流 8=发错误响应 9=清读请求头 0=空闲 */
    volatile uint32_t stage;
    volatile uint32_t stage_fd;
    volatile uint32_t stage_ms;     /* 进入当前阶段的毫秒时刻 */
    volatile uint32_t io_kind;      /* 1=正在 httpd_send 2=正在 httpd_recv 0=空闲 */
    volatile uint32_t io_fd;
    volatile uint32_t io_loop;      /* 同一 fd 上连续 IO 调用次数(卡死后观察是否爆炸) */
    char              stage_fn[24]; /* 当前处理/卡死时正在处理的 URL */
} httpd_dbg_t;

extern httpd_dbg_t httpd_dbg;

void httpd_dbg_set_stage( uint32_t stage, int fd, const char *fname );
void httpd_dbg_io( int kind, int fd );

int handle_message(char *msg_in, int msg_in_len, int conn);
int httpd_parse_hdr_main(const char *data_p, httpd_request_t *req_p);
int httpd_handle_message(int conn);

/* Various Defines */
#ifndef NULL
#define NULL 0
#endif

int httpd_wsgi(httpd_request_t *req_p);

httpd_ssifunction httpd_ssi(char *);
int httpd_ssi_init(void);
int htsys_getln_soc(int sd, char *data_p, int buflen);

void httpd_parse_useragent(char *hdrline, httpd_useragent_t *agent);

int httpd_send_last_chunk(int conn);

enum {
	HTTP_404,
	HTTP_500,
	HTTP_505,
};

int httpd_send_error(int conn, int http_error);

bool httpd_is_https_active( void );
#endif				/* __HTTPD_PRIV_H__ */
