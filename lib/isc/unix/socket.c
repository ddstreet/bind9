/* $Id: socket.c,v 1.3 1998/11/07 02:31:04 explorer Exp $ */

#include "attribute.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <isc/assertions.h>
#include <isc/unexpect.h>
#include <isc/thread.h>
#include <isc/mutex.h>
#include <isc/condition.h>
#include <isc/socket.h>

#ifndef _WIN32
#define WINAPI /* we're not windows */
#endif

/*
 * We use macros instead of calling the routines directly because
 * the capital letters make the locking stand out.
 *
 * We INSIST that they succeed since there's no way for us to continue
 * if they fail.
 */

#define LOCK(lp) \
	INSIST(isc_mutex_lock((lp)) == ISC_R_SUCCESS);
#define UNLOCK(lp) \
	INSIST(isc_mutex_unlock((lp)) == ISC_R_SUCCESS);

/*
 * Debugging
 */
#if 1
#define XTRACE(a)	fprintf(stderr, a)
#define XENTER(a)	fprintf(stderr, "ENTER %s\n", (a))
#define XEXIT(a)	fprintf(stderr, "EXIT %s\n", (a))
#else
#define XTRACE(a)
#define XENTER(a)
#define XEXIT(a)
#endif

/*
 * internal event used to send readable/writable events to our internal
 * functions.
 */
typedef struct isc_socket_intev {
	struct isc_event		common;	   /* Sender is the socket. */
	isc_task_t			task;	   /* task to send these to */
	isc_socketevent_t		done_ev;   /* the done event to post */
	isc_boolean_t			partial;   /* partial i/o ok */
	isc_boolean_t			listener;
	isc_taskaction_t		action;	   /* listen needs this too */
	void			       *arg;	   /* listen needs this too */
	LINK(struct isc_socket_intev)	link;
} *isc_socket_intev_t;

#define SOCKET_MAGIC			0x494f696fU	/* IOio */
#define VALID_SOCKET(t)			((t) != NULL && \
				 (t)->magic == SOCKET_MAGIC)
struct isc_socket {
	/* Not locked. */
	unsigned int			magic;
	isc_socketmgr_t			manager;
	isc_mutex_t			lock;
	/* Locked by socket lock. */
	unsigned int			references;
	int				fd;
	LIST(struct isc_socket_intev)	read_list;
	LIST(struct isc_socket_intev)	write_list;
	isc_boolean_t			pending_read;
	isc_boolean_t			pending_write;
	isc_sockettype_t		type;
	isc_socket_intev_t		riev;
	isc_socket_intev_t		wiev;
	struct isc_sockaddr		address;
	int				addrlength;
};

#define SOCKET_MANAGER_MAGIC		0x494f6d67U	/* IOmg */
#define VALID_MANAGER(m)		((m) != NULL && \
					 (m)->magic == SOCKET_MANAGER_MAGIC)
struct isc_socketmgr {
	/* Not locked. */
	unsigned int			magic;
	isc_memctx_t			mctx;
	isc_mutex_t			lock;
	/* Locked by manager lock. */
	isc_boolean_t			done;
	unsigned int			nscheduled;
	unsigned int			nsockets;  /* sockets managed */
	isc_thread_t			thread;
	fd_set				read_fds;
	fd_set				write_fds;
	isc_socket_t			fds[FD_SETSIZE];
	int				pipe_fds[2];
	sig_atomic_t			pipe_msgs;
};

typedef int select_msg_t;
#define SELECT_POKE_SHUTDOWN		(-1)
#define SELECT_POKE_NOTHING		(-2)

static void send_done_event(isc_socket_t, isc_socket_intev_t *,
			    isc_socketevent_t *, isc_result_t);
static void done_event_destroy(isc_event_t);

/*
 * poke the select loop when there is something for us to do.  Manager must
 * be locked.
 */
