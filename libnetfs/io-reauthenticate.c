/* 
   Copyright (C) 1995, 1996 Free Software Foundation, Inc.
   Written by Michael I. Bushnell, p/BSG.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include "netfs.h"
#include "io_S.h"

error_t
netfs_S_io_reauthenticate (struct protid *user, mach_port_t rend_port)
{
  struct protid *newpi;
  error_t err;
  mach_port_t newright;
  
  if (!user)
    return EOPNOTSUPP;
  
  mutex_lock (&user->po->np->lock);
  newpi = netfs_make_protid (user->po, 0);

  newright = ports_get_right (newpi);
  err = mach_port_insert_right (mach_task_self (), newright, newright,
				MACH_MSG_TYPE_MAKE_SEND);
  assert_perror (err);

  newpi->user = iohelp_reauth (netfs_auth_server_port, rend_port, newright, 1);

  mach_port_deallocate (mach_task_self (), rend_port);
  mach_port_deallocate (mach_task_self (), newright);

  mach_port_move_member (mach_task_self (), newpi->pi.port_right,
			 netfs_port_bucket->portset);

  mutex_unlock (&user->po->np->lock);
  ports_port_deref (newpi);

  return 0;
}
