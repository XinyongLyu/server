/*****************************************************************************

Copyright (c) 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

#ifndef __linux__
# error "This file is for Linux only"
#endif

#include "frw_lock.h"

#ifdef FRW_LOCK_DUMMY
/* Work around a potential build failure by preventing an empty .o file */
int frw_lock_dummy_function() { return 0; }
#else
# include <linux/futex.h>
# include <sys/syscall.h>

# include "srv0srv.h"

/** Wait for a read lock after a failed read_trylock() */
void frw_lock::read_lock()
{
  for (;;)
  {
    uint32_t l= read_lock_yield() - 1;
    if (l & WRITER_PENDING)
    {
      if (l == WRITER_WAITING)
      wake_writer:
        syscall(SYS_futex, word(), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
      syscall(SYS_futex, word(), FUTEX_WAIT_PRIVATE, l, nullptr, nullptr, 0);
    }
    else
    {
      for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
      {
        if (read_trylock())
          return;
        l= read_lock_yield() - 1;
        if (l & WRITER_PENDING)
        {
          if (l == WRITER_WAITING)
            goto wake_writer;
          ut_delay(srv_spin_wait_delay);
        }
      }
    }
    if (read_trylock())
      return;
  }
}

/** Wait for a write lock after a failed write_trylock() */
void frw_lock::write_lock()
{
  for (;;)
  {
    uint32_t l= write_lock_wait_start();
    /* We are the first writer to be granted the lock. Spin for a while. */
    for (auto spin= srv_n_spin_wait_rounds; spin; spin--)
    {
      if (write_lock_wait_try(l))
        return;
      if (!(l & WRITER_WAITING))
        l= write_lock_wait_start() | WRITER_WAITING;
      ut_delay(srv_spin_wait_delay);
    }

    if (write_lock_wait_try(l))
      return;
    DBUG_ASSERT(~WRITER_WAITING & l);
    if (!(l & WRITER_WAITING))
    {
      if (l == UNLOCKED && write_trylock())
        return;
      l= write_lock_wait_start() | WRITER_WAITING;
    }

    syscall(SYS_futex, word(), FUTEX_WAIT_PRIVATE, l, nullptr, nullptr, 0);
  }
}

void frw_lock::rd_unlock()
{
#ifdef UNIV_PFS_RWLOCK
  if (pfs_psi) PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
#endif
  if (read_unlock())
    syscall(SYS_futex, word(), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

void frw_lock::wr_unlock()
{
#ifdef UNIV_PFS_RWLOCK
  if (pfs_psi) PSI_RWLOCK_CALL(unlock_rwlock)(pfs_psi);
#endif
  write_unlock();
  syscall(SYS_futex, word(), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}
#endif
