/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/**
 * This file contains IO operations that use nginx
 *
 * @author Sergey Avseyev
 */

#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"

#include "ngx_lcb_module.h"

typedef struct {
    struct lcb_io_opt_st base;
    ngx_log_t *log;
    ngx_pool_t *pool;
} ngx_lcb_io_opt_t;

typedef struct {
    lcb_sockdata_t base;
    ngx_peer_connection_t *peer;
    lcb_io_connect_cb connected;
    lcb_io_connect_cb read_callback;
    ngx_chain_t *out_head;
    ngx_chain_t *out_tail;
} ngx_lcb_socket_t;

typedef struct {
    lcb_io_writebuf_t base;
    ngx_lcb_socket_t *sock;
} ngx_lcb_writebuf_t;

typedef struct {
    ngx_chain_t chain;
    ngx_buf_t buf;
    ngx_lcb_writebuf_t *parent;
    lcb_io_write_cb callback;
} ngx_lcb_request_t;

static void ngx_lcb_destroy_io_opts(lcb_io_opt_t iobase);
static void* ngx_lcb_timer_create(lcb_io_opt_t io);
static void ngx_lcb_timer_thunk(ngx_event_t *ev);
static void ngx_lcb_noop(lcb_io_opt_t io);
static void ngx_lcb_destroy_io_opts(lcb_io_opt_t iobase);
static void ngx_lcb_timer_destroy(lcb_io_opt_t io, void *timer);
void ngx_lcb_timer_delete(lcb_io_opt_t io, void *timer);
static int ngx_lcb_timer_update(lcb_io_opt_t io, void *timer, lcb_uint32_t usec, void *data, void (*handler)(lcb_socket_t sock, short which, void *arg));
static lcb_sockdata_t* ngx_lcb_create_socket(lcb_io_opt_t iobase, int domain, int type, int protocol);
static int ngx_lcb_get_nameinfo(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase, struct lcb_nameinfo_st *ni);
static unsigned int ngx_lcb_close_socket(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase);
static void ngx_lcb_connect_handler_thunk(ngx_event_t *ev);
static int ngx_lcb_start_connect(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase, const struct sockaddr *name, unsigned int namelen, lcb_io_connect_cb callback);
static lcb_io_writebuf_t* ngx_lcb_create_writebuf(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase);
static void ngx_lcb_release_writebuf(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase, lcb_io_writebuf_t *bufbase);
static void ngx_lcb_handler_thunk(ngx_event_t *ev);
static int ngx_lcb_start_write(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase, lcb_io_writebuf_t *bufbase, lcb_io_write_cb callback);
static int ngx_lcb_start_read(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase, lcb_io_read_cb callback);
static void ngx_lcb_send_error(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase, lcb_io_error_cb callback);

static void
ngx_lcb_noop(lcb_io_opt_t io)
{
    (void)io;
}

lcb_error_t
ngx_lcb_create_io_opts(int version, lcb_io_opt_t *io, void *cookie)
{
    ngx_lcb_io_opt_t *ret;
    ngx_lcb_cookie_t c = cookie;

    if (cookie == NULL) {
        return LCB_EINVAL;
    }
    if (version != 0) {
        return LCB_PLUGIN_VERSION_MISMATCH;
    }
    ret = ngx_pcalloc(c->pool, sizeof(ngx_lcb_io_opt_t));
    if (ret == NULL) {
        ngx_pfree(c->pool, ret);
        return LCB_CLIENT_ENOMEM;
    }
    ret->log = c->log;
    ret->pool = c->pool;

    /* setup io iops! */
    ret->base.version = 1;
    ret->base.dlhandle = NULL;
    ret->base.destructor = ngx_lcb_destroy_io_opts;
    /* consider that struct isn't allocated by the library,
     * `need_cleanup' flag might be set in lcb_create() */
    ret->base.v.v0.need_cleanup = 0;

    ret->base.v.v1.create_timer = ngx_lcb_timer_create;
    ret->base.v.v1.destroy_timer = ngx_lcb_timer_destroy;
    ret->base.v.v1.delete_timer = ngx_lcb_timer_delete;
    ret->base.v.v1.update_timer = ngx_lcb_timer_update;

    ret->base.v.v1.create_socket = ngx_lcb_create_socket;
    ret->base.v.v1.get_nameinfo = ngx_lcb_get_nameinfo;
    ret->base.v.v1.close_socket = ngx_lcb_close_socket;
    ret->base.v.v1.start_connect = ngx_lcb_start_connect;

    ret->base.v.v1.create_writebuf = ngx_lcb_create_writebuf;
    ret->base.v.v1.release_writebuf = ngx_lcb_release_writebuf;
    ret->base.v.v1.start_write = ngx_lcb_start_write;
    ret->base.v.v1.start_read = ngx_lcb_start_read;
    ret->base.v.v1.send_error = ngx_lcb_send_error;

    ret->base.v.v1.run_event_loop = ngx_lcb_noop;
    ret->base.v.v1.stop_event_loop = ngx_lcb_noop;

    *io = (lcb_io_opt_t)ret;
    return LCB_SUCCESS;
}

