#ifndef SSH_OPTS_H
#define SSH_OPTS_H

#include <stddef.h>

/* Connection-hardening options applied to every `ssh` the app spawns, so a
 * dead or filtered host fails in ~5s (not the multi-minute kernel TCP timeout)
 * and a mid-transfer network drop is noticed within ~15s. Two spellings:
 *   SSH_HARDENING_OPTS_STR   -- one string for the popen()/snprintf builders
 *                               (leads with a space, no trailing space)
 *   SSH_HARDENING_OPTS_ARGV  -- "-o","K=V" pairs for the execvp() builders
 *                               (SSH_HARDENING_OPTS_ARGV_COUNT elements) */
#define SSH_HARDENING_OPTS_STR \
    " -o BatchMode=yes -o RequestTTY=no -o ClearAllForwardings=yes" \
    " -o LogLevel=ERROR -o ConnectTimeout=5" \
    " -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
#define SSH_HARDENING_OPTS_ARGV \
    "-o", "BatchMode=yes", "-o", "RequestTTY=no", \
    "-o", "ClearAllForwardings=yes", "-o", "LogLevel=ERROR", \
    "-o", "ConnectTimeout=5", \
    "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3"
#define SSH_HARDENING_OPTS_ARGV_COUNT 14

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