static void
select_poke(isc_socketmgr_t mgr, select_msg_t msg)
{
	int cc;

	cc = write(mgr->pipe_fds[1], &msg, sizeof(select_msg_t));
	if (cc < 0)
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "write() failed during watcher poke: %s",
				 strerror(errno));

	INSIST(cc == sizeof(select_msg_t));
}

/*
 * read a message on the internal fd.
 */
static select_msg_t
select_readmsg(isc_socketmgr_t mgr)
{
	select_msg_t msg;
	int cc;

	cc = read(mgr->pipe_fds[0], &msg, sizeof(select_msg_t));
	if (cc < 0) {
		if (errno == EWOULDBLOCK)
			return SELECT_POKE_NOTHING;

		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "read() failed during watcher poke: %s",
				 strerror(errno));
		return SELECT_POKE_NOTHING;  /* XXX */
	}

	INSIST(cc == sizeof(select_msg_t));

	return msg;
}

/*
 * Make a fd non-blocking
 */
static isc_result_t
make_nonblock(int fd)
{
	int ret;
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	flags |= O_NONBLOCK;
	ret = fcntl(fd, F_SETFL, flags);

	if (ret == -1) {
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "fcntl(%d, F_SETFL, %d): %s",
				 fd, flags, strerror(errno));
		return ISC_R_UNEXPECTED;
	}

	return ISC_R_SUCCESS;
}

/*
 * Handle freeing a done event when needed.
 */
static void
done_event_destroy(isc_event_t ev)
{
	isc_socket_t sock = ev->sender;

	/*
	 * detach from the socket.  We would have already detached from the
	 * task when we actually queue this event up.
	 */
	LOCK(&sock->lock);
	sock->references--;
	UNLOCK(&sock->lock);
}

/*
 * Kill.
 *
 * Caller must ensure locking.
 */
static void
destroy(isc_socket_t sock)
{
	isc_socketmgr_t manager = sock->manager;

	LOCK(&manager->lock);

	/*
	 * Noone has this socket open, so the watcher doesn't have to be
	 * poked.
	 */
	manager->fds[sock->fd] = NULL;
	manager->nsockets--;

	UNLOCK(&manager->lock);

	if (sock->riev)
		isc_event_free((isc_event_t *)&sock->riev);
	if (sock->wiev)
		isc_event_free((isc_event_t *)&sock->wiev);

	(void)isc_mutex_destroy(&sock->lock);
	sock->magic = 0;
	isc_mem_put(manager->mctx, sock, sizeof *sock);
}

/*
 * Create a new 'type' socket managed by 'manager'.  The sockets
 * parameters are specified by 'expires' and 'interval'.  Events
 * will be posted to 'task' and when dispatched 'action' will be
 * called with 'arg' as the arg value.  The new socket is returned
 * in 'socketp'.
 */
isc_result_t
isc_socket_create(isc_socketmgr_t manager, isc_sockettype_t type,
		  isc_socket_t *socketp)
{
	isc_socket_t sock;

	REQUIRE(VALID_MANAGER(manager));
	REQUIRE(socketp != NULL && *socketp == NULL);

	XENTER("isc_socket_create");

	sock = isc_mem_get(manager->mctx, sizeof *sock);
	if (sock == NULL)
		return (ISC_R_NOMEMORY);

	sock->magic = SOCKET_MAGIC;
	sock->manager = manager;
	sock->references = 1;
	sock->type = type;

	/*
	 * set up list of readers and writers to be initially empty
	 */
	INIT_LIST(sock->read_list);
	INIT_LIST(sock->write_list);
	sock->pending_read = ISC_FALSE;
	sock->pending_write = ISC_FALSE;

	/*
	 * Create the associated socket XXX
	 */
	switch (type) {
	case isc_socket_udp:
		sock->fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
		break;
	case isc_socket_tcp:
		sock->fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
		break;
	}
	if (sock->fd < 0) {
		isc_mem_put(manager->mctx, sock, sizeof *sock);

		switch (errno) {
		case EMFILE:
		case ENFILE:
		case ENOBUFS:
			return (ISC_R_NORESOURCES);
			break;
		default:
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "socket() failed: %s",
					 strerror(errno));
			return (ISC_R_UNEXPECTED);
			break;
		}
	}

	if (make_nonblock(sock->fd) != ISC_R_SUCCESS) {
		isc_mem_put(manager->mctx, sock, sizeof *sock);
		close(sock->fd);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "make_nonblock(%d)", sock->fd);
		return (ISC_R_UNEXPECTED);
	}

	/*
	 * initialize the lock
	 */
	if (isc_mutex_init(&sock->lock) != ISC_R_SUCCESS) {
		isc_mem_put(manager->mctx, sock, sizeof *sock);
		close(sock->fd);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() failed");
		return (ISC_R_UNEXPECTED);
	}

	LOCK(&manager->lock);

	/*
	 * Note we don't have to lock the socket like we normally would because
	 * there are no external references to it yet.
	 */

	manager->fds[sock->fd] = sock;
	manager->nsockets++;

	UNLOCK(&manager->lock);

	*socketp = sock;

	XEXIT("isc_socket_create");

	return (ISC_R_SUCCESS);
}

