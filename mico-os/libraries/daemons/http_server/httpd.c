/**
 ******************************************************************************
 * @file    httpd.c
 * @author  QQ DING
 * @version V1.0.0
 * @date    1-September-2015
 * @brief   The main HTTPD server thread and its initialization.
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

#include <string.h>
#include <stdio.h>

#include "httpd.h"
#include "http-strings.h"
#include "httpd_priv.h"
#include "mico.h"
#include "SocketUtils.h"
#include "base64.h"

typedef enum
{
    HTTPD_INACTIVE = 0,
    HTTPD_INIT_DONE,
    HTTPD_THREAD_RUNNING,
    HTTPD_THREAD_SUSPENDED,
} httpd_state_t;

httpd_state_t httpd_state;

static mico_thread_t httpd_main_thread;

// 0x8000 不行
#define http_server_thread_stack_size 0x6000

/* Why HTTPD_MAX_MESSAGE + 2?
 * Handlers are allowed to use HTTPD_MAX_MESSAGE bytes of this buffer.
 * Internally, the POST var processing needs a null termination byte and an
 * '&' termination byte.
 */
static bool httpd_stop_req;

/* keep-alive 空闲回收: 浏览器轮询间隔 3s, 5s 内连接可复用; 超时回收连接,
 * 保证看门狗探针(8s 超时)不会因连接被长期占用而误判 httpd 卡死 */
#define HTTPD_CLIENT_SOCK_TIMEOUT 5
#define HTTPD_TIMEOUT_EVENT 0

/* 主循环 tick: 每秒醒一次做空闲连接回收 */
#define HTTPD_MAIN_TICK_SECS 1

/* 并发持有的客户端连接上限: 页面轮询 1~2 条 + 看门狗探针 1 条足够。
 * 请求处理仍串行(共享 httpd_req 等全局态), 这里只是同时持有多个已建立连接,
 * 避免为了服务排队的新客户端(探针)而强行断开活跃连接 —— 断开重连的 churn
 * 会耗光 mocIP(lwIP) TCP PCB 池(约 40 个, 关闭后约 2min 才回收),
 * 约 40 个新连接后 httpd 就无法 accept, 网页失联 */
#define HTTPD_MAX_CLIENTS 4

/** Maximum number of backlogged http connections
 *
 *  httpd has a single listening socket from which it accepts connections.
 *  HTTPD_MAX_BACKLOG_CONN is the maximum number of connections that can be
 *  pending.  For example, suppose a webpage contains 10 images.  If a client
 *  attempts to load all 10 of those images at once, only the first
 *  HTTPD_MAX_BACKLOG_CONN attempts can succeed.  Some clients will retry when
 *  the attempts fail; others will limit the maximum number of open connections
 *  that it has.  But some may attempt to load all 10 simultaneously.  If your
 *  web pages have many images, or css files, or java script files, you may
 *  need to increase this number.
 *
 *  \note Your underlying TCP/IP stack may have other limitations
 *  besides the backlog.  For example, the treck stack limits the
 *  number of system-wide TCP sockets to TM_OPTION_TCP_SOCKETS_MAX.
 *  You will have to adjust this value if you need more than
 *  TM_OPTION_TCP_SOCKETS_MAX simultaneous TCP sockets.
 *
 */
#define HTTPD_MAX_BACKLOG_CONN 5

static int http_sockfd;

static int httpd_client_fds[HTTPD_MAX_CLIENTS];
static uint32_t httpd_client_lastact[HTTPD_MAX_CLIENTS]; /* mico_rtos_get_time() 毫秒时刻 */
static bool https_active;

bool httpd_is_https_active( )
{
    return https_active;
}

httpd_dbg_t httpd_dbg;

void httpd_dbg_set_stage( uint32_t stage, int fd, const char *fname )
{
    httpd_dbg.stage = stage;
    httpd_dbg.stage_fd = fd;
    httpd_dbg.stage_ms = mico_rtos_get_time( );
    if ( fname )
    {
        strncpy( httpd_dbg.stage_fn, fname, sizeof(httpd_dbg.stage_fn) - 1 );
        httpd_dbg.stage_fn[sizeof(httpd_dbg.stage_fn) - 1] = 0;
    }
}

