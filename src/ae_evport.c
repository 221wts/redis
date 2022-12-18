/* ae.c module for illumos event ports.
 *
 * Copyright (c) 2012, Joyent, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
// 用于illumos事件端口的模块。

#include <assert.h>
#include <errno.h>
#include <port.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/time.h>

#include <stdio.h>

static int evport_debug = 0;

/*
 * This file implements the ae API using event ports, present on Solaris-based
 * systems since Solaris 10.  Using the event port interface, we associate file
 * descriptors with the port.  Each association also includes the set of poll(2)
 * events that the consumer is interested in (e.g., POLLIN and POLLOUT).
 *
 * There's one tricky piece to this implementation: when we return events via
 * aeApiPoll, the corresponding file descriptors become dissociated from the
 * port.  This is necessary because poll events are level-triggered, so if the
 * fd didn't become dissociated, it would immediately fire another event since
 * the underlying state hasn't changed yet.  We must re-associate the file
 * descriptor, but only after we know that our caller has actually read from it.
 * The ae API does not tell us exactly when that happens, but we do know that
 * it must happen by the time aeApiPoll is called again.  Our solution is to
 * keep track of the last fds returned by aeApiPoll and re-associate them next
 * time aeApiPoll is invoked.
 *
 * To summarize, in this module, each fd association is EITHER (a) represented
 * only via the in-kernel association OR (b) represented by pending_fds and
 * pending_masks.  (b) is only true for the last fds we returned from aeApiPoll,
 * and only until we enter aeApiPoll again (at which point we restore the
 * in-kernel association).
 */
/* 这个文件使用事件端口实现ae API，从Solaris 10开始就出现在基于Solaris的系统上。
 * 使用事件端口接口的时候，我们将文件描述符与端口关联起来。每个关联还包括消费者感兴趣的poll(2)事件集(例如POLLIN和POLLOUT)。
 *
 * 关于这个的实现，有一个技巧：当你通过aeApiPoll返回事件，相应的文件描述符与端口分离。因为poll事件是水平触发的，
 * 所以fd没有进行分离，他会马上触发一个新的事项（因为状态还没有被改变）。只有在我们知道我们的调用者已经从中读取内容后，才能重新进行文件描述符关联。
 * ae API并没有实际上告诉我们发生了什么，但是我们可以确定在再次调用aeApiPoll的时候会发生。
 * 我们的解决方案是保持对aeApiPoll最后返回的fds并在aeApiPoll触发的时候再次进行关联。
 *
 * 总结一下，在这个模块里面，每个fd关联：
 * 1. 仅通过内核关联表示
 * 2. 由pending_fds和pending_masks表示
 * 后者仅对我们从aeApiPoll返回的最后一个fds为真，并且仅在我们再次进入aeApiPoll之前。
 * 再次进入aeApiPoll之前的时候，我们会恢复内核关联。
 */
#define MAX_EVENT_BATCHSZ 512

typedef struct aeApiState {
    int     portfd;                             /* event port */
    int     npending;                           /* # of pending fds */
    int     pending_fds[MAX_EVENT_BATCHSZ];     /* pending fds */
    int     pending_masks[MAX_EVENT_BATCHSZ];   /* pending fds' masks */
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop) {
    int i;
    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;

    state->portfd = port_create();
    if (state->portfd == -1) {
        zfree(state);
        return -1;
    }

    state->npending = 0;

    for (i = 0; i < MAX_EVENT_BATCHSZ; i++) {
        state->pending_fds[i] = -1;
        state->pending_masks[i] = AE_NONE;
    }

    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    /* Nothing to resize here. */
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->portfd);
    zfree(state);
}

static int aeApiLookupPending(aeApiState *state, int fd) {
    int i;

    for (i = 0; i < state->npending; i++) {
        if (state->pending_fds[i] == fd)
            return (i);
    }

    return (-1);
}

