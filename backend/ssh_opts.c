#include "ssh_opts.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ControlPersist keeps the master alive this many seconds after the last
 * channel closes, so back-to-back tracks from one host reuse it. */
#define SSH_CONTROL_PERSIST "30"

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

void ssh_opts_cleanup(void) {
    DIR *d;
    struct dirent *ent;
    if (!control_dir[0]) return;
    d = opendir(control_dir);
    if (d) {
        while ((ent = readdir(d))) {
            char path[512];
            if (ent->d_name[0] == '.') continue;
            if (snprintf(path, sizeof(path), "%s/%s", control_dir, ent->d_name) < (int)sizeof(path))
                unlink(path);
        }
        closedir(d);
    }
    rmdir(control_dir);
}