/* kind: 1=httpd_send 2=httpd_recv; 换 fd/方向时重新计数, 便于 telnet 观察
 * 卡死点是否在某个 fd 上被反复调用(io_loop 爆炸)或是单次阻塞 */
void httpd_dbg_io( int kind, int fd )
{
    if ( httpd_dbg.io_kind != (uint32_t) kind || (int) httpd_dbg.io_fd != fd )
    {
        httpd_dbg.io_kind = kind;
        httpd_dbg.io_fd = fd;
        httpd_dbg.io_loop = 0;
    }
    httpd_dbg.io_loop++;
}

static int net_get_sock_error( int sock )
{
    return -kInProgressErr;
}

static int httpd_close_sockets( )
{
    int ret, status = kNoErr, i;

    if ( http_sockfd != -1 )
    {
        ret = close( http_sockfd );
        if ( ret != 0 )
        {
            httpd_d("failed to close http socket: %d", net_get_sock_error(http_sockfd));
            status = -kInProgressErr;
        }
        http_sockfd = -1;
    }

    for ( i = 0; i < HTTPD_MAX_CLIENTS; i++ )
    {
        if ( httpd_client_fds[i] != -1 )
        {
            ret = close( httpd_client_fds[i] );
            if ( ret != 0 )
            {
                httpd_d("Failed to close client socket: %d", net_get_sock_error(httpd_client_fds[i]));
                status = -kInProgressErr;
            }
            httpd_client_fds[i] = -1;
        }
    }

    return status;
}

static void httpd_suspend_thread( bool warn )
{
    if ( warn )
    {
        httpd_d("Suspending thread");
    } else
    {
        httpd_d("Suspending thread");
    }
    httpd_close_sockets( );
    httpd_state = HTTPD_THREAD_SUSPENDED;
    mico_rtos_suspend_thread( NULL );
}

static int httpd_setup_new_socket( int port )
{
    int one = 1;
    int status, sockfd;
    struct sockaddr_in addr_listen;

    /* create listening TCP socket */
    sockfd = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    if ( sockfd < 0 )
    {
        status = net_get_sock_error( sockfd );
        httpd_d("Socket creation failed: Port: %d Status: %d", port, status);
        return status;
    }

    setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one) );

    addr_listen.sin_family = AF_INET;
    addr_listen.sin_addr.s_addr = INADDR_ANY;
    addr_listen.sin_port = htons( port );

    /* bind insocket */
    status = bind( sockfd, (struct sockaddr *) &addr_listen, sizeof(addr_listen) );
    if ( status < 0 )
    {
        status = net_get_sock_error( sockfd );
        httpd_d("Failed to bind socket on port: %d Status: %d", status, port);
        return status;
    }

    status = listen( sockfd, HTTPD_MAX_BACKLOG_CONN );
    if ( status < 0 )
    {
        status = net_get_sock_error( sockfd );
        httpd_d("Failed to listen on port %d: %d.", port, status);
        return status;
    }

    /* 监听 socket 必须非阻塞: select 报"可读"到 accept() 之间存在窗口期,
     * 排队连接可能已被对端撤销, 此时阻塞的 accept 会永久等待 —— 整个 httpd
     * 线程卡死不再服务, 只有看门狗 stop 的本地连接才能把它唤醒(实测 2 客户端
     * 并发轮询时触发)。非阻塞后无连接时 accept 立即返回 EAGAIN */
    if ( fcntl( sockfd, F_SETFL, O_NONBLOCK ) != 0 )
    {
        int nonblock = 1;
        httpd_d("fcntl non-block failed, fall back to SO_BLOCKMODE");
        setsockopt( sockfd, SOL_SOCKET, SO_BLOCKMODE, &nonblock, sizeof(nonblock) );
    }

    httpd_d("Listening on port %d.", port);
    return sockfd;
}