/*
 * Attach to a socket.  Caller must explicitly detach when it is done.
 */
void
isc_socket_attach(isc_socket_t sock, isc_socket_t *socketp)
{
	REQUIRE(VALID_SOCKET(sock));
	REQUIRE(socketp != NULL && *socketp == NULL);

	LOCK(&sock->lock);
	sock->references++;
	UNLOCK(&sock->lock);
	
	*socketp = sock;
}

/*
 * Dereference a socket.  If this is the last reference to it, clean things
 * up by destroying the socket.
 */
void 
isc_socket_detach(isc_socket_t *socketp)
{
	isc_socket_t sock;
	isc_boolean_t free_socket = ISC_FALSE;

	REQUIRE(socketp != NULL);
	sock = *socketp;
	REQUIRE(VALID_SOCKET(sock));

	XENTER("isc_socket_detach");

	LOCK(&sock->lock);
	REQUIRE(sock->references > 0);
	sock->references--;
	if (sock->references == 0)
		free_socket = ISC_TRUE;
	UNLOCK(&sock->lock);
	
	if (free_socket)
		destroy(sock);

	XEXIT("isc_socket_detach");

	*socketp = NULL;
}

/*
 * I/O is possible on a given socket.  Schedule an event to this task that
 * will call an internal function to do the I/O.  This will charge the
 * task with the I/O operation and let our select loop handler get back
 * to doing something real as fast as possible.
 *
 * The socket and manager must be locked before calling this function.
 */
static void
dispatch_read(isc_socket_t sock)
{
	isc_socket_intev_t iev;
	isc_event_t ev;

	iev = HEAD(sock->read_list);
	ev = (isc_event_t)iev;

	INSIST(!sock->pending_read);

	sock->pending_read = ISC_TRUE;

	isc_task_send(iev->task, &ev);
}

static void
dispatch_write(isc_socket_t sock)
{
	isc_socket_intev_t iev;
	isc_event_t ev;

	iev = HEAD(sock->write_list);
	ev = (isc_event_t)iev;

	INSIST(!sock->pending_write);
	sock->pending_write = ISC_TRUE;

	isc_task_send(iev->task, &ev);
}

/*
 * Dequeue an item off the given socket's read queue, set the result code
 * in the done event to the one provided, and send it to the task it was
 * destined for.
 *
 * Caller must have the socket locked.
 */
static void
send_done_event(isc_socket_t sock, isc_socket_intev_t *iev,
		isc_socketevent_t *dev, isc_result_t resultcode)
{
	REQUIRE(!EMPTY(sock->read_list));
	REQUIRE(iev != NULL);
	REQUIRE(*iev != NULL);
	REQUIRE(dev != NULL);
	REQUIRE(*dev != NULL);

	DEQUEUE(sock->read_list, *iev, link);
	(*dev)->result = resultcode;
	isc_task_send((*iev)->task, (isc_event_t *)dev);
	(*iev)->done_ev = NULL;
	isc_event_free((isc_event_t *)iev);
}