static void
ngx_lcb_destroy_io_opts(lcb_io_opt_t iobase)
{
    ngx_lcb_io_opt_t *io = (ngx_lcb_io_opt_t *)iobase;
    free(io);
}

static void
ngx_lcb_timer_thunk(ngx_event_t *ev)
{
    (void)ev;
#if 0
    ngx_lcb_socket_t *sock = ev->data;

    ctx->handler(to_socket(-1), 0, ctx->handler_data);
#endif
}


static void *
ngx_lcb_timer_create(lcb_io_opt_t io)
{
    (void)io;
#if 0
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    ngx_event_t *tm;
    ngx_lcb_context_t *ctx;

    tm = ngx_pcalloc(cookie->pool, sizeof(ngx_event_t));
    if (tm == NULL) {
        return NULL;
    }
    ctx = ngx_pcalloc(cookie->pool, sizeof(ngx_lcb_context_t));
    if (ctx == NULL) {
        ngx_pfree(cookie->pool, tm);
        return NULL;
    }
    tm->handler = ngx_lcb_timer_thunk;
    tm->data = ctx;
    tm->log = cookie->log;
    return tm;
#endif
    return (void *)0xdeadbeef;
}

static void
ngx_lcb_timer_destroy(lcb_io_opt_t io, void *timer)
{
    (void)io;
    (void)timer;
#if 0
    ngx_lcb_cookie_t cookie = io->v.v0.cookie;
    ngx_event_t *tm = timer;

    ngx_pfree(cookie->pool, tm);
#endif
}

void
ngx_lcb_timer_delete(lcb_io_opt_t io, void *timer)
{
    (void)timer;
#if 0
    ngx_event_t *tm = timer;

    if (tm->timer_set) {
        ngx_del_timer(tm);
    }
#endif
    (void)io;
}

static int
ngx_lcb_timer_update(lcb_io_opt_t io, void *timer, lcb_uint32_t usec, void *data, void (*handler)(lcb_socket_t sock, short which, void *arg))
{
    (void)timer;
    (void)handler;
    (void)data;
    (void)usec;
#if 0
    ngx_event_t *tm = timer;
    ngx_lcb_context_t *ctx;

    ctx = tm->data;
    ctx->handler = handler;
    ctx->handler_data = data;
    ngx_add_timer(tm, usec / 1000);
#endif
    (void)io;
    return 0;
}

static lcb_sockdata_t *
ngx_lcb_create_socket(lcb_io_opt_t iobase, int domain, int type, int protocol)
{
    ngx_lcb_io_opt_t *io = (ngx_lcb_io_opt_t *)iobase;
    ngx_lcb_socket_t *ret;

    ret = ngx_pcalloc(io->pool, sizeof(ngx_lcb_socket_t));
    if (ret == NULL) {
        return NULL;
    }
    ret->peer = ngx_pcalloc(io->pool, sizeof(ngx_peer_connection_t));
    if (ret->peer == NULL) {
        ngx_pfree(io->pool, ret);
        return NULL;
    }
    ret->peer->log = io->log;
    ret->peer->log_error = NGX_ERROR_ERR;
    ret->peer->get = ngx_event_get_peer;
    (void)domain;
    (void)type;
    (void)protocol;
    return (lcb_sockdata_t *)ret;
}

static int
ngx_lcb_get_nameinfo(lcb_io_opt_t iobase,
                     lcb_sockdata_t *sockbase,
                     struct lcb_nameinfo_st *ni)
{
    ngx_lcb_socket_t *sock = (ngx_lcb_socket_t *)sockbase;
    ngx_socket_t fd = sock->peer->connection->fd;