static int httpd_setup_main_sockets( )
{
    http_sockfd = httpd_setup_new_socket( HTTP_PORT );
    if ( http_sockfd < 0 )
    {
        /* Socket creation failed */
        return http_sockfd;
    }

    return kNoErr;
}

static int httpd_select( int max_sock, const fd_set *readfds,
                         fd_set *active_readfds,
                         int timeout_secs )
{
    int activefds_cnt;
    struct timeval timeout;

    fd_set local_readfds;

    if ( timeout_secs >= 0 )
        timeout.tv_sec = timeout_secs;
    timeout.tv_usec = 0;

    memcpy( &local_readfds, readfds, sizeof(fd_set) );
    httpd_d("WAITING for activity");

    httpd_dbg.in_select = 1;
    activefds_cnt = select(max_sock + 1, &local_readfds, NULL, NULL, timeout_secs >= 0 ? &timeout : NULL);
    httpd_dbg.in_select = 0;
    httpd_dbg.sel++;
    if (activefds_cnt < 0) {
        httpd_dbg.selerr++;
        httpd_dbg.suspend_site = 1;
        httpd_d("Select failed: %d", timeout_secs);
        httpd_suspend_thread( true );
    }

    if ( httpd_stop_req )
    {
        httpd_dbg.suspend_site = 2;
        httpd_d("HTTPD stop request received");
        httpd_stop_req = FALSE;
        httpd_suspend_thread( false );
    }

    if ( activefds_cnt )
    {
        /* Update users copy of fd_set only if he wants */
        if ( active_readfds )
            memcpy( active_readfds, &local_readfds, sizeof(fd_set) );
        return activefds_cnt;
    }

    httpd_d("TIMEOUT");

    return HTTPD_TIMEOUT_EVENT;
}

static int httpd_accept_client_socket( const fd_set *active_readfds, int *client_fd )
{
    int main_sockfd = -1;
    int client_sockfd;
    struct sockaddr addr_from;
    socklen_t addr_from_len;

    if ( FD_ISSET( http_sockfd, active_readfds ) )
    {
        main_sockfd = http_sockfd;
        https_active = FALSE;
    }
    
    addr_from_len = sizeof(addr_from);
    
    client_sockfd = accept( main_sockfd, &addr_from, &addr_from_len );
    if ( client_sockfd < 0 )
    {
        httpd_d("net_accept client socket failed %d.", client_sockfd);
        return -kInProgressErr;
    }
    
    /*
     * Enable TCP Keep-alive for accepted client connection
     *  -- By enabling this feature TCP sends probe packet if there is
     *  inactivity over connection for specfied interval
     *  -- If there is no response to probe packet for specified retries
     *  then connection is closed with RST packet to peer end
     *  -- Ref: http://tldp.org/HOWTO/html_single/TCP-Keepalive-HOWTO/
     *
     * We are doing this as we have single threaded web server with
     * synchronous (blocking) API usage like send, recv and they might get
     * blocked due to un-availability of peer end, causing web server to
     * be in-responsive forever.
     */
    int optval = true;
    if ( setsockopt( client_sockfd, SOL_SOCKET, 0x0008, &optval, sizeof(optval) ) == -1 )
    {
        httpd_d("Unsupported option SO_KEEPALIVE: %d", net_get_sock_error(client_sockfd));
    }
    
    /* TCP Keep-alive idle/inactivity timeout is 10 seconds */
    optval = 10;
    if ( setsockopt( client_sockfd, IPPROTO_TCP, 0x03, &optval, sizeof(optval) ) == -1 )
    {
        httpd_d("Unsupported option TCP_KEEPIDLE: %d", net_get_sock_error(client_sockfd));
    }
    
    /* TCP Keep-alive retry count is 5 */
    optval = 5;
    if ( setsockopt( client_sockfd, IPPROTO_TCP, 0x05, &optval, sizeof(optval) ) == -1 )
    {
        httpd_d("Unsupported option TCP_KEEPCNT: %d", net_get_sock_error(client_sockfd));
    }
    
    /* TCP Keep-alive retry interval (in case no response for probe
     * packet) is 1 second.
     */
    optval = 1;
    if ( setsockopt( client_sockfd, IPPROTO_TCP, 0x04, &optval, sizeof(optval) ) == -1 )
    {
        httpd_d("Unsupported option TCP_KEEPINTVL: %d", net_get_sock_error(client_sockfd));
    }

    httpd_d("connecting %d to %d.", client_sockfd, addr_from.s_port);

    *client_fd = client_sockfd;
    return kNoErr;
}

