#include "ssh_opts.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ControlPersist keeps the master alive this many seconds after the last
 * channel closes, so back-to-back tracks from one host reuse it.  A track's
 * transfer finishes well before the track does, so this has to outlast the
 * rest of a long track: at 30s the master was always gone by the next one,
 * which then paid a full TCP + key exchange + auth (~1s over a relayed
 * Tailscale link) before its first byte.  ssh_opts_cleanup() closes the
 * masters at shutdown, so a long persist does not leave them behind. */
#define SSH_CONTROL_PERSIST "600"

static char control_dir[256];
static char control_path_opt[320];   /* "ControlPath=<dir>/cm-%C" */
static char opts_string[512];
static int enabled;

static const char *argv_opts[6];

void ssh_opts_init(const char *ipc_dir) {
    enabled = 0;
    control_dir[0] = control_path_opt[0] = '\0';
    opts_string[0] = '\0';

    if (!ipc_dir || !ipc_dir[0]) return;
    /* The control socket path (dir + "/cm-" + 64-hex %C hash + NUL) must fit a
     * sockaddr_un, ~108 bytes.  Bail to non-multiplexed ssh if it would not. */
    if (strlen(ipc_dir) + sizeof("/ssh/cm-") + 64 > 104) return;

    if (snprintf(control_dir, sizeof(control_dir), "%s/ssh", ipc_dir) >= (int)sizeof(control_dir))
        return;
    if (mkdir(control_dir, 0700) != 0 && errno != EEXIST) { control_dir[0] = '\0'; return; }

    snprintf(control_path_opt, sizeof(control_path_opt), "ControlPath=%s/cm-%%C", control_dir);

    argv_opts[0] = "-o"; argv_opts[1] = "ControlMaster=auto";
    argv_opts[2] = "-o"; argv_opts[3] = control_path_opt;
    argv_opts[4] = "-o"; argv_opts[5] = "ControlPersist=" SSH_CONTROL_PERSIST;

    snprintf(opts_string, sizeof(opts_string),
             " -o ControlMaster=auto -o '%s' -o ControlPersist=" SSH_CONTROL_PERSIST,
             control_path_opt);
    enabled = 1;
}

const char *const *ssh_opts_argv(size_t *count) {
    if (count) *count = enabled ? 6 : 0;
    return argv_opts;
}

const char *ssh_opts_str(void) {
    return enabled ? opts_string : "";
}

/* Ask the master behind control socket `path` to exit.  Unlinking the socket
 * alone would leave the master running until ControlPersist ran out. */
static void stop_master(const char *path) {
    char opt[600];
    pid_t pid;
    if (snprintf(opt, sizeof(opt), "ControlPath=%s", path) >= (int)sizeof(opt)) return;
    pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
        execlp("ssh", "ssh", "-F", "/dev/null", "-o", opt, "-O", "exit", "leecher-master", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

void ssh_opts_cleanup(void) {
    DIR *d;
    struct dirent *ent;
    if (!control_dir[0]) return;
    d = opendir(control_dir);
    if (d) {
        while ((ent = readdir(d))) {
            char path[512];
            if (ent->d_name[0] == '.') continue;
            if (snprintf(path, sizeof(path), "%s/%s", control_dir, ent->d_name) < (int)sizeof(path)) {
                stop_master(path);
                unlink(path);
            }
        }
        closedir(d);
    }
    rmdir(control_dir);
}
