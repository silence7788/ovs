/*
 * Copyright (c) 2008, 2009, 2010 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "rconn.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "coverage.h"
#include "ofpbuf.h"
#include "openflow/openflow.h"
#include "poll-loop.h"
#include "sat-math.h"
#include "timeval.h"
#include "util.h"
#include "vconn.h"

#define THIS_MODULE VLM_rconn
#include "vlog.h"

#define STATES                                  \
    STATE(VOID, 1 << 0)                         \
    STATE(BACKOFF, 1 << 1)                      \
    STATE(CONNECTING, 1 << 2)                   \
    STATE(ACTIVE, 1 << 3)                       \
    STATE(IDLE, 1 << 4)
enum state {
#define STATE(NAME, VALUE) S_##NAME = VALUE,
    STATES
#undef STATE
};

static const char *
state_name(enum state state)
{
    switch (state) {
#define STATE(NAME, VALUE) case S_##NAME: return #NAME;
        STATES
#undef STATE
    }
    return "***ERROR***";
}

/* A reliable connection to an OpenFlow switch or controller.
 *
 * See the large comment in rconn.h for more information. */
struct rconn {
    enum state state;
    time_t state_entered;

    struct vconn *vconn;
    char *name;
    bool reliable;

    struct ovs_queue txq;

    int backoff;
    int max_backoff;
    time_t backoff_deadline;
    time_t last_received;
    time_t last_connected;
    unsigned int packets_sent;
    unsigned int seqno;

    /* In S_ACTIVE and S_IDLE, probably_admitted reports whether we believe
     * that the peer has made a (positive) admission control decision on our
     * connection.  If we have not yet been (probably) admitted, then the
     * connection does not reset the timer used for deciding whether the switch
     * should go into fail-open mode.
     *
     * last_admitted reports the last time we believe such a positive admission
     * control decision was made. */
    bool probably_admitted;
    time_t last_admitted;

    /* These values are simply for statistics reporting, not used directly by
     * anything internal to the rconn (or ofproto for that matter). */
    unsigned int packets_received;
    unsigned int n_attempted_connections, n_successful_connections;
    time_t creation_time;
    unsigned long int total_time_connected;

    /* If we can't connect to the peer, it could be for any number of reasons.
     * Usually, one would assume it is because the peer is not running or
     * because the network is partitioned.  But it could also be because the
     * network topology has changed, in which case the upper layer will need to
     * reassess it (in particular, obtain a new IP address via DHCP and find
     * the new location of the controller).  We set this flag when we suspect
     * that this could be the case. */
    bool questionable_connectivity;
    time_t last_questioned;

    /* Throughout this file, "probe" is shorthand for "inactivity probe".
     * When nothing has been received from the peer for a while, we send out
     * an echo request as an inactivity probe packet.  We should receive back
     * a response. */
    int probe_interval;         /* Secs of inactivity before sending probe. */

    /* When we create a vconn we obtain these values, to save them past the end
     * of the vconn's lifetime.  Otherwise, in-band control will only allow
     * traffic when a vconn is actually open, but it is nice to allow ARP to
     * complete even between connection attempts, and it is also polite to
     * allow traffic from other switches to go through to the controller
     * whether or not we are connected.
     *
     * We don't cache the local port, because that changes from one connection
     * attempt to the next. */
    uint32_t local_ip, remote_ip;
    uint16_t remote_port;

    /* Messages sent or received are copied to the monitor connections. */
#define MAX_MONITORS 8
    struct vconn *monitors[8];
    size_t n_monitors;
};

static unsigned int elapsed_in_this_state(const struct rconn *);
static unsigned int timeout(const struct rconn *);
static bool timed_out(const struct rconn *);
static void state_transition(struct rconn *, enum state);
static void set_vconn_name(struct rconn *, const char *name);
static int try_send(struct rconn *);
static int reconnect(struct rconn *);
static void report_error(struct rconn *, int error);
static void disconnect(struct rconn *, int error);
static void flush_queue(struct rconn *);
static void question_connectivity(struct rconn *);
static void copy_to_monitor(struct rconn *, const struct ofpbuf *);
static bool is_connected_state(enum state);
static bool is_admitted_msg(const struct ofpbuf *);

