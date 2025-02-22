/*
 ============================================================================
 Name        : hev-socks5-session-udp.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2017 - 2021 hev
 Description : Socks5 Session UDP
 ============================================================================
 */

#include <errno.h>
#include <string.h>

#include <lwip/udp.h>
#include <netinet/in.h>

#include <hev-task.h>
#include <hev-task-io.h>
#include <hev-task-io-socket.h>
#include <hev-task-mutex.h>
#include <hev-memory-allocator.h>
#include <hev-socks5-udp.h>
#include <hev-socks5-misc.h>
#include <hev-socks5-client-udp.h>

#include "hev-logger.h"
#include "hev-compiler.h"
#include "hev-config-const.h"

#include "hev-socks5-session-udp.h"

#define task_io_yielder hev_socks5_task_io_yielder

typedef struct _HevSocks5UDPFrame HevSocks5UDPFrame;

struct _HevSocks5UDPFrame
{
    HevListNode node;
    struct sockaddr_in6 addr;
    struct pbuf *data;
};

static int
hev_socks5_session_udp_fwd_f (HevSocks5SessionUDP *self)
{
    HevSocks5UDPFrame *frame;
    struct sockaddr *addr;
    HevListNode *node;
    HevSocks5UDP *udp;
    struct pbuf *buf;
    int res;

    node = hev_list_first (&self->frame_list);
    if (!node)
        return 0;

    frame = container_of (node, HevSocks5UDPFrame, node);
    udp = HEV_SOCKS5_UDP (self->base.client);
    addr = (struct sockaddr *)&frame->addr;
    buf = frame->data;

    res = hev_socks5_udp_sendto (udp, buf->payload, buf->len, addr);
    if (res <= 0) {
        LOG_E ("%p socks5 session udp fwd f send", self);
        res = -1;
    }

    hev_list_del (&self->frame_list, node);
    hev_free (frame);
    pbuf_free (buf);
    self->frames--;

    return res;
}

static int
hev_socks5_session_udp_fwd_b (HevSocks5SessionUDP *self)
{
    struct sockaddr_in6 ads = { 0 };
    struct sockaddr *saddr;
    err_t err = ERR_OK;
    HevSocks5UDP *udp;
    struct pbuf *buf;
    ip_addr_t addr;
    uint16_t port;
    int res;
    int fd;

    udp = HEV_SOCKS5_UDP (self->base.client);
    fd = HEV_SOCKS5 (udp)->fd;
    if (fd < 0) {
        LOG_E ("%p socks5 session udp fd", self);
        return -1;
    }

    res = recv (fd, &buf, 1, MSG_PEEK);
    if (res <= 0) {
        if ((res < 0) && (errno == EAGAIN))
            return 0;
        LOG_E ("%p socks5 session udp fwd b peek", self);
        return -1;
    }

    saddr = (struct sockaddr *)&ads;
    switch (self->pcb->remote_ip.type) {
    case IPADDR_TYPE_V4:
        saddr->sa_family = AF_INET;
        break;
    case IPADDR_TYPE_V6:
        saddr->sa_family = AF_INET6;
        break;
    }

    hev_task_mutex_lock (self->mutex);
    buf = pbuf_alloc (PBUF_TRANSPORT, UDP_BUF_SIZE, PBUF_RAM);
    hev_task_mutex_unlock (self->mutex);
    if (!buf) {
        LOG_E ("%p socks5 session udp fwd b buf", self);
        return -1;
    }

    res = hev_socks5_udp_recvfrom (udp, buf->payload, buf->len, saddr);
    if (res <= 0) {
        LOG_E ("%p socks5 session udp fwd b recv", self);
        pbuf_free (buf);
        return -1;
    }

    if (saddr->sa_family == AF_INET) {
        struct sockaddr_in *adp;

        adp = (struct sockaddr_in *)saddr;
        addr.type = IPADDR_TYPE_V4;
        port = ntohs (adp->sin_port);
        memcpy (&addr, &adp->sin_addr, 4);
    } else if (saddr->sa_family == AF_INET6) {
        struct sockaddr_in6 *adp;

        adp = (struct sockaddr_in6 *)saddr;
        addr.type = IPADDR_TYPE_V6;
        port = ntohs (adp->sin6_port);
        memcpy (&addr, &adp->sin6_addr, 16);
    } else {
        LOG_E ("%p socks5 session udp fwd b addr", self);
        pbuf_free (buf);
        return -1;
    }

    buf->len = res;
    buf->tot_len = res;

    hev_task_mutex_lock (self->mutex);
    err = udp_sendfrom (self->pcb, buf, &addr, port);
    hev_task_mutex_unlock (self->mutex);

    if (err != ERR_OK) {
        LOG_E ("%p socks5 session udp fwd b send", self);
        pbuf_free (buf);
        return -1;
    }

    return 1;
}

