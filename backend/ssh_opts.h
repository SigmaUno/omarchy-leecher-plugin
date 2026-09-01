#ifndef SSH_OPTS_H
#define SSH_OPTS_H

#include <stddef.h>

/* Shared OpenSSH ControlMaster options so every `ssh` the app spawns (track
 * transfer, metadata, cover art, remote listing) reuses one multiplexed
 * connection per host instead of repeating the TCP + key-exchange + auth
 * handshake for each request. */

/* Creates <ipc_dir>/ssh/ (0700) and builds the option set.  Safe to call once;
 * a NULL or empty ipc_dir disables multiplexing (the accessors then yield
 * nothing, so callers behave exactly as before). */
void ssh_opts_init(const char *ipc_dir);

/* argv fragment for execvp-based callers: pairs of "-o", "<Option=value>".
 * *count is set to the element count (0 when multiplexing is disabled).  The
 * array is static and NULL-free; splice it into the ssh argument list. */
const char *const *ssh_opts_argv(size_t *count);

/* The same options as one space-joined string for the popen-based callers
 * (leading space included when non-empty).  Returns "" when disabled. */
const char *ssh_opts_str(void);

/* Best-effort removal of the control sockets and their directory. */
void ssh_opts_cleanup(void);

#endif