/* Creates a new rconn, connects it (reliably) to 'name', and returns it. */
struct rconn *
rconn_new(const char *name, int inactivity_probe_interval, int max_backoff)
{
    struct rconn *rc = rconn_create(inactivity_probe_interval, max_backoff);
    rconn_connect(rc, name);
    return rc;
}

/* Creates a new rconn, connects it (unreliably) to 'vconn', and returns it. */
struct rconn *
rconn_new_from_vconn(const char *name, struct vconn *vconn) 
{
    struct rconn *rc = rconn_create(60, 0);
    rconn_connect_unreliably(rc, name, vconn);
    return rc;
}

/* Creates and returns a new rconn.
 *
 * 'probe_interval' is a number of seconds.  If the interval passes once
 * without an OpenFlow message being received from the peer, the rconn sends
 * out an "echo request" message.  If the interval passes again without a
 * message being received, the rconn disconnects and re-connects to the peer.
 * Setting 'probe_interval' to 0 disables this behavior.
 *
 * 'max_backoff' is the maximum number of seconds between attempts to connect
 * to the peer.  The actual interval starts at 1 second and doubles on each
 * failure until it reaches 'max_backoff'.  If 0 is specified, the default of
 * 8 seconds is used. */
struct rconn *
rconn_create(int probe_interval, int max_backoff)
{
    struct rconn *rc = xcalloc(1, sizeof *rc);

    rc->state = S_VOID;
    rc->state_entered = time_now();

    rc->vconn = NULL;
    rc->name = xstrdup("void");
    rc->reliable = false;

    queue_init(&rc->txq);

    rc->backoff = 0;
    rc->max_backoff = max_backoff ? max_backoff : 8;
    rc->backoff_deadline = TIME_MIN;
    rc->last_received = time_now();
    rc->last_connected = time_now();
    rc->seqno = 0;

    rc->packets_sent = 0;

    rc->probably_admitted = false;
    rc->last_admitted = time_now();

    rc->packets_received = 0;
    rc->n_attempted_connections = 0;
    rc->n_successful_connections = 0;
    rc->creation_time = time_now();
    rc->total_time_connected = 0;

    rc->questionable_connectivity = false;
    rc->last_questioned = time_now();

    rconn_set_probe_interval(rc, probe_interval);

    rc->n_monitors = 0;

    return rc;
}

void
rconn_set_max_backoff(struct rconn *rc, int max_backoff)
{
    rc->max_backoff = MAX(1, max_backoff);
    if (rc->state == S_BACKOFF && rc->backoff > max_backoff) {
        rc->backoff = max_backoff;
        if (rc->backoff_deadline > time_now() + max_backoff) {
            rc->backoff_deadline = time_now() + max_backoff;
        }
    }
}

int
rconn_get_max_backoff(const struct rconn *rc)
{
    return rc->max_backoff;
}

void
rconn_set_probe_interval(struct rconn *rc, int probe_interval)
{
    rc->probe_interval = probe_interval ? MAX(5, probe_interval) : 0;
}

int
rconn_get_probe_interval(const struct rconn *rc)
{
    return rc->probe_interval;
}

int
rconn_connect(struct rconn *rc, const char *name)
{
    rconn_disconnect(rc);
    set_vconn_name(rc, name);
    rc->reliable = true;
    return reconnect(rc);
}

void
rconn_connect_unreliably(struct rconn *rc,
                         const char *name, struct vconn *vconn)
{
    assert(vconn != NULL);
    rconn_disconnect(rc);
    set_vconn_name(rc, name);
    rc->reliable = false;
    rc->vconn = vconn;
    rc->last_connected = time_now();
    state_transition(rc, S_ACTIVE);
}

