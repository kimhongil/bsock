/*
 * bindsocket_daemon - daemon initialization and signal setup
 *
 * Copyright (c) 2011, Glue Logic LLC. All rights reserved. code()gluelogic.com
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Glue Logic LLC nor the names of its contributors
 *     may be used to endorse or promote products derived from this software
 *     without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <bindsocket_daemon.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ; /* avoid #define _GNU_SOURCE for visibility of environ */

#include <bindsocket_syslog.h>
#include <bindsocket_unixdomain.h>

#ifndef BINDSOCKET_GROUP
#error "BINDSOCKET_GROUP must be defined"
#endif

/* N.B. directory (and tree above it) must be writable only by root */
/* Unit test drivers not run as root should override this location at compile */
#ifndef BINDSOCKET_SOCKET_DIR
#error "BINDSOCKET_SOCKET_DIR must be defined"
#endif
#define BINDSOCKET_SOCKET BINDSOCKET_SOCKET_DIR "/socket"

#ifndef BINDSOCKET_SOCKET_MODE
#define BINDSOCKET_SOCKET_MODE S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP
#endif

/* nointr_close() - make effort to avoid leaking open file descriptors */
static int
nointr_close (const int fd)
{ int r; do { r = close(fd); } while (r != 0 && errno == EINTR); return r; }

bool
bindsocket_daemon_setuid_stdinit (void)
{
    /* Note: not retrying upon interruption; any fail to init means exit fail */

    /* Clear the environment */
    static char *empty_env[] = { NULL };
    environ = empty_env;

    /* Unblock all signals (regardless of what was inherited from parent) */
    sigset_t sigset_empty;
    if (0 != sigemptyset(&sigset_empty)
        || sigprocmask(0 != SIG_SETMASK, &sigset_empty, (sigset_t *) NULL)) {
        bindsocket_syslog(errno, "sigprocmask");
        return false;
    }

    return true;
}

static void
bindsocket_daemon_sa_handler (int signum)
{
    exit(EXIT_SUCCESS);  /* executes atexit() handlers */
}

static bool
bindsocket_daemon_signal_init (void)
{
    /* configure signal handlers for bindsocket desired behaviors
     *   SIGALRM: default handler
     *   SIGPIPE: ignore
     *   SIGCLD:  ignore
     *   SIGHUP:  clean up and exit (for now)
     *   SIGINT:  clean up and exit
     *   SIGQUIT: clean up and exit
     *   SIGTERM: clean up and exit
     */
    struct sigaction act;
    (void) sigemptyset(&act.sa_mask);

    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;  /* omit SA_RESTART */
    if (sigaction(SIGALRM, &act, (struct sigaction *) NULL) != 0) {
        bindsocket_syslog(errno, "sigaction");
        return false;
    }

    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;  /* omit SA_RESTART */
    if (sigaction(SIGPIPE, &act, (struct sigaction *) NULL) != 0) {
        bindsocket_syslog(errno, "sigaction");
        return false;
    }

    act.sa_handler = SIG_IGN;
    act.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &act, (struct sigaction *) NULL) != 0) {
        bindsocket_syslog(errno, "sigaction");
        return false;
    }

    act.sa_handler = bindsocket_daemon_sa_handler;
    act.sa_flags = SA_RESTART;
    if (sigaction(SIGHUP, &act, (struct sigaction *) NULL) != 0) {
        bindsocket_syslog(errno, "sigaction");
        return false;
    }

    act.sa_handler = bindsocket_daemon_sa_handler;
    act.sa_flags = 0;  /* omit SA_RESTART */
    if (   sigaction(SIGINT,  &act, (struct sigaction *) NULL) != 0
        || sigaction(SIGQUIT, &act, (struct sigaction *) NULL) != 0
        || sigaction(SIGTERM, &act, (struct sigaction *) NULL) != 0) {
        bindsocket_syslog(errno, "sigaction");
        return false;
    }

    return true;
}

