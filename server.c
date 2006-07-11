/*****************************************************************************\
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2001-2006 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Chris Dunlap <cdunlap@llnl.gov>.
 *  UCRL-CODE-2002-009.
 *  
 *  This file is part of ConMan, a remote console management program.
 *  For details, see <http://www.llnl.gov/linux/conman/>.
 *  
 *  ConMan is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  ConMan is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with ConMan; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/


#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#include <arpa/telnet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common.h"
#include "list.h"
#include "log.h"
#include "server.h"
#include "tpoll.h"
#include "util-file.h"
#include "util-str.h"
#include "util.h"
#include "wrapper.h"


#ifdef NDEBUG
static int begin_daemonize(void);
static void end_daemonize(int fd);
#endif /* NDEBUG */
static void display_configuration(server_conf_t *conf);
static void exit_handler(int signum);
static void sig_chld_handler(int signum);
static void sig_hup_handler(int signum);
static void schedule_timestamp(server_conf_t *conf);
static void timestamp_logfiles(server_conf_t *conf);
static void create_listen_socket(server_conf_t *conf);
static void open_objs(server_conf_t *conf);
static void mux_io(server_conf_t *conf);
static void open_daemon_logfile(server_conf_t *conf);
static void reopen_logfiles(server_conf_t *conf);
static void accept_client(server_conf_t *conf);
static void reset_console(obj_t *console, const char *cmd);
static void kill_console_reset(pid_t *arg);

static int done = 0;
static int reconfig = 0;

/*  The 'tp_global' var is to allow timers to be set or canceled
 *    without having to pass the conf's tp var through the call stack.
 */
tpoll_t tp_global = NULL;


int main(int argc, char *argv[])
{
    int fd;
    server_conf_t *conf;

#ifdef NDEBUG
    log_set_file(stderr, LOG_WARNING, 0);
    fd = begin_daemonize();
#else /* NDEBUG */
    log_set_file(stderr, LOG_DEBUG, 0);
    fd = -1;                            /* suppress unused variable warning */
#endif /* NDEBUG */

    posix_signal(SIGCHLD, sig_chld_handler);
    posix_signal(SIGHUP, sig_hup_handler);
    posix_signal(SIGINT, exit_handler);
    posix_signal(SIGPIPE, SIG_IGN);
    posix_signal(SIGTERM, exit_handler);

    conf = create_server_conf(argc, argv);
    tp_global  = conf->tp;

    if (conf->enableVerbose)
        display_configuration(conf);
    if (list_is_empty(conf->objs))
        log_err(0, "Configuration \"%s\" has no consoles defined",
            conf->confFileName);
    if (conf->tStampMinutes > 0)
        schedule_timestamp(conf);

    create_listen_socket(conf);

    if (conf->syslogFacility > 0)
        log_set_syslog(argv[0], conf->syslogFacility);
    if (conf->logFileName)
        open_daemon_logfile(conf);

#ifdef NDEBUG
    end_daemonize(fd);
    if (!conf->logFileName)
        log_set_file(NULL, 0, 0);
#endif /* NDEBUG */

    log_msg(LOG_NOTICE, "Starting ConMan daemon %s (pid %d)",
        VERSION, (int) getpid());

    open_objs(conf);
    mux_io(conf);
    destroy_server_conf(conf);

    log_msg(LOG_NOTICE, "Stopping ConMan daemon %s (pid %d)",
        VERSION, (int) getpid());
    exit(0);
}