/* If 'rc' is connected, forces it to drop the connection and reconnect. */
void
rconn_reconnect(struct rconn *rc)
{
    if (rc->state & (S_ACTIVE | S_IDLE)) {
        VLOG_INFO("%s: disconnecting", rc->name);
        disconnect(rc, 0);
    }
}

void
rconn_disconnect(struct rconn *rc)
{
    if (rc->state != S_VOID) {
        if (rc->vconn) {
            vconn_close(rc->vconn);
            rc->vconn = NULL;
        }
        set_vconn_name(rc, "void");
        rc->reliable = false;

        rc->backoff = 0;
        rc->backoff_deadline = TIME_MIN;

        state_transition(rc, S_VOID);
    }
}

/* Disconnects 'rc' and frees the underlying storage. */
void
rconn_destroy(struct rconn *rc)
{
    if (rc) {
        size_t i;

        free(rc->name);
        vconn_close(rc->vconn);
        flush_queue(rc);
        queue_destroy(&rc->txq);
        for (i = 0; i < rc->n_monitors; i++) {
            vconn_close(rc->monitors[i]);
        }
        free(rc);
    }
}

static unsigned int
timeout_VOID(const struct rconn *rc OVS_UNUSED)
{
    return UINT_MAX;
}

static void
run_VOID(struct rconn *rc OVS_UNUSED)
{
    /* Nothing to do. */
}

static int
reconnect(struct rconn *rc)
{
    int retval;

    VLOG_INFO("%s: connecting...", rc->name);
    rc->n_attempted_connections++;
    retval = vconn_open(rc->name, OFP_VERSION, &rc->vconn);
    if (!retval) {
        rc->remote_ip = vconn_get_remote_ip(rc->vconn);
        rc->local_ip = vconn_get_local_ip(rc->vconn);
        rc->remote_port = vconn_get_remote_port(rc->vconn);
        rc->backoff_deadline = time_now() + rc->backoff;
        state_transition(rc, S_CONNECTING);
    } else {
        VLOG_WARN("%s: connection failed (%s)", rc->name, strerror(retval));
        rc->backoff_deadline = TIME_MAX; /* Prevent resetting backoff. */
        disconnect(rc, 0);
    }
    return retval;
}

static unsigned int
timeout_BACKOFF(const struct rconn *rc)
{
    return rc->backoff;
}

static void
run_BACKOFF(struct rconn *rc)
{
    if (timed_out(rc)) {
        reconnect(rc);
    }
}

static unsigned int
timeout_CONNECTING(const struct rconn *rc)
{
    return MAX(1, rc->backoff);
}

static void
run_CONNECTING(struct rconn *rc)
{
    int retval = vconn_connect(rc->vconn);
    if (!retval) {
        VLOG_INFO("%s: connected", rc->name);
        rc->n_successful_connections++;
        state_transition(rc, S_ACTIVE);
        rc->last_connected = rc->state_entered;
    } else if (retval != EAGAIN) {
        VLOG_INFO("%s: connection failed (%s)", rc->name, strerror(retval));
        disconnect(rc, retval);
    } else if (timed_out(rc)) {
        VLOG_INFO("%s: connection timed out", rc->name);
        rc->backoff_deadline = TIME_MAX; /* Prevent resetting backoff. */
        disconnect(rc, 0);
    }
}

static void
do_tx_work(struct rconn *rc)
{
    if (!rc->txq.n) {
        return;
    }
    while (rc->txq.n > 0) {
        int error = try_send(rc);
        if (error) {
            break;
        }
    }
    if (!rc->txq.n) {
        poll_immediate_wake();
    }
}

