
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>

#include <liburing.h>


typedef struct {
} ngx_epoll_conf_t;


static ngx_int_t ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer);
static void ngx_epoll_done(ngx_cycle_t *cycle);
static ngx_int_t ngx_epoll_add_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_epoll_del_event(ngx_event_t *ev, ngx_int_t event,
    ngx_uint_t flags);
static ngx_int_t ngx_epoll_add_connection(ngx_connection_t *c);
static ngx_int_t ngx_epoll_del_connection(ngx_connection_t *c,
    ngx_uint_t flags);
static ngx_int_t ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer,
    ngx_uint_t flags);

static void *ngx_epoll_create_conf(ngx_cycle_t *cycle);
static char *ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf);

struct io_uring             ngx_ring;

ngx_uint_t                  ngx_use_epoll_rdhup = EPOLLRDHUP;

static ngx_str_t      epoll_name = ngx_string("epoll");

static ngx_command_t  ngx_epoll_commands[] = {

      ngx_null_command
};


static ngx_event_module_t  ngx_epoll_module_ctx = {
    &epoll_name,
    ngx_epoll_create_conf,               /* create configuration */
    ngx_epoll_init_conf,                 /* init configuration */

    {
        ngx_epoll_add_event,             /* add an event */
        ngx_epoll_del_event,             /* delete an event */
        ngx_epoll_add_event,             /* enable an event */
        ngx_epoll_del_event,             /* disable an event */
        ngx_epoll_add_connection,        /* add an connection */
        ngx_epoll_del_connection,        /* delete an connection */
        NULL,                            /* trigger a notify */
        ngx_epoll_process_events,        /* process the events */
        ngx_epoll_init,                  /* init the events */
        ngx_epoll_done,                  /* done the events */
    }
};

ngx_module_t  ngx_epoll_module = {
    NGX_MODULE_V1,
    &ngx_epoll_module_ctx,               /* module context */
    ngx_epoll_commands,                  /* module directives */
    NGX_EVENT_MODULE,                    /* module type */
    NULL,                                /* init master */
    NULL,                                /* init module */
    NULL,                                /* init process */
    NULL,                                /* init thread */
    NULL,                                /* exit thread */
    NULL,                                /* exit process */
    NULL,                                /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    if (ngx_ring.ring_fd == 0) {

        int ret = io_uring_queue_init(cycle->connection_n, &ngx_ring, 0);

        if (ret < 0) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                          "io_uring_queue_init() failed");
            return NGX_ERROR;
        }

    }

    ngx_io = ngx_os_io;

    ngx_event_actions = ngx_epoll_module_ctx.actions;

#if (NGX_HAVE_CLEAR_EVENT)
    ngx_event_flags = NGX_USE_CLEAR_EVENT
#else
    ngx_event_flags = NGX_USE_LEVEL_EVENT
#endif
                      |NGX_USE_GREEDY_EVENT
                      |NGX_USE_EPOLL_EVENT;

    return NGX_OK;
}


static void
ngx_epoll_done(ngx_cycle_t *cycle)
{
    io_uring_queue_exit(&ngx_ring);
    ngx_ring.ring_fd = 0;
}


static ngx_int_t
ngx_epoll_add_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    int                  op;
    uint32_t             events, prev;
    ngx_event_t         *e;
    ngx_connection_t    *c;
    struct epoll_event   ee;

    c = ev->data;

    events = (uint32_t) event;

    if (event == NGX_READ_EVENT) {
        e = c->write;
        prev = EPOLLOUT;
#if (NGX_READ_EVENT != EPOLLIN|EPOLLRDHUP)
        events = EPOLLIN|EPOLLRDHUP;
#endif

    } else {
        e = c->read;
        prev = EPOLLIN|EPOLLRDHUP;
#if (NGX_WRITE_EVENT != EPOLLOUT)
        events = EPOLLOUT;
#endif
    }

    if (e->active) {
        op = EPOLL_CTL_MOD;
        events |= prev;

    } else {
        op = EPOLL_CTL_ADD;
    }

    if (flags & NGX_EXCLUSIVE_EVENT) {
        events &= ~EPOLLRDHUP;
    }
    ee.events = events | (uint32_t) flags;
    ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ngx_ring);
    if (op == EPOLL_CTL_ADD) {
        io_uring_prep_poll_add(sqe, c->fd, ee.events);
        sqe->len |= IORING_POLL_ADD_MULTI;
        io_uring_sqe_set_data(sqe, ee.data.ptr);
    } else {
        io_uring_prep_poll_update(sqe, ee.data.ptr, NULL, ee.events, IORING_POLL_UPDATE_EVENTS);
    }

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "epoll add event: fd:%d op:%d ev:%08XD",
                   c->fd, op, ee.events);

    if (io_uring_submit(&ngx_ring) < 0) {
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "io_uring_submit(poll_add) failed");
        return NGX_ERROR;
    }

    ev->active = 1;