#ifdef NDEBUG
static int begin_daemonize(void)
{
/*  Begins the daemonization of the process.
 *  Despite the fact that this routine backgrounds the process, control
 *    will not be returned to the shell until end_daemonize() is called.
 *  Returns an 'fd' to pass to end_daemonize() to complete the daemonization.
 */
    struct rlimit limit;
    int fdPair[2];
    pid_t pid;
    int n;
    char c;
    int rc;

    /*  Clear file mode creation mask.
     */
    umask(0);

    /*  Disable creation of core files.
     */
    limit.rlim_cur = 0;
    limit.rlim_max = 0;
    if (setrlimit(RLIMIT_CORE, &limit) < 0)
        log_err(errno, "Unable to prevent creation of core file");

    /*  Create pipe for IPC so parent process will wait to terminate until
     *    signaled by grandchild process.  This allows messages written to
     *    stdout/stderr by the grandchild to be properly displayed before
     *    the parent process returns control to the shell.
     */
    if (pipe(fdPair) < 0)
        log_err(errno, "Unable to create pipe");

    /*  Set the fd used by log_err() to return status back to the parent.
     */
    log_daemonize_fd = fdPair[1];

    /*  Automatically background the process and
     *    ensure child is not a process group leader.
     */
    if ((pid = fork()) < 0) {
        log_err(errno, "Unable to create child process");
    }
    else if (pid > 0) {
        if (close(fdPair[1]) < 0)
            log_err(errno, "Unable to close write-pipe in parent");
        if ((n = read(fdPair[0], &c, 1)) < 0)
            log_err(errno, "Unable to read status from grandchild");
        rc = ((n == 1) && (c != 0)) ? 1 : 0;
        exit(rc);
    }
    if (close(fdPair[0]) < 0)
        log_err(errno, "Unable to close read-pipe in child");

    /*  Become a session leader and process group leader
     *    with no controlling tty.
     */
    if (setsid() < 0)
        log_err(errno, "Unable to disassociate controlling tty");

    /*  Ignore SIGHUP to keep child from terminating when
     *    the session leader (ie, the parent) terminates.
     */
    posix_signal(SIGHUP, SIG_IGN);

    /*  Abdicate session leader position in order to guarantee
     *    daemon cannot automatically re-acquire a controlling tty.
     */
    if ((pid = fork()) < 0)
        log_err(errno, "Unable to create grandchild process");
    else if (pid > 0)
        exit(0);

    return(fdPair[1]);
}
#endif /* NDEBUG */


#ifdef NDEBUG
static void end_daemonize(int fd)
{
/*  Completes the daemonization of the process,
 *    where 'fd' is the value returned by begin_daemonize().
 */
    int devnull;

    /*  Ensure process does not keep a directory in use.
     *  Avoid relative pathname from this point on!
     */
    if (chdir("/") < 0)
        log_err(errno, "Unable to change to root directory");

    /*  Discard data to/from stdin, stdout, and stderr.
     */
    if ((devnull = open("/dev/null", O_RDWR)) < 0)
        log_err(errno, "Unable to open \"/dev/null\"");
    if (dup2(devnull, STDIN_FILENO) < 0)
        log_err(errno, "Unable to dup \"/dev/null\" onto stdin");
    if (dup2(devnull, STDOUT_FILENO) < 0)
        log_err(errno, "Unable to dup \"/dev/null\" onto stdout");
    if (dup2(devnull, STDERR_FILENO) < 0)
        log_err(errno, "Unable to dup \"/dev/null\" onto stderr");
    if (close(devnull) < 0)
        log_err(errno, "Unable to close \"/dev/null\"");

    /*  Signal grandparent process to terminate.
     */
    log_daemonize_fd = -1;
    if ((fd >= 0) && (close(fd) < 0))
        log_err(errno, "Unable to close write-pipe in grandchild");

    return;
}
#endif /* NDEBUG */


static void exit_handler(int signum)
{
    log_msg(LOG_NOTICE, "Exiting on signal=%d", signum);
    done = 1;
    return;
}


static void sig_hup_handler(int signum)
{
    log_msg(LOG_NOTICE, "Performing reconfig on signal=%d", signum);
    reconfig = 1;
    return;
}


static void sig_chld_handler(int signum)
{
    pid_t pid;

    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
        DPRINTF((5, "Process %d terminated.\n", (int) pid));
    return;
}