static isc_boolean_t
internal_accept(isc_task_t task, isc_event_t ev)
{
}

static isc_boolean_t
internal_read(isc_task_t task, isc_event_t ev)
{
	isc_socket_intev_t iev;
	isc_socketevent_t dev;
	isc_socket_t sock;
	int cc;
	size_t read_count;

	/*
	 * Find out what socket this is and lock it.
	 */
	sock = (isc_socket_t)ev->sender;
	LOCK(&sock->lock);

	INSIST(sock->pending_read == ISC_TRUE);
	sock->pending_read = ISC_FALSE;

	/*
	 * Pull the first entry off the list, and look at it.  If it is
	 * NULL, or not ours, something bad happened.
	 */
	iev = HEAD(sock->read_list);
	INSIST(iev != NULL);
	INSIST(iev->task == task);

	/*
	 * Try to do as much I/O as possible on this socket.  There are no
	 * limits here, currently.  If some sort of quantum read count is
	 * desired before giving up control, make certain to process markers
	 * regardless of quantum.
	 */
	do {
		iev = HEAD(sock->read_list);
		dev = iev->done_ev;

		/*
		 * If dev is null here, there is no done event.  This means
		 * someone other than us canceled our pending I/O.
		 * Just ignore this case.
		 */
		if (dev == NULL) {
			DEQUEUE(sock->read_list, iev, link);
			isc_event_free((isc_event_t *)&iev);
			continue;
		}

		/*
		 * If this is a marker event, post its completion and
		 * continue the loop.
		 */
		if (dev->common.type == ISC_SOCKEVENT_RECVMARK) {
			send_done_event(sock, &iev, &dev, ISC_R_SUCCESS);
			continue;
		}

		/*
		 * It must be a read request.  Try to satisfy it as best
		 * we can.
		 */
		read_count = dev->region.length - dev->n;
		cc = recv(sock->fd, dev->region.base + dev->n, read_count, 0);

		/*
		 * check for error or block condition
		 */
		if (cc < 0) {
			if (cc == EWOULDBLOCK)
				goto poke;
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "internal read: %s",
					 strerror(errno));
			INSIST(cc >= 0);
		}
		/*
		 * read of 0 means the remote end was closed.  Run through
		 * the event queue and dispatch all the events with an EOF
		 * result code.  This will set the EOF flag in markers as
		 * well, but that's really ok.
		 */
		if (cc == 0) {
			do {
				send_done_event(sock, &iev, &dev,
						ISC_R_EOF);
				iev = HEAD(sock->read_list);
			} while (iev != NULL);

			goto poke;
		}

		/*
		 * if we read less than we expected, update counters,
		 * poke.
		 */
		if ((size_t)cc < read_count) {
			dev->n += cc;

			/*
			 * If partial reads are allowed, we return whatever
			 * was read with a success result, and continue
			 * the loop.
			 */
			if (iev->partial) {
				send_done_event(sock, &iev, &dev,
						ISC_R_SUCCESS);
				continue;
			}

			/*
			 * Partials not ok.  Exit the loop and notify the
			 * watcher to wait for more reads
			 */
			goto poke;
		}

		/*
		 * Exactly what we wanted to read.  We're done with this
		 * entry.  Post its completion event.
		 */
		if ((size_t)cc == read_count)
			send_done_event(sock, &iev, &dev, ISC_R_SUCCESS);

	} while (!EMPTY(sock->read_list));

 poke:
	if (!EMPTY(sock->read_list))
		select_poke(sock->manager, sock->fd);

	UNLOCK(&sock->lock);

	return (0);
}

/*
 * This is the thread that will loop forever, always in a select or poll
 * call.
 *
 * When select returns something to do, track down what thread gets to do
 * this I/O and post the event to it.
 */
