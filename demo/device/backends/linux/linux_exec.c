#include "linux_exec.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void close_inherited_fds(void)
{
    long maximum = sysconf(_SC_OPEN_MAX);
    int fd;

    if (maximum < 0 || maximum > 65536)
        maximum = 65536;
    for (fd = 3; fd < maximum; ++fd)
        close(fd);
}

int linux_exec_argv(const char *const argv[], char *output, size_t output_capacity)
{
    int pipe_fd[2] = {-1, -1};
    pid_t pid;
    int status = 0;
    pid_t waited;
    size_t used = 0;

    if (argv == NULL || argv[0] == NULL || argv[0][0] == '\0')
        return -1;
    if (output != NULL && output_capacity == 0)
        output = NULL;
    if (output != NULL && pipe(pipe_fd) != 0)
        return -1;

    pid = fork();
    if (pid == 0) {
        if (output != NULL) {
            dup2(pipe_fd[1], STDOUT_FILENO);
        }
        if (pipe_fd[0] >= 0)
            close(pipe_fd[0]);
        if (pipe_fd[1] >= 0)
            close(pipe_fd[1]);
        close_inherited_fds();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (output != NULL)
        close(pipe_fd[1]);
    if (pid < 0) {
        if (pipe_fd[0] >= 0)
            close(pipe_fd[0]);
        return -1;
    }
    if (output != NULL) {
        while (used + 1 < output_capacity) {
            ssize_t size = read(pipe_fd[0], output + used,
                                output_capacity - used - 1);
            if (size > 0)
                used += (size_t)size;
            else if (size == 0)
                break;
            else if (errno != EINTR)
                break;
        }
        close(pipe_fd[0]);
        output[used] = '\0';
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        return -1;
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}
