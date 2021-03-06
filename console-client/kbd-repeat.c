/* kbd-repeat.c - Keyboard repeater.
   Copyright (C) 2004, 2005 Free Software Foundation, Inc.
   Written by Marco Gerards.

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

#include <hurd/netfs.h>
#include <stdlib.h>
#include <error.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "kdioctl_S.h"
#include "mach-inputdev.h"
#include "input.h"

/* The amount of keyboard events that can be stored in the keyboard buffer.  */
#define KBDEVTBUFSZ	20

/* The size of the keyboard buffer in bytes.  */
#define KBDBUFSZ	(KBDEVTBUFSZ * sizeof (kd_event))

/* Return the position of X in the buffer.  */
#define KBDBUF_POS(x)	((x) % KBDBUFSZ)

/* The keyboard buffer.  */
static struct kbdbuf
{
  char keybuffer[KBDBUFSZ];
  int pos;
  size_t size;
  struct condition readcond;
  struct condition writecond;
} kbdbuf;

/* Wakeup for select */
static struct condition select_alert;

/* The global lock */
static struct mutex global_lock;

/* Amount of times the device was opened.  Normally this translator
   should be only opened once.  */ 
int kbd_repeater_opened;


/* Place the keyboard event KEY in the keyboard buffer.  */
void
kbd_repeat_key (kd_event *key)
{
  kd_event *ev;

  mutex_lock (&global_lock);
  while (kbdbuf.size + sizeof (kd_event) > KBDBUFSZ)
    {
      /* The input buffer is full, wait until there is some space.  */
      if (hurd_condition_wait (&kbdbuf.writecond, &global_lock))
	{
	  mutex_unlock (&global_lock);
	  /* Interrupt, silently continue.  */
	}
    }
  ev = (kd_event *) &kbdbuf.keybuffer[KBDBUF_POS (kbdbuf.pos 
						  + kbdbuf.size)];
  kbdbuf.size += sizeof (kd_event);
  memcpy (ev, key, sizeof (kd_event));
  
  condition_broadcast (&kbdbuf.readcond);
  mutex_unlock (&global_lock);
}


static error_t
repeater_select (struct protid *cred, mach_port_t reply,
		 mach_msg_type_name_t replytype, int *type)
{
  if (!cred)
    return EOPNOTSUPP;

  if (*type & ~SELECT_READ)
    return EINVAL;

  if (*type == 0)
    return 0;

  mutex_lock (&global_lock);
  while (1)
    {
      if (kbdbuf.size > 0)
	{
	  *type = SELECT_READ;
	  mutex_unlock (&global_lock);

	  return 0;
	}
      
      ports_interrupt_self_on_port_death (cred, reply);
      if (hurd_condition_wait (&select_alert, &global_lock))
	{
	  *type = 0;
	  mutex_unlock (&global_lock);

	  return EINTR;
	}
    }
}


static error_t
repeater_read (struct protid *cred, char **data,
	       mach_msg_type_number_t *datalen, off_t offset,
	       mach_msg_type_number_t amount)
{
  /* Deny access if they have bad credentials. */
  if (! cred)
    return EOPNOTSUPP;
  else if (! (cred->po->openstat & O_READ))
    return EBADF;
  
  mutex_lock (&global_lock);
  while (amount > kbdbuf.size)
    {
      if (cred->po->openstat & O_NONBLOCK && amount > kbdbuf.size)
	{
	  mutex_unlock (&global_lock);
	  return EWOULDBLOCK;
	}
      
      if (hurd_condition_wait (&kbdbuf.readcond, &global_lock))
	{
	  mutex_unlock (&global_lock);
	  return EINTR;
	}
    }
  
  if (amount > 0)
    {
      char *keydata;
      unsigned int i = 0;

      /* Allocate a buffer when this is required.  */
      if (*datalen < amount)
	{
	  *data = mmap (0, amount, PROT_READ|PROT_WRITE, MAP_ANON, 0, 0);
	  if (*data == MAP_FAILED)
	    {
	      mutex_unlock (&global_lock);
	      return ENOMEM;
	    }
	}
      
      /* Copy the bytes to the user's buffer and remove them from the
	 keyboard buffer.  */
      keydata = *data;
      while (i != amount)
	{
	  keydata[i++] = kbdbuf.keybuffer[kbdbuf.pos++];
	  kbdbuf.pos = KBDBUF_POS (kbdbuf.pos);
	}
      kbdbuf.size -= amount;
      condition_broadcast (&kbdbuf.writecond);
    }
  
  *datalen = amount;
  mutex_unlock (&global_lock);

  return 0;
}


static void
repeater_open (void)
{
  /* Make sure the console does not access the hardware anymore.  */
  if (! kbd_repeater_opened)
    console_switch_away ();
  kbd_repeater_opened++;
}


static void
repeater_close (void)
{
  kbd_repeater_opened--;

  /* Allow the console to access the hardware again.  */
  if (! kbd_repeater_opened)
    {
      console_switch_back ();
      kbdbuf.pos = 0;
      kbdbuf.size = 0;
    }
}


/* Set the repeater translator.  The node will be named NODENAME and
   NODE will be filled with information about this node.  */
error_t
kbd_setrepeater (const char *nodename, consnode_t *cn)
{
  extern int kdioctl_server (mach_msg_header_t *inp, mach_msg_header_t *outp);
  error_t err;
  
  err = console_create_consnode (nodename, cn);
  if (err)
    return err;
  
  (*cn)->read = repeater_read;
  (*cn)->write = 0;
  (*cn)->select = repeater_select;
  (*cn)->open = repeater_open;
  (*cn)->close = repeater_close;
  (*cn)->demuxer = kdioctl_server;
  
  mutex_init (&global_lock);

  condition_init (&kbdbuf.readcond);
  condition_init (&kbdbuf.writecond);

  condition_init (&select_alert);
  condition_implies (&kbdbuf.readcond, &select_alert);
  
  console_register_consnode (*cn);
  
  return 0;
}


/* Some RPC calls for controlling the keyboard.  These calls are just
   ignored and just exist to make XFree happy.  */

kern_return_t
S_kdioctl_kdskbdmode (io_t port, int mode)
{
  return 0;
}


kern_return_t
S_kdioctl_kdgkbdmode (io_t port, int *mode)
{
  return 0;
}