static int httpd_free_client_slot( void )
{
    int i;

    for ( i = 0; i < HTTPD_MAX_CLIENTS; i++ )
    {
        if ( httpd_client_fds[i] == -1 )
            return i;
    }
    return -1;
}

/* 回收空闲超时的 keep-alive 连接(5s ≥ 浏览器 3s 轮询间隔) */
static void httpd_reap_idle_clients( void )
{
    uint32_t now = mico_rtos_get_time( );
    int i;

    for ( i = 0; i < HTTPD_MAX_CLIENTS; i++ )
    {
        if ( httpd_client_fds[i] != -1 &&
             (uint32_t)( now - httpd_client_lastact[i] ) >= HTTPD_CLIENT_SOCK_TIMEOUT * 1000 )
        {
            httpd_d("Client socket %d timeout occurred. Force closing socket", httpd_client_fds[i]);
            if ( close( httpd_client_fds[i] ) != 0 )
                httpd_d("Failed to close socket %d", net_get_sock_error(httpd_client_fds[i]));
            httpd_client_fds[i] = -1;
            httpd_dbg.reap++;
        }
    }
}

/* 连接数达上限时让位: 关闭最久未活动的连接, 优先接纳排队的新客户端(看门狗探针),
 * 避免其等满 8s 超时误判 httpd 卡死 */
static void httpd_drop_oldest_client( void )
{
    int i, oldest = -1;

    for ( i = 0; i < HTTPD_MAX_CLIENTS; i++ )
    {
        if ( httpd_client_fds[i] == -1 )
            continue;
        if ( oldest == -1 ||
             (int32_t)( httpd_client_lastact[i] - httpd_client_lastact[oldest] ) < 0 )
            oldest = i;
    }

    if ( oldest >= 0 )
    {
        httpd_d("Client limit reached, closing oldest socket %d", httpd_client_fds[oldest]);
        if ( close( httpd_client_fds[oldest] ) != 0 )
            httpd_d("Failed to close socket %d", net_get_sock_error(httpd_client_fds[oldest]));
        httpd_client_fds[oldest] = -1;
        httpd_dbg.evict++;
    }
}