#if 0
    ev->oneshot = (flags & NGX_ONESHOT_EVENT) ? 1 : 0;
#endif

    return NGX_OK;
}


static ngx_int_t
ngx_epoll_del_event(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags)
{
    int                  op;
    uint32_t             prev;
    ngx_event_t         *e;
    ngx_connection_t    *c;
    struct epoll_event   ee;

    /*
     * when the file descriptor is closed, the epoll automatically deletes
     * it from its queue, so we do not need to delete explicitly the event
     * before the closing the file descriptor
     */

    if (flags & NGX_CLOSE_EVENT) {
        ev->active = 0;
        return NGX_OK;
    }

    c = ev->data;

    if (event == NGX_READ_EVENT) {
        e = c->write;
        prev = EPOLLOUT;

    } else {
        e = c->read;
        prev = EPOLLIN|EPOLLRDHUP;
    }

    if (e->active) {
        op = EPOLL_CTL_MOD;
        ee.events = prev | (uint32_t) flags;
        ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);

    } else {
        op = EPOLL_CTL_DEL;
        ee.events = 0;
        ee.data.ptr = (void *) ((uintptr_t) c | ev->instance);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ngx_ring);
    if (op == EPOLL_CTL_MOD) {
        io_uring_prep_poll_update(sqe, ee.data.ptr, NULL, ee.events, IORING_POLL_UPDATE_EVENTS);
    } else {
        io_uring_prep_poll_remove(sqe, ee.data.ptr);
    }

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "epoll del event: fd:%d op:%d ev:%08XD",
                   c->fd, op, ee.events);

    if (io_uring_submit(&ngx_ring) < 0) {
        ngx_log_error(NGX_LOG_ALERT, ev->log, ngx_errno,
                      "io_uring_submit(poll_remove) failed");
        return NGX_ERROR;
    }

    ev->active = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_epoll_add_connection(ngx_connection_t *c)
{
    struct epoll_event  ee;

    ee.events = EPOLLIN|EPOLLOUT|EPOLLET|EPOLLRDHUP;
    ee.data.ptr = (void *) ((uintptr_t) c | c->read->instance);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ngx_ring);
    io_uring_prep_poll_add(sqe, c->fd, ee.events);
    sqe->len |= IORING_POLL_ADD_MULTI;
    io_uring_sqe_set_data(sqe, ee.data.ptr);

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "epoll add connection: fd:%d ev:%08XD", c->fd, ee.events);

    if (io_uring_submit(&ngx_ring) < 0) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      "io_uring_submit(poll_add) failed");
        return NGX_ERROR;
    }

    c->read->active = 1;
    c->write->active = 1;

    return NGX_OK;
}