static unsigned int
timeout_ACTIVE(const struct rconn *rc)
{
    if (rc->probe_interval) {
        unsigned int base = MAX(rc->last_received, rc->state_entered);
        unsigned int arg = base + rc->probe_interval - rc->state_entered;
        return arg;
    }
    return UINT_MAX;
}

static void
run_ACTIVE(struct rconn *rc)
{
    if (timed_out(rc)) {
        unsigned int base = MAX(rc->last_received, rc->state_entered);
        VLOG_DBG("%s: idle %u seconds, sending inactivity probe",
                 rc->name, (unsigned int) (time_now() - base));

        /* Ordering is important here: rconn_send() can transition to BACKOFF,
         * and we don't want to transition back to IDLE if so, because then we
         * can end up queuing a packet with vconn == NULL and then *boom*. */
        state_transition(rc, S_IDLE);
        rconn_send(rc, make_echo_request(), NULL);
        return;
    }

    do_tx_work(rc);
}

static unsigned int
timeout_IDLE(const struct rconn *rc)
{
    return rc->probe_interval;
}

static void
run_IDLE(struct rconn *rc)
{
    if (timed_out(rc)) {
        question_connectivity(rc);
        VLOG_ERR("%s: no response to inactivity probe after %u "
                 "seconds, disconnecting",
                 rc->name, elapsed_in_this_state(rc));
        disconnect(rc, 0);
    } else {
        do_tx_work(rc);
    }
}

/* Performs whatever activities are necessary to maintain 'rc': if 'rc' is
 * disconnected, attempts to (re)connect, backing off as necessary; if 'rc' is
 * connected, attempts to send packets in the send queue, if any. */
void
rconn_run(struct rconn *rc)
{
    int old_state;
    do {
        old_state = rc->state;
        switch (rc->state) {
#define STATE(NAME, VALUE) case S_##NAME: run_##NAME(rc); break;
            STATES
#undef STATE
        default:
            NOT_REACHED();
        }
    } while (rc->state != old_state);
}

/* Causes the next call to poll_block() to wake up when rconn_run() should be
 * called on 'rc'. */
void
rconn_run_wait(struct rconn *rc)
{
    unsigned int timeo = timeout(rc);
    if (timeo != UINT_MAX) {
        unsigned int expires = sat_add(rc->state_entered, timeo);
        unsigned int remaining = sat_sub(expires, time_now());
        poll_timer_wait(sat_mul(remaining, 1000));
    }

    if ((rc->state & (S_ACTIVE | S_IDLE)) && rc->txq.n) {
        vconn_wait(rc->vconn, WAIT_SEND);
    }
}

/* Attempts to receive a packet from 'rc'.  If successful, returns the packet;
 * otherwise, returns a null pointer.  The caller is responsible for freeing
 * the packet (with ofpbuf_delete()). */
struct ofpbuf *
rconn_recv(struct rconn *rc)
{
    if (rc->state & (S_ACTIVE | S_IDLE)) {
        struct ofpbuf *buffer;
        int error = vconn_recv(rc->vconn, &buffer);
        if (!error) {
            copy_to_monitor(rc, buffer);
            if (rc->probably_admitted || is_admitted_msg(buffer)
                || time_now() - rc->last_connected >= 30) {
                rc->probably_admitted = true;
                rc->last_admitted = time_now();
            }
            rc->last_received = time_now();
            rc->packets_received++;
            if (rc->state == S_IDLE) {
                state_transition(rc, S_ACTIVE);
            }
            return buffer;
        } else if (error != EAGAIN) {
            report_error(rc, error);
            disconnect(rc, error);
        }
    }
    return NULL;
}

/* Causes the next call to poll_block() to wake up when a packet may be ready
 * to be received by vconn_recv() on 'rc'.  */
void
rconn_recv_wait(struct rconn *rc)
{
    if (rc->vconn) {
        vconn_wait(rc->vconn, WAIT_RECV);
    }
}

