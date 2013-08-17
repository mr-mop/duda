/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Duda I/O
 *  --------
 *  Copyright (C) 2012-2013, Eduardo Silva P. <edsiper@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "duda_event.h"

/*
 * @OBJ_NAME: event
 * @OBJ_MENU: Events
 * @OBJ_DESC: The event object provides a set of methods to handle event-driven sockets
 * in the stack. When Duda I/O is started, it creates a fixed number of worker threads
 * where each one is capable to receive a high number of incoming client connections, each
 * thread have a separated main event loop based in the Linux epoll(7) interface.
 *
 * The methods presented here, allows to register your own file descriptors or sockets into
 * the main event loop for the active worker in question. Once the file descriptor is
 * registered, you are responsible for it deletion. You can define different callbacks
 * for the event desired, when handling events and callbacks, the following modes are
 * available:
 *
 *   DUDA_EVENT_READ: some data is available for a read operation on the socket.
 *
 *   DUDA_EVENT_WRITE: the socket is ready for write operations.
 *
 *   DUDA_EVENT_RW: the socket is ready for read or write operations.
 *
 *   DUDA_EVENT_SLEEP: the socket events are disable, socket in sleep mode.
 *
 *   DUDA_EVENT_WAKEUP: wake up a sleeping socket.
 *
 * Besides the callbacks and handlers, this interface also support notifications. When a
 * worker is created, Duda also creates a notification interface for that main loop event,
 * internally this is done through the Linux eventfd(2) system call. So if you create your
 * own threads and wants to send some notification to the default workers, you can issue it
 * using the method event->signal().
 *
 */

/* Event object / API */
struct duda_api_event *duda_event_object()
{
    struct duda_api_event *e;

    e = mk_api->mem_alloc(sizeof(struct duda_api_event));
    e->add    = duda_event_add;
    e->lookup = duda_event_lookup;
    e->mode   = duda_event_mode;
    e->delete = duda_event_delete;
    e->signal = duda_event_signal;

    return e;
};

/*
 * @METHOD_NAME: add
 * @METHOD_DESC: Register a new socket or file descriptor into the worker event loop and
 * associate proper event handlers or callbacks.
 * @METHOD_PARAM: sockfd socket file descriptor
 * @METHOD_PARAM: dr the request context information hold by a duda_request_t type
 * @METHOD_PARAM: init_mode defines the initial event mode for the file descriptor in question,
 * allowed values are DUDA_EVENT_READ, DUDA_EVENT_WRITE, DUDA_EVENT_RW and
 * DUDA_EVENT_SLEEP.
 * @METHOD_PARAM: behavior defines the events triggered mode to work on. Allowed values are
 * DUDA_EVENT_LEVEL_TRIGGERED OR DUDA_EVENT_EDGE_TRIGGERED. For more details about the behavior
 * refer to the manpage epoll(7).
 * @METHOD_PARAM: cb_on_read callback function for read events or NULL
 * @METHOD_PARAM: cb_on_write callback function for write events or NULL
 * @METHOD_PARAM: cb_on_error callback function for error events or NULL
 * @METHOD_PARAM: cb_on_close callback function for close events or NULL
 * @METHOD_PARAM: cb_on_timeout callback function for timeout events or NULL
 * @METHOD_RETURN: Upon successful completion it returns 0, on error it returns -1
 */
int duda_event_add(int sockfd,
                   int init_mode, int behavior,
                   int (*cb_on_read) (int,  void *),
                   int (*cb_on_write) (int, void *),
                   int (*cb_on_error) (int, void *),
                   int (*cb_on_close) (int, void *),
                   int (*cb_on_timeout) (int, void *),
                   void *data)
{
    struct mk_list *event_list;
    struct duda_event_handler *eh;
    static duda_request_t *dr;

    eh = mk_api->mem_alloc_z(sizeof(struct duda_event_handler));
    if (!eh) {
        return -1;
    }

    /* set node */
    eh->sockfd = sockfd;
    eh->mode = init_mode;
    eh->behavior = behavior;
    eh->cb_on_read = cb_on_read;
    eh->cb_on_write = cb_on_write;
    eh->cb_on_error = cb_on_error;
    eh->cb_on_close = cb_on_close;
    eh->cb_on_timeout = cb_on_timeout;
    eh->cb_data = data;

    /* Link to thread list */
    event_list = pthread_getspecific(duda_events_list);
    mk_list_add(&eh->_head, event_list);

    if (init_mode < DUDA_EVENT_READ || init_mode > DUDA_EVENT_SLEEP) {
        mk_err("Duda: Invalid usage of duda_event_add()");
        exit(EXIT_FAILURE);
    }

    /* Check if the event socket belongs to an active duda_request_t */
    dr = duda_dr_list_get(sockfd);
    if (dr) {
        if (sockfd != dr->socket) {
            mk_api->event_add(sockfd, init_mode, duda_plugin, behavior);
        }
        else {
            mk_api->event_socket_change_mode(sockfd, init_mode, behavior);
        }
    }
    else {
        mk_api->event_add(sockfd, init_mode, duda_plugin, behavior);
    }

    return 0;
}