static void httpd_main( mico_thread_arg_t arg )
{
    UNUSED_PARAMETER( arg );
    int status, i, max_sockfd;
    fd_set readfds, active_readfds;

    status = httpd_setup_main_sockets( );
    if ( status != kNoErr )
    {
        httpd_dbg.suspend_site = 3;
        httpd_suspend_thread( true );
    }

    for ( i = 0; i < HTTPD_MAX_CLIENTS; i++ )
        httpd_client_fds[i] = -1;

    while ( 1 )
    {
        httpd_dbg.loops++;
        FD_ZERO( &readfds );
        FD_SET( http_sockfd, &readfds );
        max_sockfd = http_sockfd;
        for ( i = 0; i < HTTPD_MAX_CLIENTS; i++ )
        {
            if ( httpd_client_fds[i] != -1 )
            {
                FD_SET( httpd_client_fds[i], &readfds );
                if ( httpd_client_fds[i] > max_sockfd )
                    max_sockfd = httpd_client_fds[i];
            }
        }

        httpd_d("Waiting on main socket");
        if ( httpd_select( max_sockfd, &readfds, &active_readfds, HTTPD_MAIN_TICK_SECS ) == HTTPD_TIMEOUT_EVENT )
        {
            httpd_dbg.ticks++;
            httpd_reap_idle_clients( );
            continue;
        }

        /* 新连接排队: 有闲置槽位则接纳, 否则让位给最久未活动的连接 */
        if ( FD_ISSET( http_sockfd, &active_readfds ) )
        {
            int fd = -1;

            httpd_dbg.in_accept = 1;
            status = httpd_accept_client_socket( &active_readfds, &fd );
            httpd_dbg.in_accept = 0;
            if ( status == kNoErr && fd >= 0 )
                httpd_dbg.acc++;
            else
                httpd_dbg.accfail++;

            if ( status == kNoErr && fd >= 0 )
            {
                int slot = httpd_free_client_slot( );
                if ( slot < 0 )
                {
                    httpd_drop_oldest_client( );
                    slot = httpd_free_client_slot( );
                }

                if ( slot >= 0 )
                {
                    httpd_client_fds[slot] = fd;
                    httpd_client_lastact[slot] = mico_rtos_get_time( );
                    httpd_d("Client socket accepted: %d", fd);
                }
                else
                {
                    httpd_d("No free client slot, closing %d", fd);
                    close( fd );
                }
            }
            /* 每轮只处理一个事件, 保证公平且状态简单 */
            continue;
        }

        /* 已建立连接有数据: 处理一个请求。处理保持串行(共享全局 httpd_req),
         * keep-alive 连接在两次请求之间可以共存, 不再为了排队者互相断开重连 */
        for ( i = 0; i < HTTPD_MAX_CLIENTS; i++ )
        {
            int fd = httpd_client_fds[i];
            if ( fd == -1 || !FD_ISSET( fd, &active_readfds ) )
                continue;

            httpd_d("Handling %d", fd);
            /* Note:
             * Connection will be handled with call to
             * httpd_handle_message twice, first for
             * handling request (kNoErr) and second
             * time as there is no more data to receive
             * (client closed connection) and hence
             * will return with status HTTPD_DONE
             * closing socket.
             */
            {
                uint32_t t0 = mico_rtos_get_time( );
                httpd_dbg.in_handle = 1;
                httpd_dbg.io_kind = 0;
                httpd_dbg.io_loop = 0;
                status = httpd_handle_message( fd );
                httpd_dbg.in_handle = 0;
                httpd_dbg.handle_ms = mico_rtos_get_time( ) - t0;
                httpd_dbg.last_status = status;
                httpd_dbg.stage = 0;
                httpd_dbg.io_kind = 0;
            }
            if ( status == kNoErr )
            {
                httpd_dbg.srv++;
                /* keep-alive: 应答完成, 保持连接等待该客户端的下一个请求 */
                httpd_client_lastact[i] = mico_rtos_get_time( );
            }
            else
            {
                httpd_dbg.srvfail++;
                httpd_d("Close socket %d.  %s: %d", fd, status == HTTPD_DONE ? "Handler done" : "Handler failed", status);
                if ( close( fd ) != 0 )
                    httpd_d("Failed to close socket %d", net_get_sock_error(fd));
                httpd_client_fds[i] = -1;
            }
            break;
        }
    }

    /*
     * Thread will never come here. The functions called from the above
     * infinite loop will cleanly shutdown this thread when situation
     * demands so.
     */
}

static inline int tcp_local_connect( int *sockfd )
{
    uint16_t port;
    int retry_cnt = 3;

    httpd_d("Doing local connect for shutting down server\n\r");

    *sockfd = -1;
    while ( retry_cnt-- )
    {
        *sockfd = socket( AF_INET, SOCK_STREAM, 0 );
        if ( *sockfd >= 0 )
            break;
        /* Wait some time to allow some sockets to get released */
        mico_thread_msleep( 1000 );
    }

    if ( *sockfd < 0 )
    {
        httpd_d("Unable to create socket to stop server");
        return -kInProgressErr;
    }

    port = HTTP_PORT;

    char *host = "127.0.0.1";
    struct sockaddr_in addr;
    memset( &addr, 0, sizeof(struct sockaddr_in) );

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr( host );
    addr.sin_port = htons( port );

    httpd_d("local connecting ...");
    if ( connect( *sockfd, (struct sockaddr *) &addr, sizeof(addr) ) != 0 )
    {
        httpd_d("Server close error. tcp connect failed %s:%d", host, port);
        close( *sockfd );
        *sockfd = 0;
        return -kInProgressErr;
    }

    /*
     * We do not wish to do anything with this connection. Its sole
     * purpose was to wake the main httpd thread out of sleep.
     */

    return kNoErr;
}