/* Sends 'b' on 'rc'.  Returns 0 if successful (in which case 'b' is
 * destroyed), or ENOTCONN if 'rc' is not currently connected (in which case
 * the caller retains ownership of 'b').
 *
 * If 'counter' is non-null, then 'counter' will be incremented while the
 * packet is in flight, then decremented when it has been sent (or discarded
 * due to disconnection).  Because 'b' may be sent (or discarded) before this
 * function returns, the caller may not be able to observe any change in
 * 'counter'.
 *
 * There is no rconn_send_wait() function: an rconn has a send queue that it
 * takes care of sending if you call rconn_run(), which will have the side
 * effect of waking up poll_block(). */
int
rconn_send(struct rconn *rc, struct ofpbuf *b,
           struct rconn_packet_counter *counter)
{
    if (rconn_is_connected(rc)) {
        COVERAGE_INC(rconn_queued);
        copy_to_monitor(rc, b);
        b->private_p = counter;
        if (counter) {
            rconn_packet_counter_inc(counter);
        }
        queue_push_tail(&rc->txq, b);

        /* If the queue was empty before we added 'b', try to send some
         * packets.  (But if the queue had packets in it, it's because the
         * vconn is backlogged and there's no point in stuffing more into it
         * now.  We'll get back to that in rconn_run().) */
        if (rc->txq.n == 1) {
            try_send(rc);
        }
        return 0;
    } else {
        return ENOTCONN;
    }
}

/* Sends 'b' on 'rc'.  Increments 'counter' while the packet is in flight; it
 * will be decremented when it has been sent (or discarded due to
 * disconnection).  Returns 0 if successful, EAGAIN if 'counter->n' is already
 * at least as large as 'queue_limit', or ENOTCONN if 'rc' is not currently
 * connected.  Regardless of return value, 'b' is destroyed.
 *
 * Because 'b' may be sent (or discarded) before this function returns, the
 * caller may not be able to observe any change in 'counter'.
 *
 * There is no rconn_send_wait() function: an rconn has a send queue that it
 * takes care of sending if you call rconn_run(), which will have the side
 * effect of waking up poll_block(). */
int
rconn_send_with_limit(struct rconn *rc, struct ofpbuf *b,
                      struct rconn_packet_counter *counter, int queue_limit)
{
    int retval;
    retval = counter->n >= queue_limit ? EAGAIN : rconn_send(rc, b, counter);
    if (retval) {
        COVERAGE_INC(rconn_overflow);
        ofpbuf_delete(b);
    }
    return retval;
}

/* Returns the total number of packets successfully sent on the underlying
 * vconn.  A packet is not counted as sent while it is still queued in the
 * rconn, only when it has been successfuly passed to the vconn.  */
unsigned int
rconn_packets_sent(const struct rconn *rc)
{
    return rc->packets_sent;
}

/* Adds 'vconn' to 'rc' as a monitoring connection, to which all messages sent
 * and received on 'rconn' will be copied.  'rc' takes ownership of 'vconn'. */
void
rconn_add_monitor(struct rconn *rc, struct vconn *vconn)
{
    if (rc->n_monitors < ARRAY_SIZE(rc->monitors)) {
        VLOG_INFO("new monitor connection from %s", vconn_get_name(vconn));
        rc->monitors[rc->n_monitors++] = vconn;
    } else {
        VLOG_DBG("too many monitor connections, discarding %s",
                 vconn_get_name(vconn));
        vconn_close(vconn);
    }
}

/* Returns 'rc''s name (the 'name' argument passed to rconn_new()). */
const char *
rconn_get_name(const struct rconn *rc)
{
    return rc->name;
}

/* Returns true if 'rconn' is connected or in the process of reconnecting,
 * false if 'rconn' is disconnected and will not reconnect on its own. */
bool
rconn_is_alive(const struct rconn *rconn)
{
    return rconn->state != S_VOID;
}

/* Returns true if 'rconn' is connected, false otherwise. */
bool
rconn_is_connected(const struct rconn *rconn)
{
    return is_connected_state(rconn->state);
}