static void display_configuration(server_conf_t *conf)
{
/*  Displays a summary of the server's configuration.
 */
    ListIterator i;
    obj_t *obj;
    int n = 0;
    int gotOptions = 0;

    i = list_iterator_create(conf->objs);
    while ((obj = list_next(i)))
        if (is_console_obj(obj))
            n++;
    list_iterator_destroy(i);

    fprintf(stderr, "\nStarting ConMan daemon %s (pid %d)\n",
        VERSION, (int) getpid());
    fprintf(stderr, "Configuration: %s\n", conf->confFileName);
    fprintf(stderr, "Options:");

    if (conf->enableKeepAlive) {
        fprintf(stderr, " KeepAlive");
        gotOptions++;
    }
    if (conf->logFileName) {
        fprintf(stderr, " LogFile");
        gotOptions++;
    }
    if (conf->enableLoopBack) {
        fprintf(stderr, " LoopBack");
        gotOptions++;
    }
    if (conf->resetCmd) {
        fprintf(stderr, " ResetCmd");
        gotOptions++;
    }
    if (conf->syslogFacility >= 0) {
        fprintf(stderr, " SysLog");
        gotOptions++;
    }
    if (conf->enableTCPWrap) {
        fprintf(stderr, " TCP-Wrappers");
        gotOptions++;
    }
    if (conf->tStampMinutes > 0) {
        fprintf(stderr, " TimeStamp=%dm", conf->tStampMinutes);
        gotOptions++;
    }
    if (conf->enableZeroLogs) {
        fprintf(stderr, " ZeroLogs");
        gotOptions++;
    }
    if (!gotOptions) {
        fprintf(stderr, " None");
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "Listening on port %d\n", conf->port);
    fprintf(stderr, "Monitoring %d console%s\n", n, ((n == 1) ? "" : "s"));
    fprintf(stderr, "\n");
    return;
}


static void schedule_timestamp(server_conf_t *conf)
{
/*  Schedules a timer for writing timestamps to the console logfiles.
 */
    time_t t;
    struct tm tm;
    struct timeval tv;
    int numCompleted;

    assert(conf->tStampMinutes > 0);

    t = conf->tStampNext;
    get_localtime(&t, &tm);
    /*
     *  If this is the first scheduled timestamp, compute the expiration time
     *    assuming timestamps have been scheduled regularly since midnight.
     *  Otherwise, base it off of the previous timestamp.
     */
    if (!conf->tStampNext) {
        numCompleted = ((tm.tm_hour * 60) + tm.tm_min) / conf->tStampMinutes;
        tm.tm_min = (numCompleted + 1) * conf->tStampMinutes;
        tm.tm_hour = 0;
    }
    else {
        tm.tm_min += conf->tStampMinutes;
    }
    tm.tm_sec = 0;

    if ((t = mktime(&tm)) == ((time_t) -1))
        log_err(errno, "Unable to determine time of next logfile timestamp");
    tv.tv_sec = t;
    tv.tv_usec = 0;
    conf->tStampNext = t;

    /*  The timer id is not saved because this timer will never be canceled.
     */
    if (tpoll_timeout_absolute (tp_global,
            (callback_f) timestamp_logfiles, conf, &tv) < 0) {
        log_err(0, "Unable to create timer for timestamping logfiles");
    }
    return;
}


static void timestamp_logfiles(server_conf_t *conf)
{
/*  Writes a timestamp message into all of the console logfiles.
 */
    char *now;
    ListIterator i;
    obj_t *logfile;
    char buf[MAX_LINE];
    int gotLogs = 0;

    now = create_long_time_string(0);
    i = list_iterator_create(conf->objs);
    while ((logfile = list_next(i))) {
        if (!is_logfile_obj(logfile))
            continue;
        snprintf(buf, sizeof(buf), "%sConsole [%s] log at %s%s",
            CONMAN_MSG_PREFIX, logfile->aux.logfile.console->name,
            now, CONMAN_MSG_SUFFIX);
        strcpy(&buf[sizeof(buf) - 3], "\r\n");
        write_obj_data(logfile, buf, strlen(buf), 1);
        gotLogs = 1;
    }
    list_iterator_destroy(i);
    free(now);

    /*  If any logfile objs exist, schedule a timer for the next timestamp.
     */
    if (gotLogs)
        schedule_timestamp(conf);

    return;
}