static int httpd_signal_and_wait_for_halt( )
{
    const int total_wait_time_ms = 1000 * 20; /* 20 seconds */
    const int check_interval_ms = 100; /* 100 ms */

    int num_iterations = total_wait_time_ms / check_interval_ms;

    httpd_d("Sent stop request");
    httpd_stop_req = TRUE;

    /* Do a dummy local connect to wakeup the httpd thread */
    int sockfd;
    int rv = tcp_local_connect( &sockfd );
    if ( rv != kNoErr )
    {
        /* 本地连接失败时不能把 stop_req 留下: 否则下一次 httpd_start 起来的
         * 新线程会在首个 select 里立刻自挂起(state=SUSPENDED/listen=-1),
         * 网页从此永久失联 —— 实测看门狗重启后正是死在这个状态 */
        httpd_stop_req = FALSE;
        return rv;
    }

    while ( httpd_state != HTTPD_THREAD_SUSPENDED && num_iterations-- )
    {
        mico_thread_msleep( check_interval_ms );
    }

    close( sockfd );
    if ( httpd_state == HTTPD_THREAD_SUSPENDED )
        return kNoErr;

    httpd_d("Timed out waiting for httpd to stop. " "Force closed temporary socket");

    httpd_stop_req = FALSE;
    return -kInProgressErr;
}

static int httpd_thread_cleanup( void )
{
    int status = kNoErr;

    switch ( httpd_state )
    {
        case HTTPD_INIT_DONE:
            /*
             * We have no threads, no sockets to close.
             */
            break;
        case HTTPD_THREAD_RUNNING:
            status = httpd_signal_and_wait_for_halt( );
            if ( status != kNoErr )
                httpd_d("Unable to stop thread. Force killing it.");
            /* No break here on purpose */
        case HTTPD_THREAD_SUSPENDED:
            status = mico_rtos_delete_thread( &httpd_main_thread );
            if ( status != kNoErr )
                httpd_d("Failed to delete thread.");
            status = httpd_close_sockets( );
            httpd_state = HTTPD_INIT_DONE;
            break;
        default:
            return -kInProgressErr;
    }

    return status;
}

int httpd_is_running( void )
{
    return (httpd_state == HTTPD_THREAD_RUNNING);
}

/* This pairs with httpd_stop() */
int httpd_start( void )
{
    int status;

    if ( httpd_state != HTTPD_INIT_DONE )
    {
        httpd_d("Already started");
        return kNoErr;
    }

    status = mico_rtos_create_thread( &httpd_main_thread, MICO_APPLICATION_PRIORITY, "httpd",
                                      httpd_main,
                                      http_server_thread_stack_size, 0 );

    if ( status != kNoErr )
    {
        httpd_d("Failed to create httpd thread: %d", status);
        return -kInProgressErr;
    }

    httpd_state = HTTPD_THREAD_RUNNING;
    return kNoErr;
}

/* This pairs with httpd_start() */
int httpd_stop( void )
{
    return httpd_thread_cleanup( );
}

/* This pairs with httpd_init() */
int httpd_shutdown( void )
{
    int ret;

    httpd_d("Shutting down.");

    ret = httpd_thread_cleanup( );
    if ( ret != kNoErr )
        httpd_d("Thread cleanup failed");

    httpd_state = HTTPD_INACTIVE;

    return ret;
}

/* This pairs with httpd_shutdown() */
int httpd_init( )
{
    int status;

    if ( httpd_state != HTTPD_INACTIVE )
        return kNoErr;

    httpd_d("Initializing");

    for ( int i = 0; i < HTTPD_MAX_CLIENTS; i++ )
        httpd_client_fds[i] = -1;
    http_sockfd = -1;

    status = httpd_wsgi_init( );
    if ( status != kNoErr )
    {
        httpd_d("Failed to initialize WSGI!");
        return status;
    }

    status = httpd_ssi_init( );
    if ( status != kNoErr )
    {
        httpd_d("Failed to initialize SSI!");
        return status;
    }

    httpd_state = HTTPD_INIT_DONE;

    return kNoErr;
}