static void
hev_socks5_session_udp_splice (HevSocks5Session *base)
{
    HevSocks5SessionUDP *self = HEV_SOCKS5_SESSION_UDP (base);
    int res_f = 1;
    int res_b = 1;

    LOG_D ("%p socks5 session udp splice", self);

    for (;;) {
        HevTaskYieldType type;

        if (res_f >= 0)
            res_f = hev_socks5_session_udp_fwd_f (self);
        if (res_b >= 0)
            res_b = hev_socks5_session_udp_fwd_b (self);

        if (res_f < 0 || res_b < 0)
            break;
        else if (res_f > 0 || res_b > 0)
            type = HEV_TASK_YIELD;
        else
            type = HEV_TASK_WAITIO;

        if (task_io_yielder (type, base->client) < 0)
            break;
    }
}

static HevSocks5SessionUDPClass _klass = {
    {
        .name = "HevSoscks5SessionUDP",
        .splicer = hev_socks5_session_udp_splice,
        .finalizer = hev_socks5_session_udp_destruct,
    },
};

int
hev_socks5_session_udp_construct (HevSocks5SessionUDP *self)
{
    int res;

    res = hev_socks5_session_construct (&self->base);
    if (res < 0)
        return -1;

    LOG_D ("%p socks5 session udp construct", self);

    HEV_SOCKS5_SESSION (self)->klass = HEV_SOCKS5_SESSION_CLASS (&_klass);

    return 0;
}

void
hev_socks5_session_udp_destruct (HevSocks5Session *base)
{
    HevSocks5SessionUDP *self = HEV_SOCKS5_SESSION_UDP (base);
    HevListNode *node;

    LOG_D ("%p socks5 session udp destruct", self);

    node = hev_list_first (&self->frame_list);
    while (node) {
        HevSocks5UDPFrame *frame;

        frame = container_of (node, HevSocks5UDPFrame, node);
        node = hev_list_node_next (node);
        pbuf_free (frame->data);
        hev_free (frame);
    }

    hev_task_mutex_lock (self->mutex);
    if (self->pcb) {
        udp_recv (self->pcb, NULL, NULL);
        udp_remove (self->pcb);
    }
    hev_task_mutex_unlock (self->mutex);

    hev_socks5_session_destruct (base);
}

static void
udp_recv_handler (void *arg, struct udp_pcb *pcb, struct pbuf *p,
                  const ip_addr_t *addr, u16_t port)
{
    HevSocks5SessionUDP *self = arg;
    HevSocks5Session *s = arg;
    HevSocks5UDPFrame *frame;

    if (!p) {
        hev_socks5_session_terminate (s);
        return;
    }

    if (self->frames > UDP_POOL_SIZE) {
        pbuf_free (p);
        return;
    }

    frame = hev_malloc (sizeof (HevSocks5UDPFrame));
    if (!frame) {
        pbuf_free (p);
        return;
    }

    frame->data = p;
    memset (&frame->node, 0, sizeof (frame->node));

    addr = &pcb->local_ip;
    port = pcb->local_port;

    if (addr->type == IPADDR_TYPE_V4) {
        struct sockaddr_in *adp;

        adp = (struct sockaddr_in *)&frame->addr;
        adp->sin_family = AF_INET;
        adp->sin_port = htons (port);
        memcpy (&adp->sin_addr, addr, 4);
    } else if (addr->type == IPADDR_TYPE_V6) {
        struct sockaddr_in6 *adp;

        adp = (struct sockaddr_in6 *)&frame->addr;
        adp->sin6_family = AF_INET6;
        adp->sin6_port = htons (port);
        memcpy (&adp->sin6_addr, addr, 16);
    }

    self->frames++;
    hev_list_add_tail (&self->frame_list, &frame->node);
    hev_task_wakeup (s->task);
}

HevSocks5SessionUDP *
hev_socks5_session_udp_new (struct udp_pcb *pcb, HevTaskMutex *mutex)
{
    HevSocks5SessionUDP *self = NULL;
    HevSocks5ClientUDP *udp;
    int res;

    self = hev_malloc0 (sizeof (HevSocks5SessionUDP));
    if (!self)
        return NULL;

    LOG_D ("%p socks5 session udp new", self);

    res = hev_socks5_session_udp_construct (self);
    if (res < 0) {
        hev_free (self);
        return NULL;
    }

    udp = hev_socks5_client_udp_new ();
    if (!udp) {
        hev_free (self);
        return NULL;
    }

    udp_recv (pcb, udp_recv_handler, self);

    self->pcb = pcb;
    self->mutex = mutex;
    self->base.client = HEV_SOCKS5_CLIENT (udp);

    return self;
}
