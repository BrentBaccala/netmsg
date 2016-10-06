/* -*- mode: C; indent-tabs-mode: nil -*-

   fprintf-test

   This is a test program that causes fprintf to return -1,
   with errno EINTR.

   Usage:

   start translator:  settrans -ac fprintf-test-node fprintf-test
   run test:          fprintf-test fprintf-test-node
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <argp.h>
#include <assert.h>

#include <mach/message.h>
#include <mach/mach_port.h>

#include <mach/notify.h>
#include <mach_error.h>

#include <hurd/trivfs.h>
#include <hurd/hurd_types.h>

#include "netmsg-test-server.h"
#include "netmsg-test-user.h"

#include <hurd.h>

kern_return_t
S_test1(mach_port_t server, mach_port_t testport, int count, boolean_t destroy, boolean_t transfer, boolean_t copy)
{
  const static mach_msg_size_t max_size = 4096;
  char buffer[max_size];
  mach_msg_header_t * const msg = (mach_msg_header_t *) (buffer);

  /* Create a new receive right */
  mach_port_t testport2;

  mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &testport2);

  /* This call should pause 1 second, then return MACH_RCV_TIMED_OUT */

  kern_return_t kr = mach_msg (msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                               0, max_size, testport2,
                               1000, MACH_PORT_NULL);

  while (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, mach_error_string(kr)) == -1)
    {
      fprintf(stderr, "fprintf returned -1 (%s)\n", strerror(errno));
    }

  return ESUCCESS;
}

kern_return_t
S_test2(mach_port_t server, mach_port_t testport, int count, boolean_t request_no_senders, mach_port_t returnport)
{
  return ESUCCESS;
}

kern_return_t
S_test11(mach_port_t server, mach_port_t testport)
{
  return ESUCCESS;
}

/* test3 - create a send/receive pair, transfer the send right,
 *   transmit some messages to it, then destroy the receive right and
 *   get a DEAD NAME notification on the send right
 */

void
test3(mach_port_t node)
{

  U_test1(node, MACH_PORT_NULL, MACH_MSG_TYPE_MAKE_SEND, 3, FALSE, FALSE, FALSE);

}

/* trivfs stuff */

int trivfs_fstype = FSTYPE_MISC;
int trivfs_fsid = 0;               /* 0 = use translator pid as filesystem id */
int trivfs_allow_open = O_RDWR;

int trivfs_support_read = 0;
int trivfs_support_write = 0;
int trivfs_support_execute = 0;

void trivfs_modify_stat (struct trivfs_protid *CRED, io_statbuf_t *STBUF)
{
}

error_t trivfs_goaway (struct trivfs_control *CNTL, int FLAGS)
{
  exit(0);
  // return ESUCCESS;
}


/* XXX why isn't netmsg_test_server() declared in netmsg-test-server.h? */

boolean_t netmsg_test_server (mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP);

int netmsg_test_demuxer (mach_msg_header_t *in, mach_msg_header_t *out)
{

  if (trivfs_demuxer (in, out) || netmsg_test_server(in, out))
    {
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}


void
startAsTranslator(void)
{
  mach_port_t bootstrap;
  trivfs_control_t fsys;

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (1, 0, "Must be started as a translator");

  trivfs_startup(bootstrap, O_RDWR, NULL, NULL, NULL, NULL, &fsys);

  ports_manage_port_operations_multithread (fsys->pi.bucket, netmsg_test_demuxer, 0, 0, NULL);
}

/* MAIN ROUTINE */

int
main (int argc, char **argv)
{
  const char * targetPath = argv[1];

  if (targetPath == NULL)
    {
      startAsTranslator();
    }
  else
    {
      mach_port_t node = file_name_lookup (targetPath, O_RDWR, 0);

      test3(node);
    }
}