    if (getpeername(fd, ni->remote.name, (socklen_t *)ni->remote.len)) {
        return -1;
    }
    if (getsockname(fd, ni->local.name, (socklen_t *)ni->local.len)) {
        return -1;
    }
    (void)iobase;
    return 0;
}

static unsigned int
ngx_lcb_close_socket(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase)
{
    ngx_lcb_io_opt_t *io = (ngx_lcb_io_opt_t *)iobase;
    ngx_lcb_socket_t *sock = (ngx_lcb_socket_t *)sockbase;

    if (sock->peer->connection != NULL) {
        ngx_close_connection(sock->peer->connection);
        sock->peer->connection = NULL;
    }
    ngx_pfree(io->pool, sock->peer->name);
    ngx_pfree(io->pool, sock->peer);
    ngx_pfree(io->pool, sock);
    return 0;
}

static void
ngx_lcb_connect_handler_thunk(ngx_event_t *ev)
{
    ngx_connection_t *conn = ev->data;
    ngx_lcb_socket_t *sock = conn->data;

    conn->read->handler = ngx_lcb_handler_thunk;
    conn->write->handler = ngx_lcb_handler_thunk;
    sock->connected(&sock->base, LCB_SUCCESS);
    /* check for broken connection */
}

static int
ngx_lcb_start_connect(lcb_io_opt_t iobase,
                      lcb_sockdata_t *sockbase,
                      const struct sockaddr *name,
                      unsigned int namelen,
                      lcb_io_connect_cb callback)
{
    ngx_lcb_io_opt_t *io = (ngx_lcb_io_opt_t *)iobase;
    ngx_lcb_socket_t *sock = (ngx_lcb_socket_t *)sockbase;
    ngx_peer_connection_t *peer = sock->peer;
    size_t len;
    ngx_int_t rc;

    peer->sockaddr = (struct sockaddr *)name;
    peer->socklen = namelen;
    peer->name = ngx_pnalloc(io->pool, sizeof(ngx_str_t));
    if (peer->name == NULL) {
        return -1;
    }
    len = NGX_INET_ADDRSTRLEN + sizeof(":65535") - 1;
    peer->name->data = ngx_pnalloc(io->pool, len);
    if (peer->name->data == NULL) {
        ngx_pfree(io->pool, peer->name);
        return -1;
    }
    peer->name->len = ngx_sock_ntop(peer->sockaddr, peer->name->data, len, 1);
    rc = ngx_event_connect_peer(peer);
    if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
        io->base.v.v0.error = ngx_socket_errno;
        return -1;
    }
    peer->connection->data = sock;
    if (rc == NGX_AGAIN) {
        peer->connection->write->handler = ngx_lcb_connect_handler_thunk;
        peer->connection->read->handler = ngx_lcb_connect_handler_thunk;
        sock->connected = callback;
        io->base.v.v1.error = EINPROGRESS;
        return 0;
    }
    peer->connection->read->handler = ngx_lcb_handler_thunk;
    peer->connection->write->handler = ngx_lcb_handler_thunk;
    return 0;
}

static lcb_io_writebuf_t *
ngx_lcb_create_writebuf(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase)
{
    ngx_lcb_io_opt_t *io = (ngx_lcb_io_opt_t *)iobase;
    ngx_lcb_socket_t *sock = (ngx_lcb_socket_t *)sockbase;
    ngx_lcb_writebuf_t *ret;

    ret = ngx_pnalloc(io->pool, sizeof(ngx_lcb_io_opt_t));
    if (ret == NULL) {
        return NULL;
    }
    ret->base.parent = iobase;
    ret->sock = sock;
    return (lcb_io_writebuf_t *)ret;
}

static void
ngx_lcb_release_writebuf(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase, lcb_io_writebuf_t *bufbase)
{
    ngx_lcb_writebuf_t *buf = (ngx_lcb_writebuf_t *)bufbase;
    struct lcb_buf_info *bi = &buf->base.buffer;

    if (bi->root || bi->ringbuffer) {
        lcb_assert((void *)bi->root != (void *)bi->ringbuffer);
    }
    lcb_assert((bi->ringbuffer == NULL && bi->root == NULL) ||
               (bi->root && bi->ringbuffer));
    lcb_mem_free(bi->root);
    lcb_mem_free(bi->ringbuffer);
    bi->root = NULL;
    bi->ringbuffer = NULL;
    (void)iobase;
    (void)sockbase;
}