/*
 * Helper function to invoke port_associate for the given fd and mask.
 */
// 为给定的fd和mask调用port_assocaite的帮助函数
static int aeApiAssociate(const char *where, int portfd, int fd, int mask) {
    int events = 0;
    int rv, err;

    if (mask & AE_READABLE)
        events |= POLLIN;
    if (mask & AE_WRITABLE)
        events |= POLLOUT;

    if (evport_debug)
        fprintf(stderr, "%s: port_associate(%d, 0x%x) = ", where, fd, events);

    rv = port_associate(portfd, PORT_SOURCE_FD, fd, events,
        (void *)(uintptr_t)mask);
    err = errno;

    if (evport_debug)
        fprintf(stderr, "%d (%s)\n", rv, rv == 0 ? "no error" : strerror(err));

    if (rv == -1) {
        fprintf(stderr, "%s: port_associate: %s\n", where, strerror(err));

        if (err == EAGAIN)
            fprintf(stderr, "aeApiAssociate: event port limit exceeded.");
    }

    return rv;
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    int fullmask, pfd;

    if (evport_debug)
        fprintf(stderr, "aeApiAddEvent: fd %d mask 0x%x\n", fd, mask);

    /*
     * Since port_associate's "events" argument replaces any existing events, we
     * must be sure to include whatever events are already associated when
     * we call port_associate() again.
     */
    // 因为port_associate的事件参数会修改一个已经存在的事件，所以我们要确保在调用的时候要包含已经关联的事项。
    fullmask = mask | eventLoop->events[fd].mask;
    pfd = aeApiLookupPending(state, fd);

    if (pfd != -1) {
        /*
         * This fd was recently returned from aeApiPoll.  It should be safe to
         * assume that the consumer has processed that poll event, but we play
         * it safer by simply updating pending_mask.  The fd will be
         * re-associated as usual when aeApiPoll is called again.
         */
        // fd最近被从aeApiPool返回过的话，要确保可以假设使用者已经处理过了这个poll事项，但是这里使用了一个更加安全的处理思路，我们仅仅更新pending_mask。
        // fd会像正常情况一样，在aeApiPoll被再次调用的时候，被用于重新关联。
        if (evport_debug)
            fprintf(stderr, "aeApiAddEvent: adding to pending fd %d\n", fd);
        state->pending_masks[pfd] |= fullmask;
        return 0;
    }

    return (aeApiAssociate("aeApiAddEvent", state->portfd, fd, fullmask));
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    int fullmask, pfd;

    if (evport_debug)
        fprintf(stderr, "del fd %d mask 0x%x\n", fd, mask);

    pfd = aeApiLookupPending(state, fd);

    if (pfd != -1) {
        if (evport_debug)
            fprintf(stderr, "deleting event from pending fd %d\n", fd);

        /*
         * This fd was just returned from aeApiPoll, so it's not currently
         * associated with the port.  All we need to do is update
         * pending_mask appropriately.
         */
        // 刚刚被从aeApiPoll返回的fd还没有和当前的port关联。我们要做的就是正确的更新pending_mask
        state->pending_masks[pfd] &= ~mask;

        if (state->pending_masks[pfd] == AE_NONE)
            state->pending_fds[pfd] = -1;

        return;
    }

    /*
     * The fd is currently associated with the port.  Like with the add case
     * above, we must look at the full mask for the file descriptor before
     * updating that association.  We don't have a good way of knowing what the
     * events are without looking into the eventLoop state directly.  We rely on
     * the fact that our caller has already updated the mask in the eventLoop.
     */
    // fd已经和该port进行关联。就像上面的添加的情形一样我们一定要在更新关联之前，查看完整的mask。
    // 我们没有比直接查看事件循环的状态更好的办法来确定对应的事项。
    // 我们基于一个事实：调用者已经在事件循环中更新了mask。

    fullmask = eventLoop->events[fd].mask;
    if (fullmask == AE_NONE) {
        /*
         * We're removing *all* events, so use port_dissociate to remove the
         * association completely.  Failure here indicates a bug.
         */
        // 正在移除所有的事项，所以使用port_dissociate完全移除关联。如果失败的话会被认为是一个bug。
        if (evport_debug)
            fprintf(stderr, "aeApiDelEvent: port_dissociate(%d)\n", fd);

        if (port_dissociate(state->portfd, PORT_SOURCE_FD, fd) != 0) {
            perror("aeApiDelEvent: port_dissociate");
            abort(); /* will not return */
        }
    } else if (aeApiAssociate("aeApiDelEvent", state->portfd, fd,
        fullmask) != 0) {
        /*
         * ENOMEM is a potentially transient condition, but the kernel won't
         * generally return it unless things are really bad.  EAGAIN indicates
         * we've reached an resource limit, for which it doesn't make sense to
         * retry (counter-intuitively).  All other errors indicate a bug.  In any
         * of these cases, the best we can do is to abort.
         */
        /* ENOMEM是一个潜在的短暂的情况，但是内核并不会在事情确实出现了问题以外的情况下真正的对这个进行返回。
         * EAGAIN表示我们触及到了一个资源的限制（这种情况下的重试是没有意义的，这一点可能和直觉相反）。
         * 所有的出现的问题都是bug，上面的任意一种情况发生，我们都直接直接报错。
         */
        abort(); /* will not return */
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    struct timespec timeout, *tsp;
    int mask, i;
    uint_t nevents;
    port_event_t event[MAX_EVENT_BATCHSZ];

    /*
     * If we've returned fd events before, we must re-associate them with the
     * port now, before calling port_get().  See the block comment at the top of
     * this file for an explanation of why.
     */
    /* 如果之前已经返回了fd 事项，我们需要在调用port_get之前将他们重新和端口绑定。
     * 可以在文件开头的注释中找到为什么。
     */
    for (i = 0; i < state->npending; i++) {
        if (state->pending_fds[i] == -1)
            /* This fd has since been deleted. */
            continue;

        if (aeApiAssociate("aeApiPoll", state->portfd,
            state->pending_fds[i], state->pending_masks[i]) != 0) {
            /* See aeApiDelEvent for why this case is fatal. */
            abort();
        }

        state->pending_masks[i] = AE_NONE;
        state->pending_fds[i] = -1;
    }

    state->npending = 0;

    if (tvp != NULL) {
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        tsp = &timeout;
    } else {
        tsp = NULL;
    }

    /*
     * port_getn can return with errno == ETIME having returned some events (!).
     * So if we get ETIME, we check nevents, too.
     */
    /* port_getn可以返回errno == ETIME并返回一些事件(!)。
     * 如果得到了一个ETIME，也会检查 nevents
     */
    nevents = 1;
    if (port_getn(state->portfd, event, MAX_EVENT_BATCHSZ, &nevents,
        tsp) == -1 && (errno != ETIME || nevents == 0)) {
        if (errno == ETIME || errno == EINTR)
            return 0;

        /* Any other error indicates a bug. */
        perror("aeApiPoll: port_get");
        abort();
    }

    state->npending = nevents;

    for (i = 0; i < nevents; i++) {
            mask = 0;
            if (event[i].portev_events & POLLIN)
                mask |= AE_READABLE;
            if (event[i].portev_events & POLLOUT)
                mask |= AE_WRITABLE;

            eventLoop->fired[i].fd = event[i].portev_object;
            eventLoop->fired[i].mask = mask;

            if (evport_debug)
                fprintf(stderr, "aeApiPoll: fd %d mask 0x%x\n",
                    (int)event[i].portev_object, mask);

            state->pending_fds[i] = event[i].portev_object;
            state->pending_masks[i] = (uintptr_t)event[i].portev_user;
    }

    return nevents;
}

static char *aeApiName(void) {
    return "evport";
}
