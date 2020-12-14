/*
Based on the utility routines for Twopence.

Copyright (C) 2014-2015 SUSE

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef __APPLE__
#include <malloc.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>

#include "twopence.h"
#include "utils.h"

int
twopence_plugin_type(const char *plugin_name)
{
  if (!strcmp(plugin_name, "virtio"))
    return TWOPENCE_PLUGIN_VIRTIO;
  if (!strcmp(plugin_name, "ssh"))
    return TWOPENCE_PLUGIN_SSH;
  if (!strcmp(plugin_name, "serial"))
    return TWOPENCE_PLUGIN_SERIAL;
  if (!strcmp(plugin_name, "tcp"))
    return TWOPENCE_PLUGIN_TCP;
  if (!strcmp(plugin_name, "chroot"))
    return TWOPENCE_PLUGIN_CHROOT;
  if (!strcmp(plugin_name, "local"))
    return TWOPENCE_PLUGIN_LOCAL;

  return TWOPENCE_PLUGIN_UNKNOWN;
}

bool
twopence_plugin_name_is_valid(const char *name)
{
  /* For the time being, we only recognize built-in plugin names.
   * That is not really the point of a pluggable architecture, though -
   * it's supposed to allow plugging in functionality that we weren't
   * aware of at originally...
   * Well, whatever :-)
   */
  return twopence_plugin_type(name) != TWOPENCE_PLUGIN_UNKNOWN;
}

/*
 * Split the target, which is of the form "plugin:specstring" into its
 * two components.
 */
static char *
twopence_target_split(char **target_spec_p)
{
  char *plugin;
  unsigned int len;

  if (target_spec_p == NULL || (plugin = *target_spec_p) == NULL)
    return NULL;

  len = strcspn(plugin, ":");
  if (len == 0) {
    *target_spec_p = NULL;
    return plugin;
  }

  /* NUL terminate the plugin string */
  if (plugin[len] != '\0') {
    plugin[len++] = '\0';
    *target_spec_p = plugin + len;
  } else {
    *target_spec_p = NULL;
  }

  if (!twopence_plugin_name_is_valid(plugin))
    return NULL;

  return plugin;
}

static int
__twopence_get_plugin_ops(const char *name, const struct twopence_plugin **ret)
{
  static const struct twopence_plugin *plugins[__TWOPENCE_PLUGIN_MAX] = {
  [TWOPENCE_PLUGIN_VIRTIO]	= &twopence_virtio_ops,
  [TWOPENCE_PLUGIN_SERIAL]	= &twopence_serial_ops,
  [TWOPENCE_PLUGIN_SSH]		= &twopence_ssh_ops,
  [TWOPENCE_PLUGIN_TCP]		= &twopence_tcp_ops,
  [TWOPENCE_PLUGIN_CHROOT]	= &twopence_chroot_ops,
  [TWOPENCE_PLUGIN_LOCAL]	= &twopence_local_ops,
  };
  int type;

  type = twopence_plugin_type(name);
  if (type < 0 || type >= __TWOPENCE_PLUGIN_MAX)
    return TWOPENCE_UNKNOWN_PLUGIN_ERROR;

  *ret = plugins[type];
  if (*ret == NULL)
    return TWOPENCE_UNKNOWN_PLUGIN_ERROR;

  return 0;
}

static int
__twopence_target_new(char *target_spec, struct twopence_target **ret)
{
  const struct twopence_plugin *plugin;
  struct twopence_target *target;
  char *name;
  int rc;

  name = twopence_target_split(&target_spec);
  if (name == NULL)
    return TWOPENCE_INVALID_TARGET_ERROR;

  rc = __twopence_get_plugin_ops(name, &plugin);
  if (rc < 0)
    return rc;

  /* FIXME: check a version number provided by the plugin data */

  if (plugin->init == NULL)
    return TWOPENCE_INCOMPATIBLE_PLUGIN_ERROR;

  /* Create the handle */
  target = plugin->init(target_spec);
  if (target == NULL)
    return TWOPENCE_UNKNOWN_PLUGIN_ERROR;

  *ret = target;
  return 0;
}