static ngx_int_t
ngx_epoll_del_connection(ngx_connection_t *c, ngx_uint_t flags)
{
    struct epoll_event  ee;

    /*
     * when the file descriptor is closed the epoll automatically deletes
     * it from its queue so we do not need to delete explicitly the event
     * before the closing the file descriptor
     */

    if (flags & NGX_CLOSE_EVENT) {
        c->read->active = 0;
        c->write->active = 0;
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "epoll del connection: fd:%d", c->fd);

    ee.events = 0;
    ee.data.ptr = (void *) ((uintptr_t) c | c->read->instance);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ngx_ring);
    io_uring_prep_poll_remove(sqe, ee.data.ptr);

    if (io_uring_submit(&ngx_ring) < 0) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      "io_uring_submit(poll_remove, %d) failed", c->fd);
        return NGX_ERROR;
    }

    c->read->active = 0;
    c->write->active = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    uint32_t           revents;
    ngx_int_t          instance;
    ngx_uint_t         level;
    ngx_err_t          err;
    ngx_event_t       *rev, *wev;
    ngx_queue_t       *queue;

    /* NGX_TIMER_INFINITE == INFTIM */

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "epoll timer: %M", timer);

    struct io_uring_cqe *cqe;
    int ret = 0;

    if (timer == NGX_TIMER_INFINITE) {
        struct __kernel_timespec ts;
        ts.tv_sec = timer / 1000;
        ts.tv_nsec = (timer - ts.tv_sec * 1000) * 1000;
        ret = io_uring_wait_cqe_timeout(&ngx_ring, &cqe, &ts);
    } else {
        ret = io_uring_wait_cqe(&ngx_ring, &cqe);
    }

    err = (ret < 0) ? ngx_errno : 0;

    if (flags & NGX_UPDATE_TIME || ngx_event_timer_alarm) {
        ngx_time_update();
    }

    if (err) {
        if (err == NGX_EINTR) {

            if (ngx_event_timer_alarm) {
                ngx_event_timer_alarm = 0;
                return NGX_OK;
            }

            level = NGX_LOG_INFO;

        } else {
            level = NGX_LOG_ALERT;
        }

        ngx_log_error(level, cycle->log, err, "io_uring_wait_cqe() failed");
        return NGX_ERROR;
    }

    if (!cqe) {
        if (timer != NGX_TIMER_INFINITE) {
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                      "io_uring_wait_cqe() returned no events without timeout");
        return NGX_ERROR;
    }

    unsigned head;
    unsigned cqe_count = 0;

    io_uring_for_each_cqe(&ngx_ring, head, cqe) {
        ++cqe_count;
        if (!cqe->user_data) continue;

        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                        "io_event: %p %d %d",
                        cqe->user_data, cqe->res, cqe->flags);

            ngx_event_aio_t  *aio;
            ngx_event_t *e = (ngx_event_t *) io_uring_cqe_get_data(cqe);
            e->complete = 1;
            e->active = 0;
            e->ready = 1;

            aio = e->data;
            aio->res = cqe->res;

            ngx_post_event(e, &ngx_posted_events);
        } else {
            ngx_connection_t *c = io_uring_cqe_get_data(cqe);
            instance = (uintptr_t) c & 1;
            c = (ngx_connection_t *) ((uintptr_t) c & (uintptr_t) ~1);

            rev = c->read;

            if (c->fd == -1 || rev->instance != instance) {

                /*
                * the stale event from a file descriptor
                * that was just closed in this iteration
                */

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                            "epoll: stale event %p", c);
                continue;
            }

            revents = cqe->res;

            ngx_log_debug3(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                        "epoll: fd:%d ev:%04XD d:%p",
                        c->fd, revents, c);

            if (revents & (EPOLLERR|EPOLLHUP)) {
                ngx_log_debug2(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                            "io_uring_wait_cqe() error on fd:%d ev:%04XD",
                            c->fd, revents);

                /*
                * if the error events were returned, add EPOLLIN and EPOLLOUT
                * to handle the events at least in one active handler
                */

                revents |= EPOLLIN|EPOLLOUT;
            }

#if 0
            if (revents & ~(EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP)) {
                ngx_log_error(NGX_LOG_ALERT, cycle->log, 0,
                            "strange epoll_wait() events fd:%d ev:%04XD",
                            c->fd, revents);
            }
#endif

            if ((revents & EPOLLIN) && rev->active) {

                if (revents & EPOLLRDHUP) {
                    rev->pending_eof = 1;
                }

                rev->ready = 1;
                rev->available = -1;

                if (flags & NGX_POST_EVENTS) {
                    queue = rev->accept ? &ngx_posted_accept_events
                                        : &ngx_posted_events;

                    ngx_post_event(rev, queue);

                } else {
                    rev->handler(rev);
                }
            }

            wev = c->write;

            if ((revents & EPOLLOUT) && wev->active) {

                if (c->fd == -1 || wev->instance != instance) {

                    /*
                    * the stale event from a file descriptor
                    * that was just closed in this iteration
                    */

                    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                                "epoll: stale event %p", c);
                    continue;
                }

                wev->ready = 1;
#if (NGX_THREADS)
                wev->complete = 1;
#endif

                if (flags & NGX_POST_EVENTS) {
                    ngx_post_event(wev, &ngx_posted_events);

                } else {
                    wev->handler(wev);
                }
            }
        }
    }

    io_uring_cq_advance(&ngx_ring, cqe_count);

    return NGX_OK;
}


static void *
ngx_epoll_create_conf(ngx_cycle_t *cycle)
{
    ngx_epoll_conf_t  *epcf;

    epcf = ngx_palloc(cycle->pool, sizeof(ngx_epoll_conf_t));
    if (epcf == NULL) {
        return NULL;
    }

    return epcf;
}


static char *
ngx_epoll_init_conf(ngx_cycle_t *cycle, void *conf)
{
    return NGX_CONF_OK;
}