/*
 * @METHOD_NAME: lookup
 * @METHOD_DESC: When an event is registered through the add method, internally an
 * event handler is created. It stores the references for socket and the callbacks for
 * each type of event. This method allows to find a specific event_handler through the
 * given socket file descriptor.
 * @METHOD_PROTO: struct duda_event_handler *lookup(int socket)
 * @METHOD_PARAM: socket socket file descriptor
 * @METHOD_RETURN: Upon successful completion it returns the event handler node, if the
 * lookup fails it returns NULL
 */
struct duda_event_handler *duda_event_lookup(int sockfd)
{
    struct mk_list *head, *event_list;
    struct duda_event_handler *eh;

    event_list = pthread_getspecific(duda_events_list);
    if (!event_list) {
        return NULL;
    }

    mk_list_foreach(head, event_list) {
        eh = mk_list_entry(head, struct duda_event_handler, _head);
        if (eh->sockfd == sockfd) {
            return eh;
        }
    }

    return NULL;
}

/*
 * @METHOD_NAME: mode
 * @METHOD_DESC: For a given socket file descriptor, alter the event handler mode and behavior.
 * @METHOD_PARAM: sockfd socket file descriptor
 * @METHOD_PARAM: mode defines the new event mode for the file descriptor in question,
 * allowed values are: DUDA_EVENT_READ, DUDA_EVENT_WRITE, DUDA_EVENT_RW, DUDA_EVENT_SLEEP or
 * DUDA_EVENT_WAKEUP.
 * @METHOD_PARAM: behavior defines the events triggered mode to work on. Allowed values are
 * DUDA_EVENT_LEVEL_TRIGGERED OR DUDA_EVENT_EDGE_TRIGGERED. For more details about the behavior
 * refer to the manpage epoll(7).
 * @METHOD_RETURN: Upon successful completion it returns 0, on error it returns -1
 */
int duda_event_mode(int sockfd, int mode, int behavior)
{
    struct duda_event_handler *eh;

    /* We just put to sleep epoll events created through this event object */
    eh = duda_event_lookup(sockfd);
    if (!eh) {
        return -1;
    }

    return mk_api->event_socket_change_mode(sockfd, mode, behavior);
}

/*
 * @METHOD_NAME: delete
 * @METHOD_DESC: Delete a registered event handler from the worker events queue.
 * @METHOD_PARAM: socket socket file descriptor
 * @METHOD_RETURN: Upon successful completion it returns 0, on error it returns -1
 */
int duda_event_delete(int sockfd)
{
    struct mk_list *head, *tmp, *event_list;
    struct duda_event_handler *eh;

    event_list = pthread_getspecific(duda_events_list);
    if (!event_list) {
        return -1;
    }

    mk_list_foreach_safe(head, tmp, event_list) {
        eh = mk_list_entry(head, struct duda_event_handler, _head);
        if (eh->sockfd == sockfd) {
            mk_list_del(&eh->_head);
            mk_api->mem_free(eh);
            return 0;
        }
    }

    return -1;
}

/*
 * @METHOD_NAME: signal
 * @METHOD_DESC: Send a notification signal to each worker thread. Upon receiving this
 * signal on each worker, the defined callback through the function duda_event_set_callback()
 * from duda_main(), will be triggered.
 * @METHOD_PARAM: val an unsigned 64 bits value to be used as a signal type.
 * @METHOD_RETURN: Upon successful completion it returns 0, on error it returns -1
 */
int duda_event_signal(uint64_t val)
{
    struct mk_list *head;
    struct duda_event_signal_channel *esc;

    mk_list_foreach(head, &duda_event_signals_list) {
        esc = mk_list_entry(head, struct duda_event_signal_channel, _head);
        write(esc->fd, &val, sizeof(uint64_t));
    }

    return 0;
}

/*
 * This call aims to be the proxy for notification coming from some
 * signal writer. Once we get here, the next step is to identify which
 * service have defined it callbacks for it. This eventfd function is
 * intended to be used from a server HTTP worker context. It can be used
 * to wake up some pending HTTP response sleeping connection.
 */
int duda_event_fd_read(int fd, void *data)
{
    (void) data;
    ssize_t s;
    uint64_t val;
    struct mk_list *head;
    struct web_service *ws;

    /* read the value */
    s = read(fd, &val, sizeof(uint64_t));
    if (s != sizeof(uint64_t)) {
        msg->warn("Could not read signal");
        return -1;
    }

    /* For all our web services, search and invoke the callback */
    mk_list_foreach(head, &services_loaded) {
        ws = mk_list_entry(head, struct web_service, _head_loaded);
        if (ws->setup->event_signal_cb) {
            ws->setup->event_signal_cb(fd, val);
        }
    }

    return DUDA_EVENT_OWNED;
}

/*
 * @METHOD_NAME: duda_event_signal_set_callback
 * @METHOD_DESC: Define a callback function inside the web service to be triggered
 * for when a signal is emited through the signal() method. This is a function that must
 * be called inside duda_main().
 * @METHOD_PROTO: void duda_event_set_signal_callback(void (*func) (int, uint64_t))
 * @METHOD_PARAM: func the function reference, an example of this function definition
 * is as follows:
 *
 * void my_signal_callback(int fd, uint64_t value) {
 * ...
 * }
 *
 * then from duda_main it can be set as:
 *
 * duda_event_set_signal_set_callback(my_signal_callback);
 * @METHOD_RETURN: This function do not return any value.
 */

/* this is a static function defined in duda_event.h */
