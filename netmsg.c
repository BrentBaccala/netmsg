/* Translator for S_IFLNK nodes
   Copyright (C) 1994, 2000, 2001, 2002 Free Software Foundation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include <hurd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>
#include <hurd/fsys.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <version.h>

mach_port_t realnode;

/* We return this for O_NOLINK lookups */
mach_port_t realnodenoauth;

/* We return this for non O_NOLINK lookups */
char *linktarget;

extern int fsys_server (mach_msg_header_t *, mach_msg_header_t *);

const char *argp_program_version = STANDARD_HURD_VERSION (symlink);

static const struct argp_option options[] =
  {
    { 0 }
  };

static const char args_doc[] = "TARGET";
static const char doc[] = "A translator for symlinks."
"\vA symlink is an alias for another node in the filesystem."
"\n"
"\nA symbolic link refers to its target `by name', and contains no actual"
" reference to the target.  The target referenced by the symlink is"
" looked up in the namespace of the client.";

/* Parse a single option/argument.  */
static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  if (key == ARGP_KEY_ARG && state->arg_num == 0)
    linktarget = arg;
  else if (key == ARGP_KEY_ARG || key == ARGP_KEY_NO_ARGS)
    argp_usage (state);
  else
    return ARGP_ERR_UNKNOWN;
  return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc };


int
main (int argc, char **argv)
{
  mach_port_t bootstrap;
  mach_port_t control;
  error_t err;

  /* Parse our options...  */
  argp_parse (&argp, argc, argv, 0, 0, 0);

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  linktarget = argv[1];

  /* Reply to our parent */
  mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &control);
  mach_port_insert_right (mach_task_self (), control, control,
			  MACH_MSG_TYPE_MAKE_SEND);
  err =
    fsys_startup (bootstrap, 0, control, MACH_MSG_TYPE_COPY_SEND, &realnode);
  mach_port_deallocate (mach_task_self (), control);
  mach_port_deallocate (mach_task_self (), bootstrap);
  if (err)
    error (1, err, "Starting up translator");

  io_restrict_auth (realnode, &realnodenoauth, 0, 0, 0, 0);
  mach_port_deallocate (mach_task_self (), realnode);

  /* Mark us as important.  */
  mach_port_t proc = getproc ();
  if (proc == MACH_PORT_NULL)
    error (2, err, "cannot get a handle to our process");

  err = proc_mark_important (proc);
  /* This might fail due to permissions or because the old proc server
     is still running, ignore any such errors.  */
  if (err && err != EPERM && err != EMIG_BAD_ID)
    error (2, err, "Cannot mark us as important");

  mach_port_deallocate (mach_task_self (), proc);

  mach_port_t portset;
  mach_msg_size_t max_size = 4 * __vm_page_size; /* XXX */

  /* Launch */
  while (1)
    {
      mach_msg_header_t msg;
      mach_msg_return_t mr;

      mr = mach_msg (&msg, MACH_RCV_MSG,
		     0, max_size, portset,
		     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

      /* A message has been received via IPC.  Transmit it across the network.
       *
       * If the remote queue is full, we want to remove this port from our
       * portset until space is available on the remote.
       */
    }

  /* A message has been received via the network.
   *
   * It was targeted at a remote port that corresponds to a local send right.
   *
   * If we're a server, then the very first message on a new
   * connection is targeted at a remote port that we've never seen
   * before.  It's the control port on the client/translator and it
   * maps to the root of our local filesystem (or whatever filesystem
   * object we want to present to the client).
   *
   * Otherwise, it came in on a remote receive right, and we should
   * have seen the port before when the remote got the receive right
   * and relayed it to us.  So we've got a send port to transmit the
   * message on.
   *
   * We don't want to block on the send.
   *
   * Possible port rights:
   *
   * SEND - Check to see if we've seen this remote port before.  If
   * not, create a port, hold onto its receive right, make a send
   * right, and transmit the send right on via IPC.  If so, make a new
   * send right on the existing port and send it on.
   *
   * SEND-ONCE - Always on a new name.  Create a new send-once right
   * (do we need a new receive port?) and send it on via IPC.
   *
   * RECEIVE - Check to see if we've seen this remote port before.  If
   * so, we got send rights before, so we have a receive port already.
   * Send it on via IPC.  Otherwise, create a new port, save a send
   * right for ourselves, and send the receive port on.
   */
}