bool
bindsocket_daemon_init (const int supervised)
{
    /* Note: not retrying upon interruption; any fail to init means exit fail */

    /* Change current working dir to / for sane cwd and to limit mounts in use*/
    if (0 != chdir("/")) {
        bindsocket_syslog(errno, "chdir /");
        return false;
    }

    /* Detach from parent (process to be inherited by init) unless supervised */
    if (supervised) {
        if (setsid() == (pid_t)-1) {
            bindsocket_syslog(errno, "setsid");
            return false;
        }
    }
    else {
        pid_t pid;

        /* Ensure that SIGCHLD is not ignored (might be inherited from caller)*/
        struct sigaction act;
        (void) sigemptyset(&act.sa_mask);
        act.sa_handler = SIG_DFL;
        act.sa_flags = SA_RESTART;
        if (sigaction(SIGCHLD, &act, (struct sigaction *) NULL) != 0) {
            bindsocket_syslog(errno, "sigaction");
            return false;
        }

        if ((pid = fork()) != 0) {   /* parent */
            int status = EXIT_FAILURE;
            if (pid > 0 && waitpid(pid, &status, 0) != pid)
                status = EXIT_FAILURE;
            _exit(status);
        }                            /* child */
        else if ((pid = setsid()) == (pid_t)-1 || (pid = fork()) != 0) {
            if ((pid_t)-1 == pid) bindsocket_syslog(errno, "setsid,fork");
            _exit((pid_t)-1 == pid);
        }                            /* grandchild falls through */
    }

    /* Close unneeded file descriptors */
    /* (not closing all fds > STDERR_FILENO; lazy and we check root is caller)
     * (if closing all fds, must then closelog(); bindsocket_syslog_openlog())*/
    if (0 != nointr_close(STDIN_FILENO))  return false;
    if (0 != nointr_close(STDOUT_FILENO)) return false;
    if (!supervised) {
        if (0 != nointr_close(STDERR_FILENO)) return false;
        bindsocket_syslog_setlevel(BINDSOCKET_SYSLOG_DAEMON);
    }
    else {
        /* STDERR_FILENO must be open so it is not reused for sockets */
        struct stat st;
        if (0 != fstat(STDERR_FILENO, &st)) {
            bindsocket_syslog(errno, "stat STDERR_FILENO");
            return false;
        }
    }

    /* Configure signal handlers for bindsocket desired behaviors */
    if (!bindsocket_daemon_signal_init())
        return false;

    /* Sanity check system socket option max memory for ancillary data
     * (see bindsocket_unixdomain.h for more details) */
  #ifdef __linux__
    {
        ssize_t r;
        long optmem_max;
        const int fd = open("/proc/sys/net/core/optmem_max", O_RDONLY, 0);
        char buf[32];
        if (-1 != fd) {
            if ((r = read(fd, buf, sizeof(buf)-1)) >= 0) {
                buf[r] = '\0';
                errno = 0;
                optmem_max = strtol(buf, NULL, 10);
                if (0 == errno && optmem_max > BINDSOCKET_ANCILLARY_DATA_MAX)
                    bindsocket_syslog(errno, "max ancillary data very large "
                      "(%ld > %d); consider recompiling bindsocket with larger "
                      "BINDSOCKET_ANCILLARY_DATA_MAX", optmem_max,
                      BINDSOCKET_ANCILLARY_DATA_MAX);
            }
            nointr_close(fd);
        }
    }
  #endif

    return true;
}

static int bindsocket_daemon_pid = -1;
static int bindsocket_daemon_socket_bound = -1;

static void
bindsocket_daemon_atexit (void)
{
    if (0 == bindsocket_daemon_socket_bound
        && getpid() == bindsocket_daemon_pid)
        unlink(BINDSOCKET_SOCKET);
}

int
bindsocket_daemon_init_socket (void)
{
    struct group *gr;
    struct stat st;
    int sfd;
    const uid_t euid = geteuid();
    mode_t mask;

    /* sanity check ownership and permissions on dir that will contain socket */
    /* (note: not checking entire tree above BINDSOCKET_SOCKET_DIR; TOC-TOU) */
    if (0 != stat(BINDSOCKET_SOCKET_DIR, &st)) {
        bindsocket_syslog(errno, BINDSOCKET_SOCKET_DIR);
        return -1;
    }
    if (st.st_uid != euid || (st.st_mode & (S_IWGRP|S_IWOTH))) {
        bindsocket_syslog((errno = EPERM),
                          "ownership/permissions incorrect on %s",
                          BINDSOCKET_SOCKET_DIR);
        return -1;
    }

    bindsocket_daemon_pid = getpid();
    atexit(bindsocket_daemon_atexit);

    mask = umask(0177); /* create socket with very restricted permissions */
    sfd = bindsocket_unixdomain_socket_bind_listen(BINDSOCKET_SOCKET,
                                               &bindsocket_daemon_socket_bound);
    umask(mask);        /* restore prior umask */
    if (-1 == sfd) {
        bindsocket_syslog(errno, "socket,bind,listen");
        return -1;
    }

    if (NULL != (gr = getgrnam(BINDSOCKET_GROUP)) /* ok; no other threads yet */
        && 0 == chown(BINDSOCKET_SOCKET, euid, gr->gr_gid)
        && 0 == chmod(BINDSOCKET_SOCKET, BINDSOCKET_SOCKET_MODE))
        return sfd;

    bindsocket_syslog(errno, "getgrnam,chown,chmod");
    return -1;
}