static isc_threadresult_t
WINAPI
watcher(void *uap)
{
	isc_socketmgr_t manager = uap;
	isc_socket_t sock;
	isc_boolean_t done;
	int ctlfd;
	int cc;
	fd_set readfds;
	fd_set writefds;
	select_msg_t msg;
	isc_boolean_t unlock_sock;
	int i;
	isc_socket_intev_t	iev;

	/*
	 * Get the control fd here.  This will never change.
	 */
	LOCK(&manager->lock);
	ctlfd = manager->pipe_fds[0];

	done = ISC_FALSE;
	while (!done) {
		readfds = manager->read_fds;
		writefds = manager->write_fds;

		UNLOCK(&manager->lock);

		do {
			cc = select(FD_SETSIZE, &readfds, &writefds, NULL,
				    NULL);
			if (cc < 0) {
				if (errno != EINTR)
					UNEXPECTED_ERROR(__FILE__, __LINE__,
							 "select failed: %s",
							 strerror(errno));
			}
		} while (cc < 0);

		LOCK(&manager->lock);

		/*
		 * Process reads on internal, control fd.
		 */
		if (FD_ISSET(ctlfd, &readfds)) {
			while (1) {
				msg = select_readmsg(manager);

				/*
				 * Nothing to read?
				 */
				if (msg == SELECT_POKE_NOTHING)
					break;

				/*
				 * handle shutdown message.  We really should
				 * jump out of this loop right away, but
				 * it doesn't matter if we have to do a little
				 * more work first.
				 */
				if (msg == SELECT_POKE_SHUTDOWN)
					done = ISC_TRUE;

				/*
				 * This is a wakeup on a socket.  Look
				 * at the event queue for both read and write,
				 * and decide if we need to watch on it now
				 * or not.
				 */
				if (msg >= 0) {
					INSIST(msg < FD_SETSIZE);

					sock = manager->fds[msg];
					LOCK(&sock->lock);

					/*
					 * If there are no events, or there
					 * is an event but we have already
					 * queued up the internal event on a
					 * task's queue, clear the bit.
					 * Otherwise, set it.
					 */
					iev = HEAD(sock->read_list);
					if (iev == NULL || sock->pending_read)
						FD_CLR(sock->fd,
						       &manager->read_fds);
					else
						FD_SET(sock->fd,
						       &manager->read_fds);

					iev = HEAD(sock->write_list);
					if (iev == NULL || sock->pending_write)
						FD_CLR(sock->fd,
						       &manager->write_fds);
					else
						FD_SET(sock->fd,
						       &manager->write_fds);

					UNLOCK(&sock->lock);
				}
			}
		}

		/*
		 * Process read/writes on other fds here.  Avoid locking
		 * and unlocking twice if both reads and writes are possible.
		 */
		for (i = 0 ; i < FD_SETSIZE ; i++) {
			if (manager->fds[i] != NULL) {
				sock = manager->fds[i];
				unlock_sock = ISC_FALSE;
				if (FD_ISSET(i, &readfds)) {
					unlock_sock = ISC_TRUE;
					LOCK(&sock->lock);
					dispatch_read(sock);
					FD_CLR(i, &manager->read_fds);
				}
				if (FD_ISSET(i, &writefds)) {
					if (!unlock_sock) {
						unlock_sock = ISC_TRUE;
						LOCK(&sock->lock);
					}
					dispatch_write(sock);
					FD_CLR(i, &manager->write_fds);
				}
				if (unlock_sock)
					UNLOCK(&sock->lock);
			}
		}
	}

	UNLOCK(&manager->lock);
	return ((isc_threadresult_t)0);
}

/*
 * Create a new socket manager.
 */