static void
ngx_lcb_handler_thunk(ngx_event_t *ev)
{
    ngx_connection_t *conn = ev->data;
    ngx_lcb_socket_t *sock = conn->data;

    if (ev->write) {
        ngx_chain_t *ret, *cur;
        ssize_t old;
        int status = 0;

        old = conn->sent;
        ret = conn->send_chain(conn, sock->out_head, 0);
        if (ret == NGX_CHAIN_ERROR) {
            io->v.v1.error = ngx_socket_errno;
            ret = sock->out_tail;
            status = -1;
        }
        cur = sock->out_head;
        do {
            ngx_lcb_request_t *req = (ngx_lcb_request_t *)cur;
            if (conn->sent - old >= 0) {
                if (req->callback) {
                    req->callback(sock->base, req->parent, status);
                }
                ngx_pfree(req);
                cur = req->chain.next;
            }
        } while (cur != ret);
        sock->out_head = cur;
    } else {
        ngx_chain_t chains[2];
        ngx_buf_t bufs[2];
        struct lcb_iovec_st *iov;
        ssize_t ret;
        int i;

        iov = sock->base.read_buffer.iov;
        for (i = 0; i < 2; i++) {
            chains[i].buf = &bufs[i];
            bufs[i].memory = 1;
            bufs[i].start = (u_char *)iov[i].iov_base;
            bufs[i].end = (u_char *)bufs[i].start + iov[i].iov_len;
            bufs[i].pos = bufs[i].start;
            bufs[i].last = bufs[i].start;
        }
        chains[0].next = &chains[1];
        chains[1].next = NULL;

        ret = conn->recv_chain(conn, &chains[0]);
        if (ret < 0) {
            io->v.v0.error = ngx_socket_errno;
        }
        sock->read_callback(sock->base, ret);
    }
}

static int
ngx_lcb_start_write(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase, lcb_io_writebuf_t *bufbase, lcb_io_write_cb callback)
{
    ngx_lcb_io_opt_t *io = (ngx_lcb_io_opt_t *)iobase;
    ngx_lcb_writebuf_t *buf = (ngx_lcb_writebuf_t *)bufbase;
    ngx_lcb_socket_t *sock = (ngx_lcb_socket_t *)sockbase;
    ngx_lcb_request_t *req;
    int nreq, i;

    nreq = (buf->base.buffer.iov[1].iov_len > 0) ? 2 : 1;
    req = ngx_pnalloc(io->pool, nreq * sizeof(ngx_lcb_io_opt_t));
    if (req == NULL) {
        return -1;
    }
    for (i = 0; i < 2; i++) {
        req[i].parent = buf;
        req[i].callback = callback;
        req[i].buf.memory = 1;
        req[i].buf.start = (u_char *)buf->base.buffer.iov[i].iov_base;
        req[i].buf.end = (u_char *)req[i].buf.start + buf->base.buffer.iov[i].iov_len;
        req[i].buf.pos = req[i].buf.start;
        req[i].chain.buf = &req[i].buf;
        /* append the chain to output queue and set last
           bb[ii].last = recv ? bb[ii].start : bb[ii].end;
           cc[ii].next = cc + ii + 1;
         */
        ngx_lcb_io_push_request(sock, &req[i]);
    }
    /* trigger callback only when the latest buffer done */
    if (nreq > 1) {
        req[0].callback = NULL;
    }
    {
        ngx_int_t f;
        if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {
            /* kqueue */
            f = NGX_CLEAR_EVENT;
        } else {
            /* select, poll, /dev/poll */
            f = NGX_LEVEL_EVENT;
        }
        /* TODO check result */
        ngx_add_event(sock->peer->connection->write, NGX_WRITE_EVENT, f);
    }
    return 0;
}

static int
ngx_lcb_start_read(lcb_io_opt_t iobase, lcb_sockdata_t *sockbase, lcb_io_read_cb callback)
{
    ngx_lcb_io_opt_t *io = (ngx_lcb_io_opt_t *)iobase;
    ngx_lcb_socket_t *sock = (ngx_lcb_socket_t *)sockbase;

    sock->read_callback = callback;
}


static void
ngx_lcb_send_error(lcb_io_opt_t iobase,
                   lcb_sockdata_t *sockbase,
                   lcb_io_error_cb callback)
{
    (void)iobase;
    (void)sockbase;
    (void)callback;
}
