#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

static void report_child_error(int fd)
{
    int error_number = errno;
    ssize_t result;
    do
    {
        result = write(fd, &error_number, sizeof(error_number));
    } while (result < 0 && errno == EINTR);
    _exit(126);
}

static void reset_child_signals(void)
{
    signal(SIGABRT, SIG_DFL);
    signal(SIGALRM, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGFPE, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGILL, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGTRAP, SIG_DFL);
}

pid_t pty_forkpty(
    int *master,
    int *slave,
    const struct termios *termp,
    const struct winsize *winp,
    int child_error_fd)
{
    int ptm = open("/dev/ptmx", O_RDWR | O_NOCTTY);

    if (ptm < 0)
    {
        return -1;
    }

    if (fcntl(ptm, F_SETFD, FD_CLOEXEC) == -1)
    {
        int error_number = errno;
        close(ptm);
        errno = error_number;
        return -1;
    }

    if (grantpt(ptm) || unlockpt(ptm))
    {
        int error_number = errno;
        close(ptm);
        errno = error_number;
        return -1;
    }

    char *devname;

    if ((devname = ptsname(ptm)) == NULL)
    {
        int error_number = errno;
        close(ptm);
        errno = error_number;
        return -1;
    }

    int pts = open(devname, O_RDWR | O_NOCTTY);
    if (pts < 0)
    {
        int error_number = errno;
        close(ptm);
        errno = error_number;
        return -1;
    }

#ifdef IUTF8
    struct termios terminal_attributes;
    if (tcgetattr(pts, &terminal_attributes) == 0)
    {
        terminal_attributes.c_iflag |= IUTF8;
        tcsetattr(pts, TCSANOW, &terminal_attributes);
    }
#endif

    if (termp)
    {
        if (tcsetattr(pts, TCSAFLUSH, termp) == -1)
        {
            int error_number = errno;
            close(pts);
            close(ptm);
            errno = error_number;
            return -1;
        }
    }

    if (winp)
    {
        if (ioctl(pts, TIOCSWINSZ, winp) == -1)
        {
            int error_number = errno;
            close(pts);
            close(ptm);
            errno = error_number;
            return -1;
        }
    }

    pid_t pid = fork();

    if (pid < 0)
    {
        int error_number = errno;
        close(pts);
        close(ptm);
        errno = error_number;
        return -1;
    }

    if (pid == 0)
    {
        reset_child_signals();
        if (setsid() == -1) report_child_error(child_error_fd);
        if (ioctl(pts, TIOCSCTTY, (char *)NULL) == -1)
            report_child_error(child_error_fd);

        if (dup2(pts, STDIN_FILENO) == -1 ||
            dup2(pts, STDOUT_FILENO) == -1 ||
            dup2(pts, STDERR_FILENO) == -1)
        {
            report_child_error(child_error_fd);
        }

        if (pts > 2)
        {
            close(pts);
        }
        close(ptm);
    }
    else
    {
        *master = ptm;
        if (slave)
        {
            *slave = pts;
        }
        else
        {
            close(pts);
        }
    }

    return pid;
}