int
twopence_target_new(const char *target_spec, struct twopence_target **ret)
{
  char *spec_copy;
  int rv;

  spec_copy = twopence_strdup(target_spec);
  rv = __twopence_target_new(spec_copy, ret);
  free(spec_copy);

  return rv;
}

void
twopence_target_free(struct twopence_target *target)
{
  if (target->ops->end == NULL) {
    free(target);
  } else {
    target->ops->end(target);
  }
}

/*
 * Set target specific options
 */
int
twopence_target_set_option(struct twopence_target *target, int option, const void *value_p)
{
  if (target->ops->set_option == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  if (value_p == NULL)
    return TWOPENCE_PARAMETER_ERROR;

  return target->ops->set_option(target, option, value_p);
}

/*
 * Manipulate the default environment of a target.
 * This default environment is passed to every command execution.
 * Note: any environment variables defined in a command object
 * take precedence of variables set in the targe's default env.
 */
void
twopence_target_setenv(twopence_target_t *target, const char *name, const char *value)
{
	twopence_env_set(&target->env, name, value);
}

void
twopence_target_passenv(twopence_target_t *target, const char *name)
{
	twopence_env_unset(&target->env, name);
}

/*
 * General API
 */
int
twopence_run_test(struct twopence_target *target, twopence_command_t *cmd, twopence_status_t *status)
{
  memset(status, 0, sizeof(*status));

  if (target->ops->run_test == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  /* Populate defaults. Instead of hard-coding them, we could also set
   * default values for a given target. */
  if (cmd->timeout == 0)
    cmd->timeout = 60;
  if (cmd->user == NULL)
    cmd->user = "root";

  twopence_command_merge_default_env(cmd, &target->env);

  return target->ops->run_test(target, cmd, status);
}

int
twopence_wait(struct twopence_target *target, int pid, twopence_status_t *status)
{
  memset(status, 0, sizeof(*status));

  if (target->ops->wait == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  return target->ops->wait(target, pid, status);
}

/*
 * Chat script support
 */
void
twopence_chat_init(twopence_chat_t *chat, twopence_buf_t *sendbuf, twopence_buf_t *recvbuf)
{
  memset(chat, 0, sizeof(*chat));
  chat->sendbuf = sendbuf;
  chat->recvbuf = recvbuf;
  twopence_buf_init(&chat->consumed);
}

void
twopence_chat_destroy(twopence_chat_t *chat)
{
  twopence_buf_destroy(&chat->consumed);
  twopence_strfree(&chat->found);
}

int
twopence_chat_begin(twopence_target_t *target, twopence_command_t *cmd, twopence_chat_t *chat)
{
  twopence_status_t status;
  int rv;

  if (chat->recvbuf == NULL || chat->sendbuf == NULL)
    return TWOPENCE_PARAMETER_ERROR;

  if (target->ops->chat_recv == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  cmd->keepopen_stdin = true;
  cmd->background = true;
  cmd->request_tty = true;

  /* Reset stdandard IO channels. stdin is connected to our sendbuf,
   * and stderr and stdout both go to our recvbuf */
  twopence_command_ostreams_reset(cmd);
  twopence_command_ostream_capture(cmd, TWOPENCE_STDOUT, chat->recvbuf);
  twopence_command_ostream_capture(cmd, TWOPENCE_STDERR, chat->recvbuf);

  chat->stdin = &cmd->iostream[TWOPENCE_STDIN];

  rv = twopence_run_test(target, cmd, &status);
  if (rv < 0)
    return rv;

  if (rv == 0) {
    fprintf(stderr, "%s: received pid 0 when starting command\n", __func__);
    return TWOPENCE_SEND_COMMAND_ERROR;
  }

  chat->pid = rv;

  return chat->pid;
}

/*
 * Wait for the remote command to output the expected string.
 *
 * If timeout is non-negative, it tells us how many seconds to wait at most.
 * If the string is not received within this amount of time, the backend's chat()
 * function will return TWOPENCE_COMMAND_TIMEOUT_ERROR.
 */
int
twopence_chat_expect(twopence_target_t *target, twopence_chat_t *chat, const twopence_expect_t *args)
{
  struct timeval __deadline, *deadline;
  twopence_buf_t *bp = chat->recvbuf;

  twopence_buf_destroy(&chat->consumed);
  twopence_strfree(&chat->found);

  deadline = NULL;
  if (args->timeout >= 0) {
    gettimeofday(&__deadline, NULL);
    __deadline.tv_sec += args->timeout;
    deadline = &__deadline;
  }

  while (true) {
    const char *string = NULL;
    int k, nbytes, pos;

    for (k = 0, pos = -1; k < args->nstrings; ++k) {
      const char *s = args->strings[k];
      int at;

      at = twopence_buf_index(bp, s);
      if (at >= 0 && (pos < 0 || at < pos || (at == pos && strlen(s) > strlen(string)))) {
	string = s;
	pos = at;
      }
    }

    if (pos >= 0) {
      /* Consume everything up to and including the string we waited for.
       * We return the data we skipped over in chat->consumed.
       */
      chat->found = twopence_strdup(string);

      nbytes = pos + strlen(string);
      twopence_buf_ensure_tailroom(&chat->consumed, nbytes);
      twopence_buf_append(&chat->consumed, twopence_buf_head(bp), nbytes);
      twopence_buf_pull(bp, nbytes);
      return nbytes;
    }

    nbytes = target->ops->chat_recv(target, chat->pid, deadline);
    if (nbytes <= 0) {
      /* There are a number of reasons for getting here:
       *  - command exited without producing further output (nbytes is 0 in this case)
       *  - command closed its stderr and stdout (nbytes is 0 in this case as well)
       *  - timed out waiting for output (TWOPENCE_COMMAND_TIMEOUT_ERROR)
       *  - transaction failed for some reason (nbytes < 0)
       *  - transport errors (nbytes < 0)
       */
      return nbytes;
    }
  }
}

/*
 * Write a string to the remote command's stdin
 */
void
twopence_chat_puts(twopence_target_t *target, twopence_chat_t *chat, const char *string)
{
  twopence_iostream_t *stream = chat->stdin;

  /* Append the string to the send buffer */
  twopence_buf_ensure_tailroom(chat->sendbuf, strlen(string));
  twopence_buf_append(chat->sendbuf, string, strlen(string));

  /* Rebuild the stdin stream */
  twopence_iostream_destroy(stream);
  twopence_iostream_add_substream(stream, twopence_substream_new_buffer(chat->sendbuf, false));

  /* And inform the backend about the fact that we've added data and want it sent */
  if (target->ops->chat_send)
    target->ops->chat_send(target, chat->pid, stream);
}

/*
 * Read a string from the remote output, up to the next newline.
 * This tries to mimick the behavior of fgets()
 */
char *
twopence_chat_gets(twopence_target_t *target, twopence_chat_t *chat, char *buf, size_t size, int timeout)
{
  twopence_buf_t *bp = chat->recvbuf;
  unsigned int i, j, count;
  const char *data;

  if (size == 0)
    return NULL;

  count = twopence_buf_count(bp);
  if (size - 1 < count) {
    /* We already have more data than we can swallow */
    count = size - 1;
  } else
  if (twopence_buf_index(bp, "\n") < 0) {
    struct timeval __deadline, *deadline;

    /* We don't have a complete line yet, so wait for input */
    deadline = NULL;
    if (timeout >= 0) {
      gettimeofday(&__deadline, NULL);
      __deadline.tv_sec += timeout;
      deadline = &__deadline;
    }

    while (twopence_buf_index(bp, "\n") < 0) {
      int nbytes;

      nbytes = target->ops->chat_recv(target, chat->pid, deadline);

      /*
       *  - command exited without producing further output (nbytes is 0 in this case)
       *  - command closed its stderr and stdout (nbytes is 0 in this case as well)
       *  - timed out waiting for output (TWOPENCE_COMMAND_TIMEOUT_ERROR)
       *  - transaction failed for some reason (nbytes < 0)
       *  - transport errors (nbytes < 0)
       */
      if (nbytes < 0)
        return NULL;
      if (nbytes == 0)
	break;

      /* Continue and re-check whether we have a newline now */
    }

    count = twopence_buf_count(bp);
    if (size - 1 < count)
      count = size - 1;
  }

  /* Now we either have a newline, or the remote command stopped
   * producing output (by exiting or by closing its stdout channels) */
  data = twopence_buf_head(bp);
  for (i = j = 0; i < count; ) {
    char cc = data[i++];

    /* Collapse CRLF into LF */
    if (cc == '\r' && i < count && data[i] == '\n')
      cc = data[i++];

    if (cc == '\0' || cc == '\n')
      break;
    buf[j++] = cc;
  }

  twopence_buf_pull(bp, i);
  buf[j] = '\0';
  return buf;
}

int
twopence_test_and_print_results
  (struct twopence_target *target, const char *username, long timeout, const char *command, twopence_status_t *status)
{
  if (target->ops->run_test) {
    twopence_command_t cmd;

    twopence_command_init(&cmd, command);
    cmd.user = username;
    cmd.timeout = timeout;

    twopence_command_ostreams_reset(&cmd);
    twopence_command_iostream_redirect(&cmd, TWOPENCE_STDIN, 0, false);
    twopence_command_iostream_redirect(&cmd, TWOPENCE_STDOUT, 1, false);
    twopence_command_iostream_redirect(&cmd, TWOPENCE_STDERR, 2, false);

    return twopence_run_test(target, &cmd, status);
  }

  return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;
}

int
twopence_test_and_drop_results
  (struct twopence_target *target, const char *username, long timeout, const char *command, twopence_status_t *status)
{
  if (target->ops->run_test) {
    twopence_command_t cmd;

    twopence_command_init(&cmd, command);
    cmd.user = username;
    cmd.timeout = timeout;

    /* Reset both ostreams to nothing */
    twopence_command_ostreams_reset(&cmd);
    twopence_command_iostream_redirect(&cmd, TWOPENCE_STDIN, 0, false);

    return twopence_run_test(target, &cmd, status);
  }

  return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;
}

int
twopence_test_and_store_results_together
  (struct twopence_target *target, const char *username, long timeout, const char *command,
   twopence_buf_t *buffer, twopence_status_t *status)
{
  if (target->ops->run_test) {
    twopence_command_t cmd;

    twopence_command_init(&cmd, command);
    cmd.user = username;
    cmd.timeout = timeout;

    twopence_command_ostreams_reset(&cmd);
    twopence_command_iostream_redirect(&cmd, TWOPENCE_STDIN, 0, false);
    if (buffer) {
      twopence_command_ostream_capture(&cmd, TWOPENCE_STDOUT, buffer);
      twopence_command_ostream_capture(&cmd, TWOPENCE_STDERR, buffer);
    }

    return twopence_run_test(target, &cmd, status);
  }

  return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;
}

int
twopence_test_and_store_results_separately
  (struct twopence_target *target, const char *username, long timeout, const char *command,
   twopence_buf_t *stdout_buffer, twopence_buf_t *stderr_buffer, twopence_status_t *status)
{
  if (target->ops->run_test) {
    twopence_command_t cmd;

    twopence_command_init(&cmd, command);
    cmd.user = username;
    cmd.timeout = timeout;

    twopence_command_ostreams_reset(&cmd);
    twopence_command_iostream_redirect(&cmd, TWOPENCE_STDIN, 0, false);
    if (stdout_buffer)
      twopence_command_ostream_capture(&cmd, TWOPENCE_STDOUT, stdout_buffer);
    if (stderr_buffer)
      twopence_command_ostream_capture(&cmd, TWOPENCE_STDERR, stderr_buffer);

    return twopence_run_test(target, &cmd, status);
  }

  return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;
}

int
twopence_inject_file
  (struct twopence_target *target, const char *username,
   const char *local_path, const char *remote_path,
   int *remote_rc, bool print_dots)
{
  twopence_status_t status;
  twopence_file_xfer_t xfer;
  int rv;

  twopence_file_xfer_init(&xfer);

  /* Open the file */
  rv = twopence_iostream_open_file(local_path, &xfer.local_stream);
  if (rv < 0)
    return rv;

  xfer.user = username;
  xfer.remote.name = remote_path;
  xfer.remote.mode = 0660;
  xfer.print_dots = print_dots;

  rv = twopence_send_file(target, &xfer, &status);
  *remote_rc = status.major;

  twopence_file_xfer_destroy(&xfer);
  return rv;
}

int
twopence_send_file(struct twopence_target *target, twopence_file_xfer_t *xfer, twopence_status_t *status)
{
  memset(status, 0, sizeof(*status));

  if (target->ops->inject_file == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  if (xfer->local_stream == NULL)
    return TWOPENCE_PARAMETER_ERROR;

  if (xfer->user == NULL)
    xfer->user = "root";
  if (xfer->remote.mode == 0)
    xfer->remote.mode = 0644;

  return target->ops->inject_file(target, xfer, status);
}

int
twopence_extract_file
  (struct twopence_target *target, const char *username,
   const char *remote_path, const char *local_path,
   int *remote_rc, bool print_dots)
{
  twopence_status_t status;
  twopence_file_xfer_t xfer;
  int rv;

  twopence_file_xfer_init(&xfer);

  /* Open the file */
  rv = twopence_iostream_create_file(local_path, 0666, &xfer.local_stream);
  if (rv < 0)
    return rv;

  xfer.user = username;
  xfer.remote.name = remote_path;
  xfer.remote.mode = 0660;
  xfer.print_dots = print_dots;

  rv = twopence_recv_file(target, &xfer, &status);
  *remote_rc = status.major;

  twopence_file_xfer_destroy(&xfer);
  return rv;
}

int
twopence_recv_file(struct twopence_target *target, twopence_file_xfer_t *xfer, twopence_status_t *status)
{
  memset(status, 0, sizeof(*status));

  if (target->ops->inject_file == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  if (xfer->local_stream == NULL)
    return TWOPENCE_PARAMETER_ERROR;

  if (xfer->user == NULL)
    xfer->user = "root";
  if (xfer->remote.mode == 0)
    xfer->remote.mode = 0644;

  return target->ops->extract_file(target, xfer, status);
}

int
twopence_exit_remote(struct twopence_target *target)
{
  if (target->ops->exit_remote == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  return target->ops->exit_remote(target);
}

int
twopence_cancel_transactions(twopence_target_t *target)
{
  if (target->ops->cancel_transactions == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  return target->ops->cancel_transactions(target);
}

int
twopence_disconnect(twopence_target_t *target)
{
  if (target->ops->disconnect == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  return target->ops->disconnect(target);
}

int
twopence_interrupt_command(struct twopence_target *target)
{
  if (target->ops->interrupt_command == NULL)
    return TWOPENCE_UNSUPPORTED_FUNCTION_ERROR;

  return target->ops->interrupt_command(target);
}


/*
 * Convert twopence error code to string message
 */
const char *
twopence_strerror(int rc)
{
  switch (rc) {
    case TWOPENCE_PARAMETER_ERROR:
      return "Invalid command parameter";
    case TWOPENCE_OPEN_SESSION_ERROR:
      return "Error opening the communication with the system under test";
    case TWOPENCE_SEND_COMMAND_ERROR:
      return "Error sending command to the system under test";
    case TWOPENCE_FORWARD_INPUT_ERROR:
      return "Error forwarding keyboard input";
    case TWOPENCE_RECEIVE_RESULTS_ERROR:
      return "Error receiving the results of action";
    case TWOPENCE_COMMAND_TIMEOUT_ERROR:
      return "Remote command took too long to execute";
    case TWOPENCE_LOCAL_FILE_ERROR:
      return "Local error while transferring file";
    case TWOPENCE_SEND_FILE_ERROR:
      return "Error sending file to the system under test";
    case TWOPENCE_REMOTE_FILE_ERROR:
      return "Remote error while transferring file";
    case TWOPENCE_RECEIVE_FILE_ERROR:
      return "Error receiving file from the system under test";
    case TWOPENCE_INTERRUPT_COMMAND_ERROR:
      return "Failed to interrupt command";
    case TWOPENCE_INVALID_TARGET_ERROR:
      return "Invalid target specification";
    case TWOPENCE_UNKNOWN_PLUGIN_ERROR:
      return "Unknown plugin";
    case TWOPENCE_INCOMPATIBLE_PLUGIN_ERROR:
      return "Incompatible plugin";
    case TWOPENCE_UNSUPPORTED_FUNCTION_ERROR:
      return "Operation not supported by the plugin";
    case TWOPENCE_PROTOCOL_ERROR:
      return "Twopence custom protocol error";
    case TWOPENCE_INTERNAL_ERROR:
      return "Internal error";
    case TWOPENCE_TRANSPORT_ERROR:
      return "Error sending or receiving data on socket";
    case TWOPENCE_INCOMPATIBLE_PROTOCOL_ERROR:
      return "Protocol versions not compatible between client and server";
    case TWOPENCE_INVALID_TRANSACTION:
      return "Invalid transaction ID";
    case TWOPENCE_COMMAND_CANCELED_ERROR:
      return "Command canceled by user";
  }
  return "Unknow error";
}

void
twopence_perror(const char *msg, int rc)
{
  fprintf(stderr, "%s: %s.\n", msg, twopence_strerror(rc));
}

/*
 * Handling of command structs
 */
void
twopence_command_init(twopence_command_t *cmd, const char *cmdline)
{
  memset(cmd, 0, sizeof(*cmd));

  /* By default, all output from the remote command is sent to our own
   * stdout and stderr.
   * The input of the remote command is not connected.
   */
  twopence_command_iostream_redirect(cmd, TWOPENCE_STDOUT, 1, false);
  twopence_command_iostream_redirect(cmd, TWOPENCE_STDERR, 2, false);

  cmd->command = cmdline;
}

static inline twopence_buf_t *
__twopence_command_buffer(twopence_command_t *cmd, twopence_iofd_t dst)
{
  if (0 <= dst && dst < __TWOPENCE_IO_MAX)
    return &cmd->buffer[dst];
  return NULL;
}

twopence_buf_t *
twopence_command_alloc_buffer(twopence_command_t *cmd, twopence_iofd_t dst, size_t size)
{
  twopence_buf_t *bp;

  if ((bp = __twopence_command_buffer(cmd, dst)) == NULL)
    return NULL;

  twopence_buf_destroy(bp);
  if (size)
    twopence_buf_resize(bp, size);
  return bp;
}

static inline twopence_iostream_t *
__twopence_command_ostream(twopence_command_t *cmd, twopence_iofd_t dst)
{
  if (0 <= dst && dst < __TWOPENCE_IO_MAX)
    return &cmd->iostream[dst];
  return NULL;
}

void
twopence_command_ostreams_reset(twopence_command_t *cmd)
{
  unsigned int i;

  for (i = 0; i < __TWOPENCE_IO_MAX; ++i)
    twopence_iostream_destroy(&cmd->iostream[i]);
}

void
twopence_command_ostream_reset(twopence_command_t *cmd, twopence_iofd_t dst)
{
  twopence_iostream_t *stream;

  if ((stream = __twopence_command_ostream(cmd, dst)) != NULL)
    twopence_iostream_destroy(stream);
}

void
twopence_command_ostream_capture(twopence_command_t *cmd, twopence_iofd_t dst, twopence_buf_t *bp)
{
  twopence_iostream_t *stream;

  if ((stream = __twopence_command_ostream(cmd, dst)) != NULL)
    twopence_iostream_add_substream(stream, twopence_substream_new_buffer(bp, false));
}

void
twopence_command_iostream_redirect(twopence_command_t *cmd, twopence_iofd_t dst, int fd, bool closeit)
{
  twopence_iostream_t *stream;

  if ((stream = __twopence_command_ostream(cmd, dst)) != NULL)
    twopence_iostream_add_substream(stream, twopence_substream_new_fd(fd, closeit));
}

void
twopence_command_setenv(twopence_command_t *cmd, const char *name, const char *value)
{
	twopence_env_set(&cmd->env, name, value);
}

void
twopence_command_passenv(twopence_command_t *cmd, const char *name)
{
	twopence_env_unset(&cmd->env, name);
}

void
twopence_command_merge_default_env(twopence_command_t *cmd, const twopence_env_t *def_env)
{
	twopence_env_merge_inferior(&cmd->env, def_env);
}

void
twopence_command_destroy(twopence_command_t *cmd)
{
  unsigned int i;

  for (i = 0; i < __TWOPENCE_IO_MAX; ++i) {
    twopence_buf_destroy(&cmd->buffer[i]);
    twopence_iostream_destroy(&cmd->iostream[i]);
  }
  twopence_env_destroy(&cmd->env);
}

/*
 * Environment handling functions
 */
void
twopence_env_init(twopence_env_t *env)
{
	memset(env, 0, sizeof(*env));
}

static void
__twopence_env_append(twopence_env_t *env, const char *var)
{
	env->array = twopence_realloc(env->array, (env->count + 2) * sizeof(env->array[0]));
	env->array[env->count++] = twopence_strdup(var);
	env->array[env->count] = NULL;
}

static char *
__twopence_env_get(const twopence_env_t *env, const char *name, unsigned int *pos)
{
	unsigned int len;
	unsigned int i;

	if (!name || !name[0])
		return NULL;
	len = strlen(name);

	for (i = 0; i < env->count; ++i) {
		char *var = env->array[i];

		if (!strncmp(var, name, len) && var[len] == '=') {
			if (pos)
				*pos = i;
			return var + len + 1;
		}
	}
	return NULL;
}

void
twopence_env_set(twopence_env_t *env, const char *name, const char *value)
{
	if (value == NULL) {
		twopence_env_unset(env, name);
	} else {
		char buffer[1024];

		snprintf(buffer, sizeof(buffer), "%s=%s", name, value);
		__twopence_env_append(env, buffer);
	}
}

void
twopence_env_unset(twopence_env_t *env, const char *name)
{
	unsigned int pos, i;

	while (__twopence_env_get(env, name, &pos) != NULL) {
		free(env->array[pos]);
		for (i = pos + 1; i < env->count; )
			env->array[pos++] = env->array[i++];
		env->array[pos] = NULL;
		env->count = pos;
	}
}

void
twopence_env_pass(twopence_env_t *env, const char *name)
{
	twopence_env_set(env, name, getenv(name));
}

/*
 * Copy an environment
 */
extern void
twopence_env_copy(twopence_env_t *env, const twopence_env_t *src_env)
{
	unsigned int i;

	twopence_env_destroy(env);
	for (i = 0; i < src_env->count; ++i)
		__twopence_env_append(env, src_env->array[i]);
}

/*
 * Merge a default environment to a command environment
 */
void
twopence_env_merge_inferior(twopence_env_t *env, const twopence_env_t *def_env)
{
	unsigned int i, j;

	for (i = 0; i < def_env->count; ++i) {
		char *var = def_env->array[i];
		unsigned int len;
		bool found = false;

		len = strcspn(var, "=") + 1;
		for (j = 0; j < env->count && !found; ++j) {
			if (!strncmp(env->array[j], var, len))
				found = true;
		}
		if (!found)
			__twopence_env_append(env, var);
	}
}

void
twopence_env_destroy(twopence_env_t *env)
{
	unsigned int i;

	for (i = 0; i < env->count; ++i)
		free(env->array[i]);
	free(env->array);
	memset(env, 0, sizeof(*env));
}

/*
 * File transfer object
 */
void
twopence_file_xfer_init(twopence_file_xfer_t *xfer)
{
  memset(xfer, 0, sizeof(*xfer));
  xfer->remote.mode = 0640;
}

void
twopence_file_xfer_destroy(twopence_file_xfer_t *xfer)
{
  if (xfer->local_stream) {
    twopence_iostream_free(xfer->local_stream);
    xfer->local_stream = NULL;
  }
}