isc_result_t
isc_socketmgr_create(isc_memctx_t mctx, isc_socketmgr_t *managerp)
{
	isc_socketmgr_t manager;

	REQUIRE(managerp != NULL && *managerp == NULL);

	XENTER("isc_socketmgr_create");

	manager = isc_mem_get(mctx, sizeof *manager);
	if (manager == NULL)
		return (ISC_R_NOMEMORY);
	
	manager->magic = SOCKET_MANAGER_MAGIC;
	manager->mctx = mctx;
	manager->done = ISC_FALSE;
	memset(manager->fds, 0, sizeof(manager->fds));
	manager->nsockets = 0;
	manager->nscheduled = 0;
	if (isc_mutex_init(&manager->lock) != ISC_R_SUCCESS) {
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_mutex_init() failed");
		return (ISC_R_UNEXPECTED);
	}

	/*
	 * Create the special fds that will be used to wake up the
	 * select/poll loop when something internal needs to be done.
	 */
	if (pipe(manager->pipe_fds) != 0) {
		(void)isc_mutex_destroy(&manager->lock);
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "pipe() failed: %s",
				 strerror(errno)); /* XXX */

		return (ISC_R_UNEXPECTED);
	}

	INSIST(make_nonblock(manager->pipe_fds[0]) == ISC_R_SUCCESS);
	INSIST(make_nonblock(manager->pipe_fds[1]) == ISC_R_SUCCESS);

	/*
	 * Set up initial state for the select loop
	 */
	FD_ZERO(&manager->read_fds);
	FD_ZERO(&manager->write_fds);
	FD_SET(manager->pipe_fds[0], &manager->read_fds);

	/*
	 * Start up the select/poll thread.
	 */
	if (isc_thread_create(watcher, manager, &manager->thread) !=
	    ISC_R_SUCCESS) {
		(void)isc_mutex_destroy(&manager->lock);
		isc_mem_put(mctx, manager, sizeof *manager);
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_thread_create() failed");
		return (ISC_R_UNEXPECTED);
	}

	*managerp = manager;

	XEXIT("isc_socketmgr_create (normal)");
	return (ISC_R_SUCCESS);
}

void
isc_socketmgr_destroy(isc_socketmgr_t *managerp)
{
	isc_socketmgr_t manager;

	/*
	 * Destroy a socket manager.
	 */

	REQUIRE(managerp != NULL);
	manager = *managerp;
	REQUIRE(VALID_MANAGER(manager));

	LOCK(&manager->lock);
	REQUIRE(manager->nsockets == 0);
	manager->done = ISC_TRUE;
	UNLOCK(&manager->lock);

	/*
	 * Here, poke our select/poll thread.  Do this by closing the write
	 * half of the pipe, which will send EOF to the read half.
	 */
	select_poke(manager, SELECT_POKE_SHUTDOWN);

	/*
	 * Wait for thread to exit.
	 */
	if (isc_thread_join(manager->thread, NULL) != ISC_R_SUCCESS)
		UNEXPECTED_ERROR(__FILE__, __LINE__,
				 "isc_thread_join() failed");

	/*
	 * Clean up.
	 */
	close(manager->pipe_fds[0]);
	close(manager->pipe_fds[1]);
	(void)isc_mutex_destroy(&manager->lock);
	manager->magic = 0;
	isc_mem_put(manager->mctx, manager, sizeof *manager);

	*managerp = NULL;
}