/* Returns true if 'rconn' is connected and thought to have been accepted by
 * the peer's admission-control policy. */
bool
rconn_is_admitted(const struct rconn *rconn)
{
    return (rconn_is_connected(rconn)
            && rconn->last_admitted >= rconn->last_connected);
}

/* Returns 0 if 'rconn' is currently connected and considered to have been
 * accepted by the peer's admission-control policy, otherwise the number of
 * seconds since 'rconn' was last in such a state. */
int
rconn_failure_duration(const struct rconn *rconn)
{
    return rconn_is_admitted(rconn) ? 0 : time_now() - rconn->last_admitted;
}

/* Returns the IP address of the peer, or 0 if the peer's IP address is not
 * known. */
uint32_t
rconn_get_remote_ip(const struct rconn *rconn) 
{
    return rconn->remote_ip;
}

/* Returns the transport port of the peer, or 0 if the peer's port is not
 * known. */
uint16_t
rconn_get_remote_port(const struct rconn *rconn) 
{
    return rconn->remote_port;
}

/* Returns the IP address used to connect to the peer, or 0 if the
 * connection is not an IP-based protocol or if its IP address is not 
 * known. */
uint32_t
rconn_get_local_ip(const struct rconn *rconn) 
{
    return rconn->local_ip;
}

/* Returns the transport port used to connect to the peer, or 0 if the
 * connection does not contain a port or if the port is not known. */
uint16_t
rconn_get_local_port(const struct rconn *rconn) 
{
    return rconn->vconn ? vconn_get_local_port(rconn->vconn) : 0;
}

/* If 'rconn' can't connect to the peer, it could be for any number of reasons.
 * Usually, one would assume it is because the peer is not running or because
 * the network is partitioned.  But it could also be because the network
 * topology has changed, in which case the upper layer will need to reassess it
 * (in particular, obtain a new IP address via DHCP and find the new location
 * of the controller).  When this appears that this might be the case, this
 * function returns true.  It also clears the questionability flag and prevents
 * it from being set again for some time. */
bool
rconn_is_connectivity_questionable(struct rconn *rconn)
{
    bool questionable = rconn->questionable_connectivity;
    rconn->questionable_connectivity = false;
    return questionable;
}

/* Returns the total number of packets successfully received by the underlying
 * vconn.  */
unsigned int
rconn_packets_received(const struct rconn *rc)
{
    return rc->packets_received;
}

/* Returns a string representing the internal state of 'rc'.  The caller must
 * not modify or free the string. */
const char *
rconn_get_state(const struct rconn *rc)
{
    return state_name(rc->state);
}

/* Returns the number of connection attempts made by 'rc', including any
 * ongoing attempt that has not yet succeeded or failed. */
unsigned int
rconn_get_attempted_connections(const struct rconn *rc)
{
    return rc->n_attempted_connections;
}

/* Returns the number of successful connection attempts made by 'rc'. */
unsigned int
rconn_get_successful_connections(const struct rconn *rc)
{
    return rc->n_successful_connections;
}

/* Returns the time at which the last successful connection was made by
 * 'rc'. */
time_t
rconn_get_last_connection(const struct rconn *rc)
{
    return rc->last_connected;
}

/* Returns the time at which the last OpenFlow message was received by 'rc'.
 * If no packets have been received on 'rc', returns the time at which 'rc'
 * was created. */
time_t
rconn_get_last_received(const struct rconn *rc)
{
    return rc->last_received;
}

/* Returns the time at which 'rc' was created. */
time_t
rconn_get_creation_time(const struct rconn *rc)
{
    return rc->creation_time;
}

/* Returns the approximate number of seconds that 'rc' has been connected. */
unsigned long int
rconn_get_total_time_connected(const struct rconn *rc)
{
    return (rc->total_time_connected
            + (rconn_is_connected(rc) ? elapsed_in_this_state(rc) : 0));
}

