/* Implementation of memory_object_data_return for pager library
   Copyright (C) 1994,95,96,99,2000,02 Free Software Foundation, Inc.

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

#include "priv.h"
#include "memory_object_S.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* Worker function used by _pager_seqnos_memory_object_data_return
   and _pager_seqnos_memory_object_data_initialize.  All args are
   as for _pager_seqnos_memory_object_data_return; the additional
   INITIALIZING arg identifies which function is calling us. */
kern_return_t
_pager_do_write_request (mach_port_t object,
			 mach_port_seqno_t seqno,
			 mach_port_t control,
			 vm_offset_t offset,
			 pointer_t data,
			 vm_size_t length,
			 int dirty,
			 int kcopy,
			 int initializing)
{
  struct pager *p;
  short *pm_entries;
  int npages, i;
  error_t *pagerrs;
  struct lock_request *lr;
  struct lock_list {struct lock_request *lr;
		    struct lock_list *next;} *lock_list, *ll;
  int wakeup;
  int omitdata = 0;

  p = ports_lookup_port (0, object, _pager_class);
  if (!p)
    return EOPNOTSUPP;

  /* Acquire the right to meddle with the pagemap */
  mutex_lock (&p->interlock);
  _pager_wait_for_seqno (p, seqno);

  /* sanity checks -- we don't do multi-page requests yet.  */
  if (control != p->memobjcntl)
    {
      printf ("incg data return: wrong control port\n");
      goto release_out;
    }
  if (length % __vm_page_size)
    {
      printf ("incg data return: bad length size %zd\n", length);
      goto release_out;
    }
  if (offset % __vm_page_size)
    {
      printf ("incg data return: misaligned request\n");
      goto release_out;
    }

  if (! dirty)
    goto release_out;

  if (p->pager_state != NORMAL)
    {
      printf ("pager in wrong state for write\n");
      goto release_out;
    }

  npages = length / __vm_page_size;
  pagerrs = alloca (npages * sizeof (error_t));

  _pager_block_termination (p);	/* until we are done with the pagemap
				   when the write completes. */

  _pager_pagemap_resize (p, offset + length);

  pm_entries = &p->pagemap[offset / __vm_page_size];

  /* Make sure there are no other in-progress writes for any of these
     pages before we begin.  This imposes a little more serialization
     than we really have to require (because *all* future writes on
     this object are going to wait for seqno while we wait for the
     previous write), but the case is relatively infrequent.  */
 retry:
  for (i = 0; i < npages; i++)
    if (pm_entries[i] & PM_PAGINGOUT)
      {
	pm_entries[i] |= PM_WRITEWAIT;
	condition_wait (&p->wakeup, &p->interlock);
	goto retry;
      }

  /* Mark these pages as being paged out.  */
  if (initializing)
    {
      assert (npages <= 32);
      for (i = 0; i < npages; i++)
	{
	  if (pm_entries[i] & PM_INIT)
	    omitdata |= 1 << i;
	  else
	    pm_entries[i] |= PM_PAGINGOUT | PM_INIT;
	}
    }
  else
    for (i = 0; i < npages; i++)
      pm_entries[i] |= PM_PAGINGOUT | PM_INIT;

  if (!kcopy)
    for (i = 0; i < npages; i++)
      pm_entries[i] &= ~PM_INCORE;

  /* If this write occurs while a lock is pending, record
     it.  We have to keep this list because a lock request
     might come in while we do the I/O; in that case there
     would be a new entry on p->lock_requests and we must
     make sure we don't decrement it.  So we have to keep
     track independently of which lock requests we incremented. */
  lock_list = 0;
  for (lr = p->lock_requests; lr; lr = lr->next)
    if (offset < lr->end && offset + length >= lr->start)
      {
	ll = alloca (sizeof (struct lock_list));
	ll->lr = lr;
	ll->next = lock_list;
	lock_list = ll;
	lr->pending_writes++;
      }

  /* Let someone else in. */
  _pager_release_seqno (p, seqno);
  mutex_unlock (&p->interlock);

  /* This is inefficient; we should send all the pages to the device at once
     but until the pager library interface is changed, this will have to do. */

  for (i = 0; i < npages; i++)
    if (!(omitdata & (1 << i)))
      pagerrs[i] = pager_write_page (p->upi,
				     offset + (vm_page_size * i),
				     data + (vm_page_size * i));

  /* Acquire the right to meddle with the pagemap */
  mutex_lock (&p->interlock);
  _pager_pagemap_resize (p, offset + length);
  pm_entries = &p->pagemap[offset / __vm_page_size];

  wakeup = 0;
  for (i = 0; i < npages; i++)
    {
      if (omitdata & (1 << i))
	continue;

      if (pm_entries[i] & PM_WRITEWAIT)
	wakeup = 1;

      if (pagerrs[i] && ! (pm_entries[i] & PM_PAGEINWAIT))
	/* The only thing we can do here is mark the page, and give
	   errors from now on when it is to be read.  This is
	   imperfect, because if all users go away, the pagemap will
	   be freed, and this information lost.  Oh well.  It's still
	   better than Un*x.  Of course, if we are about to hand this
	   data to the kernel, the error isn't a problem, hence the
	   check for pageinwait.  */
	pm_entries[i] |= PM_INVALID;

      if (pm_entries[i] & PM_PAGEINWAIT)
	memory_object_data_supply (p->memobjcntl,
				   offset + (vm_page_size * i),
				   data + (vm_page_size * i),
				   vm_page_size, 1,
				   VM_PROT_NONE, 0, MACH_PORT_NULL);
      else
	munmap ((caddr_t) (data + (vm_page_size * i)),
		vm_page_size);

      pm_entries[i] &= ~(PM_PAGINGOUT | PM_PAGEINWAIT | PM_WRITEWAIT);
    }

  for (ll = lock_list; ll; ll = ll->next)
    if (!--ll->lr->pending_writes && !ll->lr->locks_pending)
      wakeup = 1;

  if (wakeup)
    condition_broadcast (&p->wakeup);

  _pager_allow_termination (p);

  mutex_unlock (&p->interlock);

  ports_port_deref (p);
  return 0;

 release_out:
  _pager_release_seqno (p, seqno);
  mutex_unlock (&p->interlock);
  ports_port_deref (p);
  return 0;
}

/* Implement pageout call back as described by <mach/memory_object.defs>. */
kern_return_t
_pager_seqnos_memory_object_data_return (mach_port_t object,
					 mach_port_seqno_t seqno,
					 mach_port_t control,
					 vm_offset_t offset,
					 pointer_t data,
					 vm_size_t length,
					 int dirty,
					 int kcopy)
{
  return _pager_do_write_request (object, seqno, control, offset, data,
				  length, dirty, kcopy, 0);
}
