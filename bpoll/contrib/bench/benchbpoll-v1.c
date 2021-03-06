/*
 * Portions
 * Copyright (c) 2012, Glue Logic LLC. All rights reserved. code()gluelogic.com
 *
 * Modified from bench.c provided with libevent and, subsequently libev:
 *
 * Copyright 2003 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * Mon 03/10/2003 - Modified by Davide Libenzi <davidel@xmailserver.org>
 *
 *     Added chain event propagation to improve the sensitivity of
 *     the measure respect to the event loop efficency.
 *
 * Sat 03/24/2012 - Modified by code () gluelogic.com
 *     Support libev NATIVE mode with and without EV_MULTIPLICITY
 *
 */

#include <bpoll/bpoll.h>

#define	timersub(tvp, uvp, vvp)						\
	do {								\
		(vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;		\
		(vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;	\
		if ((vvp)->tv_usec < 0) {				\
			(vvp)->tv_sec--;				\
			(vvp)->tv_usec += 1000000;			\
		}							\
	} while (0)
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#if NATIVE
# include "ev.h"
#if EV_MULTIPLICITY
static struct ev_loop *loop;
#endif
#include <event.h>
static struct event *events;
static struct ev_io *evio;
static struct ev_timer *evto;
#endif


static int count, writes, fired;
static int *pipes;
static int num_pipes, num_active, num_writes;
static struct bpollset_t * restrict bpollset;
static int timers, native;



void
read_cb(bpollset_t * const bpollset  __attribute_unused__  ,
        bpollelt_t * const bpollelt,
        const int data  __attribute_unused__ )
{
	const int fd = bpollelt->fd;
	unsigned char ch[4];

	count += read(fd, ch, sizeof(ch));
	if (writes) {
		write((int)(intptr_t)bpollelt->udata, "e", 1);
		writes--;
		fired++;
	}
}

#if NATIVE
void
read_thunk(EV_P_ struct ev_io *w, int revents)
{
  read_cb (w->fd, revents, w->data);
}

void
timer_cb (EV_P_ struct ev_timer *w, int revents)
{
  /* nop */
}
#endif

struct timeval *
run_once(void)
{
	bpollelt_t *bpollelt;
	int *cp, i, space;
	static struct timeval ta, ts, te;

	gettimeofday(&ta, NULL);
	for (cp = pipes, i = 0; i < num_pipes; i++, cp += 2) {
          if (native)
            {
#if NATIVE
              if (ev_is_active (&evio [i]))
                ev_io_stop (EV_A_ &evio [i]);

              ev_io_set (&evio [i], cp [0], EV_READ);
              ev_io_start (EV_A_ &evio [i]);

              evto [i].repeat = 10. + drand48 ();
              ev_timer_again (EV_A_ &evto [i]);
#endif
            }
          else
            {
                bpollelt = bpoll_elt_init(bpollset, NULL, *cp,
                                          BPOLL_FD_SOCKET, BPOLL_FL_ZERO);
                if (bpollelt == NULL)
                    return perror("bpoll_elt_init"), NULL;  /* ignore error */
                /* pre-cache another descriptor (prior to i in list) for extra
                 * writes (wrap around to end if at beginning of list) */
                bpollelt->udata = (void *)(uintptr_t)
                  pipes[ (i != 0 ? i-1 : num_pipes-1) * 2 + 1 ];

                /* subsequent calls to run_once() after the first will result
                 * in large number of EEXIST returned here (even though extra
                 * syscalls will be avoided), but do it anyway for comparison
                 * to benchev-mod.c.  Uncomment native=1 in main() to avoid. */
                bpoll_elt_add(bpollset, bpollelt, BPOLLIN);  /* ignore error */
            }
	}

	bpoll_poll(bpollset, bpoll_timespec(bpollset));

	fired = 0;
	space = num_pipes / num_active;
	space = space * 2;
	for (i = 0; i < num_active; i++, fired++)
		write(pipes[i * space + 1], "e", 1);

	count = 0;
	writes = num_writes;
	{ int xcount = 0;
	gettimeofday(&ts, NULL);
	do {
		bpoll_poll(bpollset, bpoll_timespec(bpollset));
		xcount++;
	} while (count != fired);
	gettimeofday(&te, NULL);

	//if (xcount != count) fprintf(stderr, "Xcount: %d, Rcount: %d\n", xcount, count);
	}

	timersub(&te, &ta, &ta);
	timersub(&te, &ts, &ts);
		fprintf(stdout, "%8ld %8ld\n",
			ta.tv_sec * 1000000L + ta.tv_usec,
			ts.tv_sec * 1000000L + ts.tv_usec
                        );

	return (&te);
}

int
main (int argc, char **argv)
{
	struct rlimit rl;
	int i, c;
	int *cp;
	extern char *optarg;

	num_pipes = 100;
	num_active = 1;
	num_writes = num_pipes;
	while ((c = getopt(argc, argv, "n:a:w:te")) != -1) {
		switch (c) {
		case 'n':
			num_pipes = atoi(optarg);
			break;
		case 'a':
			num_active = atoi(optarg);
			break;
		case 'w':
			num_writes = atoi(optarg);
			break;
		case 'e':
                        native = 1;
			break;
		case 't':
                        timers = 1;
			break;
		default:
			fprintf(stderr, "Illegal argument \"%c\"\n", c);
			exit(1);
		}
	}

#if 1
	rl.rlim_cur = rl.rlim_max = num_pipes * 2 + 50;
	if (setrlimit(RLIMIT_NOFILE, &rl) == -1) {
		perror("setrlimit");
	}
#endif

#if NATIVE
      #if EV_MULTIPLICITY
	loop = ev_default_loop(0);
      #endif
	evio = calloc(num_pipes, sizeof(struct ev_io));
	evto = calloc(num_pipes, sizeof(struct ev_timer));
	events = calloc(num_pipes, sizeof(struct event));
	if (events == NULL) {
		perror("malloc");
		exit(1);
	}
#endif
	pipes = calloc(num_pipes * 2, sizeof(int));
	if (pipes == NULL) {
		perror("malloc");
		exit(1);
	}

	bpollset = bpoll_create(NULL, read_cb, NULL, NULL, NULL);
	if (bpoll_init(bpollset,BPOLL_M_NOT_SET,num_pipes,num_pipes,0) != 0) {
		perror("bpoll_init");
		exit(1);
	}
	bpoll_timespec_from_msec(bpollset, 0);

	for (cp = pipes, i = 0; i < num_pipes; i++, cp += 2) {
#if NATIVE
          if (native) {
            ev_init (&evto [i], timer_cb);
            ev_init (&evio [i], read_thunk);
            evio [i].data = (void *)i;
          }
#endif
#ifdef USE_PIPES
		if (pipe(cp) == -1) {
#else
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, cp) == -1) {
#endif
			perror("pipe");
			exit(1);
		}
	}

	for (i = 0; i < 2; i++) {
		run_once();
		/*native=1;*/ /* hack to not re-add in second loop */
	}

	exit(0);
}