/* Returns the current amount of backoff, in seconds.  This is the amount of
 * time after which the rconn will transition from BACKOFF to CONNECTING. */
int
rconn_get_backoff(const struct rconn *rc)
{
    return rc->backoff;
}

/* Returns the number of seconds spent in this state so far. */
unsigned int
rconn_get_state_elapsed(const struct rconn *rc)
{
    return elapsed_in_this_state(rc);
}

/* Returns 'rc''s current connection sequence number, a number that changes
 * every time that 'rconn' connects or disconnects. */
unsigned int
rconn_get_connection_seqno(const struct rconn *rc)
{
    return rc->seqno;
}

struct rconn_packet_counter *
rconn_packet_counter_create(void)
{
    struct rconn_packet_counter *c = xmalloc(sizeof *c);
    c->n = 0;
    c->ref_cnt = 1;
    return c;
}

void
rconn_packet_counter_destroy(struct rconn_packet_counter *c)
{
    if (c) {
        assert(c->ref_cnt > 0);
        if (!--c->ref_cnt && !c->n) {
            free(c);
        }
    }
}

void
rconn_packet_counter_inc(struct rconn_packet_counter *c)
{
    c->n++;
}

void
rconn_packet_counter_dec(struct rconn_packet_counter *c)
{
    assert(c->n > 0);
    if (!--c->n && !c->ref_cnt) {
        free(c);
    }
}

/* Set the name of the remote vconn to 'name' and clear out the cached IP
 * address and port information, since changing the name also likely changes
 * these values. */
static void
set_vconn_name(struct rconn *rc, const char *name)
{
    free(rc->name);
    rc->name = xstrdup(name);
    rc->local_ip = 0;
    rc->remote_ip = 0;
    rc->remote_port = 0;
}

/* Tries to send a packet from 'rc''s send buffer.  Returns 0 if successful,
 * otherwise a positive errno value. */
static int
try_send(struct rconn *rc)
{
    int retval = 0;
    struct ofpbuf *next = rc->txq.head->next;
    struct rconn_packet_counter *counter = rc->txq.head->private_p;
    retval = vconn_send(rc->vconn, rc->txq.head);
    if (retval) {
        if (retval != EAGAIN) {
            report_error(rc, retval);
            disconnect(rc, retval);
        }
        return retval;
    }
    COVERAGE_INC(rconn_sent);
    rc->packets_sent++;
    if (counter) {
        rconn_packet_counter_dec(counter);
    }
    queue_advance_head(&rc->txq, next);
    return 0;
}

/* Reports that 'error' caused 'rc' to disconnect.  'error' may be a positive
 * errno value, or it may be EOF to indicate that the connection was closed
 * normally. */
static void
report_error(struct rconn *rc, int error)
{
    if (error == EOF) {
        /* If 'rc' isn't reliable, then we don't really expect this connection
         * to last forever anyway (probably it's a connection that we received
         * via accept()), so use DBG level to avoid cluttering the logs. */
        enum vlog_level level = rc->reliable ? VLL_INFO : VLL_DBG;
        VLOG(level, "%s: connection closed by peer", rc->name);
    } else {
        VLOG_WARN("%s: connection dropped (%s)", rc->name, strerror(error));
    }
}

/* Disconnects 'rc'. */
static void
disconnect(struct rconn *rc, int error OVS_UNUSED)
{
    if (rc->reliable) {
        time_t now = time_now();

        if (rc->state & (S_CONNECTING | S_ACTIVE | S_IDLE)) {
            vconn_close(rc->vconn);
            rc->vconn = NULL;
            flush_queue(rc);
        }

        if (now >= rc->backoff_deadline) {
            rc->backoff = 1;
        } else {
            rc->backoff = MIN(rc->max_backoff, MAX(1, 2 * rc->backoff));
            VLOG_INFO("%s: waiting %d seconds before reconnect\n",
                      rc->name, rc->backoff);
        }
        rc->backoff_deadline = now + rc->backoff;
        state_transition(rc, S_BACKOFF);
        if (now - rc->last_connected > 60) {
            question_connectivity(rc);
        }
    } else {
        rconn_disconnect(rc);
    }
}