/* telnet `httpd` 命令的诊断输出: 观察一会儿内 loop 是否增长、哪个 in_*=1,
 * 即可区分"线程卡死在某调用内"与"线程活着但不服务请求" */
char *httpd_debug_info( void )
{
    static char info[512];
    uint32_t now = mico_rtos_get_time( );
    int i, n;

    n = snprintf( info, sizeof(info),
        "state=%d listen=%d up=%lus\r\n"
        "loop=%lu tick=%lu sel=%lu selerr=%lu susp=%lu\r\n"
        "acc=%lu afail=%lu srv=%lu sfail=%lu reap=%lu evict=%lu\r\n"
        "in sel/acc/hnd=%lu/%lu/%lu hnd_ms=%lu lastst=%ld\r\n"
        "stg=%lu fd=%lu age=%lums fn=%s io=%lu/%lu/%lu\r\n"
        "slot:",
        (int) httpd_state, http_sockfd, (unsigned long) ( now / 1000 ),
        (unsigned long) httpd_dbg.loops, (unsigned long) httpd_dbg.ticks,
        (unsigned long) httpd_dbg.sel, (unsigned long) httpd_dbg.selerr,
        (unsigned long) httpd_dbg.suspend_site,
        (unsigned long) httpd_dbg.acc, (unsigned long) httpd_dbg.accfail,
        (unsigned long) httpd_dbg.srv, (unsigned long) httpd_dbg.srvfail,
        (unsigned long) httpd_dbg.reap, (unsigned long) httpd_dbg.evict,
        (unsigned long) httpd_dbg.in_select, (unsigned long) httpd_dbg.in_accept,
        (unsigned long) httpd_dbg.in_handle, (unsigned long) httpd_dbg.handle_ms,
        (long) httpd_dbg.last_status,
        (unsigned long) httpd_dbg.stage, (unsigned long) httpd_dbg.stage_fd,
        (unsigned long) ( httpd_dbg.stage ? now - httpd_dbg.stage_ms : 0 ),
        httpd_dbg.stage_fn,
        (unsigned long) httpd_dbg.io_kind, (unsigned long) httpd_dbg.io_fd,
        (unsigned long) httpd_dbg.io_loop );

    if ( n < 0 )
        n = 0;
    for ( i = 0; i < HTTPD_MAX_CLIENTS && n < (int) sizeof(info) - 1; i++ )
    {
        if ( httpd_client_fds[i] == -1 )
            n += snprintf( info + n, sizeof(info) - n, " -" );
        else
            n += snprintf( info + n, sizeof(info) - n, " %d/%lums",
                           httpd_client_fds[i],
                           (unsigned long) ( now - httpd_client_lastact[i] ) );
    }

    return info;
}

int httpd_use_tls_certificates( const httpd_tls_certs_t *tls_certs )
{

    httpd_d("HTTPS is not enabled in server. ");
    return -kInProgressErr;
}

static char *auth_str = NULL;

int httpd_auth_init(char *name, char *passwd)
{
  int len, outlen;
  char *src_str;
  
  len = strlen(name) + strlen(passwd) + 2;
  
  if (auth_str)
    free(auth_str);
  
  auth_str = NULL;
  if (strlen(name) == 0 && strlen(passwd) == 0) // no username and password
    return 0;
  
  src_str = malloc(len);
  if (src_str == 0)
    return -1;
  
  sprintf(src_str, "%s:%s", name, passwd);
  auth_str = (char *)base64_encode((unsigned char const *)src_str, strlen(src_str), &outlen); 
  len = strlen(auth_str);
  auth_str[len-1] = 0;
  free(src_str);
  return kNoErr;
}

char *get_httpd_auth( void )
{
  return auth_str;
}