static void create_listen_socket(server_conf_t *conf)
{
/*  Creates the socket on which to listen for client connections.
 */
    int ld;
    struct sockaddr_in addr;
    const int on = 1;

    if ((ld = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        log_err(errno, "Unable to create listening socket");

    set_fd_nonblocking(ld);
    set_fd_closed_on_exec(ld);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(conf->port);

    if (conf->enableLoopBack)
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (setsockopt(ld, SOL_SOCKET, SO_REUSEADDR,
      (const void *) &on, sizeof(on)) < 0)
        log_err(errno, "Unable to set REUSEADDR socket option");

    if (bind(ld, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        log_err(errno, "Unable to bind to port %d", conf->port);

    if (listen(ld, 10) < 0)
        log_err(errno, "Unable to listen on port %d", conf->port);

    conf->ld = ld;
    return;
}


static void open_objs(server_conf_t *conf)
{
/*  Initially opens everything in the 'objs' list.
 */
    int n;
    struct rlimit limit;
    ListIterator i;
    obj_t *obj;

    if (getrlimit(RLIMIT_NOFILE, &limit) < 0)
        log_err(errno, "Unable to get the num open file limit");
    n = MAX(limit.rlim_max, list_count(conf->objs) * 2);
    if (limit.rlim_cur < n) {
        limit.rlim_cur = limit.rlim_max = n;
        if (setrlimit(RLIMIT_NOFILE, &limit) < 0)
            log_msg(LOG_ERR, "Unable to set the num open file limit to %d", n);
        else
            log_msg(LOG_INFO, "Increased the num open file limit to %d", n);
    }
    i = list_iterator_create(conf->objs);
    while ((obj = list_next(i))) {
        if (is_serial_obj(obj))
            open_serial_obj(obj);
        else if (is_telnet_obj(obj))
            connect_telnet_obj(obj);
        else if (is_logfile_obj(obj))
            open_logfile_obj(obj, conf->enableZeroLogs);
        else
            log_err(0, "INTERNAL: Unrecognized object [%s] type=%d",
                obj->name, obj->type);
    }
    list_iterator_destroy(i);
    return;
}


static void mux_io(server_conf_t *conf)
{
/*  Multiplexes I/O between all of the objs in the configuration.
 *  This routine is the heart of ConMan.
 */
    ListIterator i;
    int n;
    obj_t *obj;

    assert(conf->tp != NULL);
    assert(!list_is_empty(conf->objs));

    i = list_iterator_create(conf->objs);

    while (!done) {

        if (reconfig) {
            /*
             *  FIXME: A reconfig should pro'ly resurrect "downed" serial objs
             *         and reset reconnect timers of "downed" telnet objs.
             */
            reopen_logfiles(conf);
            reconfig = 0;
        }

        (void) tpoll_zero(conf->tp, TPOLL_ZERO_FDS);
        tpoll_set(conf->tp, conf->ld, POLLIN);

        list_iterator_reset(i);
        while ((obj = list_next(i))) {

            if (obj->gotReset) {
                reset_console(obj, conf->resetCmd);
            }
            if (obj->fd < 0) {
                continue;
            }
            if ((is_telnet_obj(obj)
                && obj->aux.telnet.conState == CONMAN_TELCON_UP)
              || is_serial_obj(obj)
              || is_client_obj(obj)) {
                tpoll_set(conf->tp, obj->fd, POLLIN);
            }
            if (((obj->bufInPtr != obj->bufOutPtr) || (obj->gotEOF))
              && (!(is_client_obj(obj) && obj->aux.client.gotSuspend))) {
                tpoll_set(conf->tp, obj->fd, POLLOUT);
            }
            if (is_telnet_obj(obj)
              && obj->aux.telnet.conState == CONMAN_TELCON_PENDING) {
                tpoll_set(conf->tp, obj->fd, POLLIN | POLLOUT);
            }
        }
        while ((n = tpoll(conf->tp, 1000)) < 0) {
            if (errno != EINTR) {
                log_err(errno, "Unable to multiplex I/O");
            }
            else if (done || reconfig) {
                break;
            }
        }
        if (n <= 0) {
            continue;
        }
        if (tpoll_is_set(conf->tp, conf->ld, POLLIN)) {
            accept_client(conf);
        }
        /*  If read_from_obj() or write_to_obj() returns -1,
         *    the obj's buffer has been flushed.  If it is a telnet obj,
         *    retain it and attempt to re-establish the connection;
         *    o/w, give up and remove it from the master objs list.
         */
        list_iterator_reset(i);
        while ((obj = list_next(i))) {

            if (obj->fd < 0) {
                continue;
            }
            if (is_telnet_obj(obj)
              && tpoll_is_set(conf->tp, obj->fd, POLLIN | POLLOUT)
              && (obj->aux.telnet.conState == CONMAN_TELCON_PENDING)) {
                connect_telnet_obj(obj);
                continue;
            }
            if (tpoll_is_set(conf->tp, obj->fd, POLLIN | POLLHUP | POLLERR)) {
                if (read_from_obj(obj, conf->tp) < 0) {
                    list_delete(i);
                    continue;
                }
                if (obj->fd < 0) {
                    continue;
                }
            }
            if (tpoll_is_set(conf->tp, obj->fd, POLLOUT)) {
                if (write_to_obj(obj) < 0) {
                    list_delete(i);
                    continue;
                }
                if (obj->fd < 0) {
                    continue;
                }
            }
        }
    }
    list_iterator_destroy(i);
    return;
}


static void open_daemon_logfile(server_conf_t *conf)
{
/*  (Re)opens the daemon logfile.
 *  Since this logfile can be re-opened after the daemon has chdir()'d,
 *    it must be specified with an absolute pathname.
 */
    static int once = 1;
    const char *mode = "a";
    mode_t mask;
    FILE *fp;
    int fd;

    assert(conf->logFileName != NULL);
    assert(conf->logFileName[0] == '/');

    /*  Only truncate logfile at startup if needed.
     */
    if (once) {
        if (conf->enableZeroLogs)
            mode = "w";
        once = 0;
    }
    /*  Perform conversion specifier expansion if needed.
     */
    if (conf->logFmtName) {

        char buf[MAX_LINE];

        if (format_obj_string(buf, sizeof(buf), NULL, conf->logFmtName) < 0) {
            log_msg(LOG_WARNING,
                "Unable to open daemon logfile: filename too long");
            goto err;
        }
        free(conf->logFileName);
        conf->logFileName = create_string(buf);
    }
    /*  Protect logfile against unauthorized writes by removing
     *    group+other write-access from current mask.
     */
    mask = umask(0);
    umask(mask | 022);
    /*
     *  Open the logfile.
     */
    fp = fopen(conf->logFileName, mode);
    umask(mask);

    if (!fp) {
        log_msg(LOG_WARNING, "Unable to open daemon logfile \"%s\": %s",
            conf->logFileName, strerror(errno));
        goto err;
    }
    if ((fd = fileno(fp)) < 0) {
        log_msg(LOG_WARNING,
            "Unable to obtain descriptor for daemon logfile \"%s\": %s",
            conf->logFileName, strerror(errno));
        goto err;
    }
    if (get_write_lock(fd) < 0) {
        log_msg(LOG_WARNING, "Unable to lock daemon logfile \"%s\"",
            conf->logFileName);
        if (fclose(fp) == EOF)
            log_msg(LOG_WARNING, "Unable to close daemon logfile \"%s\"",
                conf->logFileName);
        goto err;
    }
    set_fd_closed_on_exec(fd);
    /*
     *  Transition to new log file.
     */
    log_set_file(fp, conf->logFileLevel, 1);
    if (conf->logFilePtr)
        if (fclose(conf->logFilePtr) == EOF)
            log_msg(LOG_WARNING, "Unable to close daemon logfile \"%s\"",
                conf->logFileName);
    conf->logFilePtr = fp;
    return;

err:
    /*  Abandon old log file and go logless.
     */
    log_set_file(NULL, 0, 0);
    if (conf->logFilePtr)
        if (fclose(conf->logFilePtr) == EOF)
            log_msg(LOG_WARNING, "Unable to close daemon logfile \"%s\"",
                conf->logFileName);
    conf->logFilePtr = NULL;
    return;
}


static void reopen_logfiles(server_conf_t *conf)
{
/*  Reopens the daemon logfile and all of the logfiles in the 'objs' list.
 */
    ListIterator i;
    obj_t *logfile;

    i = list_iterator_create(conf->objs);
    while ((logfile = list_next(i))) {
        if (!is_logfile_obj(logfile))
            continue;
        open_logfile_obj(logfile, 0);   /* do not truncate the logfile */
    }
    list_iterator_destroy(i);

    open_daemon_logfile(conf);

    return;
}


static void accept_client(server_conf_t *conf)
{
/*  Accepts a new client connection on the listening socket.
 *  The new socket connection must be accept()'d within the poll() loop.
 *    O/w, the following scenario could occur:  Read activity would be
 *    poll()'d on the listen socket.  A new thread would be created to
 *    process this request.  Before this new thread is scheduled and the
 *    socket connection is accept()'d, the poll() loop begins its next
 *    iteration.  It notices read activity on the listen socket from the
 *    client that has not yet been accepted, so a new thread is created.
 *    Since the listen socket is set non-blocking, this new thread would
 *    receive an EAGAIN/EWOULDBLOCK on the accept() and terminate, but still...
 */
    int sd;
    const int on = 1;
    client_arg_t *args;
    int rc;
    pthread_t tid;

    while ((sd = accept(conf->ld, NULL, NULL)) < 0) {
        if (errno == EINTR)
            continue;
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
            return;
        if (errno == ECONNABORTED)
            return;
        log_err(errno, "Unable to accept new connection");
    }
    DPRINTF((5, "Accepted new client on fd=%d.\n", sd));

    if (conf->enableKeepAlive) {
        if (setsockopt(sd, SOL_SOCKET, SO_KEEPALIVE,
          (const void *) &on, sizeof(on)) < 0)
            log_err(errno, "Unable to set KEEPALIVE socket option");
    }

    /*  Create a tmp struct to hold two args to pass to the thread.
     *  Note that the thread is responsible for freeing this memory.
     */
    if (!(args = malloc(sizeof(client_arg_t))))
        out_of_memory();
    args->sd = sd;
    args->conf = conf;

    if ((rc = pthread_create(&tid, NULL,
      (PthreadFunc) process_client, args)) != 0)
        log_err(rc, "Unable to create new thread");

    return;
}


static void reset_console(obj_t *console, const char *cmd)
{
/*  Resets the 'console' obj by performing the reset 'cmd' in a subshell.
 */
    char cmdbuf[MAX_LINE];
    pid_t pid;
    pid_t *arg;

    assert(is_console_obj(console));
    assert(console->gotReset);
    assert(cmd != NULL);

    DPRINTF((5, "Resetting console [%s].\n", console->name));
    console->gotReset = 0;

    if (format_obj_string(cmdbuf, sizeof(cmdbuf), console, cmd) < 0) {
        log_msg(LOG_NOTICE, "Unable to reset console [%s]: command too long",
            console->name);
        return;
    }
    if ((pid = fork()) < 0) {
        log_msg(LOG_NOTICE, "Unable to reset console [%s]: %s",
            console->name, strerror(errno));
        return;
    }
    else if (pid == 0) {
        setpgid(pid, 0);
        close(STDIN_FILENO);            /* ignore errors on close() */
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        execl("/bin/sh", "sh", "-c", cmdbuf, NULL);
        _exit(127);                     /* execl() error */
    }
    /*  Both parent and child call setpgid() to make the child a process
     *    group leader.  One of these calls is redundant, but by doing
     *    both we avoid a race condition.  (cf. APUE 9.4 p244)
     */
    setpgid(pid, 0);

    /*  Set a timer to ensure the reset cmd does not exceed its time limit.
     *  The callback function's arg must be allocated on the heap since
     *    local vars on the stack will be lost once this routine returns.
     */
    if (!(arg = malloc(sizeof *arg)))
        out_of_memory();
    *arg = pid;

    if (tpoll_timeout_relative (tp_global,
            (callback_f) kill_console_reset, arg,
            RESET_CMD_TIMEOUT * 1000) < 0) {
        log_msg(LOG_ERR,
            "Unable to create timer for resetting console [%s]",
            console->name);
    }
    return;
}


static void kill_console_reset(pid_t *arg)
{
/*  Terminates the "ResetCmd" process associated with 'arg' if it has
 *    exceeded its time limit.
 *  Memory allocated to 'arg' will be free()'d by this routine.
 */
    pid_t pid;

    assert(arg != NULL);
    pid = *arg;
    assert(pid > 0);
    free(arg);

    if (kill(pid, 0) < 0)               /* process is no longer running */
        return;
    if (kill(-pid, SIGKILL) == 0)       /* kill entire process group */
        log_msg(LOG_NOTICE, "ResetCmd process pid=%d exceeded %ds time limit",
            (int) pid, RESET_CMD_TIMEOUT);
    return;
}