isc_result_t
isc_socket_recv(isc_socket_t sock, isc_region_t region,
		isc_boolean_t partial, isc_task_t task,
		isc_taskaction_t action, void *arg)
{
	isc_socketevent_t ev;
	isc_socket_intev_t iev;
	isc_socketmgr_t manager;
	isc_task_t ntask;

	manager = sock->manager;

	ev = (isc_socketevent_t)isc_event_allocate(manager->mctx, sock,
						   ISC_SOCKEVENT_RECVDONE,
						   action, arg, sizeof(*ev));
	if (ev == NULL)
		return (ISC_R_NOMEMORY);

	LOCK(&sock->lock);

	if (sock->riev == NULL) {
		iev = (isc_socket_intev_t)isc_event_allocate(manager->mctx,
							     sock,
							     ISC_SOCKEVENT_INTIO,
							     internal_read,
							     sock,
							     sizeof(*iev));
		if (iev == NULL) {
			/* no special free routine yet */
			isc_event_free((isc_event_t *)&ev);
			return (ISC_R_NOMEMORY);
		}

		INIT_LINK(iev, link);

		sock->riev = iev;
		iev = NULL;  /* just in case */
	}

	sock->references++;  /* attach to socket in cheap way */

	/*
	 * Remember that we need to detach on event free
	 */
	ev->common.destroy = done_event_destroy;

	ev->region = *region;

	/*
	 * If the read queue is empty, try to do the I/O right now.
	 */
#if 0
	if (EMPTY(sock->read_list)) {
		cc = recv(...);

		isc_task_send(task, (isc_event_t *)&ev);

		UNLOCK(&sock->lock);

		return (ISC_R_SUCCESS);
	}
#endif

	/*
	 * We couldn't read all or part of the request right now, so queue
	 * it.
	 */
	iev = sock->riev;
	sock->riev = NULL;

	isc_task_attach(task, &ntask);

	iev->done_ev = ev;
	iev->task = ntask;
	iev->partial = partial;

	/*
	 * Enqueue the request.  If the socket was previously not being
	 * watched, poke the watcher to start paying attention to it.
	 */
	if (EMPTY(sock->read_list)) {
		ENQUEUE(sock->read_list, iev, link);
		select_poke(sock->manager, sock->fd);
	} else {
		ENQUEUE(sock->read_list, iev, link);
	}

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

isc_result_t
isc_socket_bind(isc_socket_t sock, struct isc_sockaddr *sockaddr,
		int addrlen)
{
	LOCK(&sock->lock);

	if (bind(sock->fd, (struct sockaddr *)sockaddr, addrlen) < 0) {
		UNLOCK(&sock->lock);
		switch (errno) {
		case EACCES:
			return (ISC_R_NOPERM);
			break;
		case EADDRNOTAVAIL:
			return (ISC_R_ADDRNOTAVAIL);
			break;
		case EADDRINUSE:
			return (ISC_R_ADDRINUSE);
			break;
		case EINVAL:
			return (ISC_R_BOUND);
			break;
		default:
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "bind: %s", strerror(errno));
			return (ISC_R_UNEXPECTED);
			break;
		}
	}

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}

/*
 * set up to listen on a given socket.  We do this by creating an internal
 * event that will be dispatched when the socket has read activity.  The
 * watcher will send the internal event to the task when there is a new
 * connection.
 *
 * Unlike in read, we don't preallocate a done event here.  Every time there
 * is a new connection we'll have to allocate a new one anyway, so we might
 * as well keep things simple rather than having to track them.
 */
isc_result_t
isc_socket_listen(isc_socket_t sock, int backlog, isc_task_t task,
		  isc_taskaction_t action, void *arg)
{
	isc_socket_intev_t iev;
	isc_task_t ntask;
	isc_socketmgr_t manager;

	manager = sock->manager;

	iev = (isc_socket_intev_t)isc_event_allocate(manager->mctx,
						     sock,
						     ISC_SOCKEVENT_INTCONN,
						     internal_accept,
						     sock,
						     sizeof(*iev));
	if (iev == NULL)
		return (ISC_R_NOMEMORY);

	INIT_LINK(iev, link);

	LOCK(&sock->lock);

	if (listen(sock->fd, backlog) < 0) {
		isc_event_free((isc_event_t *)&iev);
		UNLOCK(&sock->lock);
		UNEXPECTED_ERROR(__FILE__, __LINE__, "listen: %s",
				 strerror(errno));

		return (ISC_R_UNEXPECTED);
	}

	/*
	 * Attach to socket and to task
	 */
	isc_task_attach(task, &ntask);
	sock->references++;

	iev->task = ntask;
	iev->done_ev = NULL;
	iev->partial = ISC_FALSE;  /* state doesn't really matter */
	iev->listener = ISC_TRUE;
	iev->action = action;
	iev->arg = arg;

	UNLOCK(&sock->lock);

	return (ISC_R_SUCCESS);
}