/* Drops all the packets from 'rc''s send queue and decrements their queue
 * counts. */
static void
flush_queue(struct rconn *rc)
{
    if (!rc->txq.n) {
        return;
    }
    while (rc->txq.n > 0) {
        struct ofpbuf *b = queue_pop_head(&rc->txq);
        struct rconn_packet_counter *counter = b->private_p;
        if (counter) {
            rconn_packet_counter_dec(counter);
        }
        COVERAGE_INC(rconn_discarded);
        ofpbuf_delete(b);
    }
    poll_immediate_wake();
}

static unsigned int
elapsed_in_this_state(const struct rconn *rc)
{
    return time_now() - rc->state_entered;
}

static unsigned int
timeout(const struct rconn *rc)
{
    switch (rc->state) {
#define STATE(NAME, VALUE) case S_##NAME: return timeout_##NAME(rc);
        STATES
#undef STATE
    default:
        NOT_REACHED();
    }
}

static bool
timed_out(const struct rconn *rc)
{
    return time_now() >= sat_add(rc->state_entered, timeout(rc));
}

static void
state_transition(struct rconn *rc, enum state state)
{
    rc->seqno += (rc->state == S_ACTIVE) != (state == S_ACTIVE);
    if (is_connected_state(state) && !is_connected_state(rc->state)) {
        rc->probably_admitted = false;
    }
    if (rconn_is_connected(rc)) {
        rc->total_time_connected += elapsed_in_this_state(rc);
    }
    VLOG_DBG("%s: entering %s", rc->name, state_name(state));
    rc->state = state;
    rc->state_entered = time_now();
}

static void
question_connectivity(struct rconn *rc) 
{
    time_t now = time_now();
    if (now - rc->last_questioned > 60) {
        rc->questionable_connectivity = true;
        rc->last_questioned = now;
    }
}

static void
copy_to_monitor(struct rconn *rc, const struct ofpbuf *b)
{
    struct ofpbuf *clone = NULL;
    int retval;
    size_t i;

    for (i = 0; i < rc->n_monitors; ) {
        struct vconn *vconn = rc->monitors[i];

        if (!clone) {
            clone = ofpbuf_clone(b);
        }
        retval = vconn_send(vconn, clone);
        if (!retval) {
            clone = NULL;
        } else if (retval != EAGAIN) {
            VLOG_DBG("%s: closing monitor connection to %s: %s",
                     rconn_get_name(rc), vconn_get_name(vconn),
                     strerror(retval));
            rc->monitors[i] = rc->monitors[--rc->n_monitors];
            continue;
        }
        i++;
    }
    ofpbuf_delete(clone);
}

static bool
is_connected_state(enum state state) 
{
    return (state & (S_ACTIVE | S_IDLE)) != 0;
}

static bool
is_admitted_msg(const struct ofpbuf *b)
{
    struct ofp_header *oh = b->data;
    uint8_t type = oh->type;
    return !(type < 32
             && (1u << type) & ((1u << OFPT_HELLO) |
                                (1u << OFPT_ERROR) |
                                (1u << OFPT_ECHO_REQUEST) |
                                (1u << OFPT_ECHO_REPLY) |
                                (1u << OFPT_VENDOR) |
                                (1u << OFPT_FEATURES_REQUEST) |
                                (1u << OFPT_FEATURES_REPLY) |
                                (1u << OFPT_GET_CONFIG_REQUEST) |
                                (1u << OFPT_GET_CONFIG_REPLY) |
                                (1u << OFPT_SET_CONFIG)));
}
