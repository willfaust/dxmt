/*
 * d3d9shim_lock.c -- the D3DCREATE_MULTITHREADED device lock (8.2(d))
 *
 * The lock moves out of the frontend and into the shim, because it serializes
 * APPLICATION threads and those only exist on the guest side; the native
 * device is constructed is_protected=false.  Same semantics as
 * d3d9_multithread.hpp D9RecursiveSpinlock: recursive, keyed by
 * GetCurrentThreadId, a bounded pause-spin and then SwitchToThread, because
 * the holder can be parked on a GPU fence for milliseconds.  A device created
 * without D3DCREATE_MULTITHREADED takes no lock at all -- the application has
 * promised single-threaded use, which is what the native runtime and DXVK
 * assume too.
 *
 * Copyright 2026 Will Faust
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define CINTERFACE
#define COBJMACROS

#include <stdio.h>

#include "d3d9shim_object.h"

#if defined(__i386__) || defined(__x86_64__)
#define D3D9SHIM_YIELD_PROCESSOR() __asm__ __volatile__("pause" ::: "memory")
#else
#define D3D9SHIM_YIELD_PROCESSOR() __asm__ __volatile__("" ::: "memory")
#endif

static int
try_lock(struct d3d9shim_device_extra *extra, LONG self)
{
    LONG previous = InterlockedCompareExchange(&extra->lock_owner, self, 0);

    if (previous == 0)
        return 1;
    if (previous != self)
        return 0;
    extra->lock_depth += 1;
    return 1;
}

/* MADEIRA ml2000: a waiter that has yielded for a long time switches
 * SwitchToThread for Sleep(1) -- SwitchToThread only yields to a thread ready
 * on the same core, so with few cores and QoS a preempted owner can starve
 * behind a yielding waiter (MADEIRA_D9_LOCK_BACKOFF=0 keeps the pure yield).
 * A wait over 1 s is reported once per wait with owner, depth and waiter:
 * [d3d9-lock-spin] ml2000 (MADEIRA_D9_LOCK_DIAG=0 silences). */
#define D3D9SHIM_BACKOFF_ROUNDS 256

static LONG backoff_flag = -1, diag_flag = -1, lock_reports;

static int
lock_flag(LONG *flag, const char *name)
{
    LONG v = *flag;

    if (v < 0) {
        char value[8];
        DWORD len = GetEnvironmentVariableA(name, value, sizeof(value));
        v = !(len == 1 && value[0] == '0');
        InterlockedExchange(flag, v);
    }
    return (int)v;
}

static int
lock_report_spin(struct d3d9shim_device_extra *extra, LONG self, ULONGLONG start, unsigned int rounds)
{
    char msg[256];

    if (InterlockedIncrement(&lock_reports) > 32)
        return 0;
    snprintf(msg, sizeof(msg), "[d3d9-lock-spin] ml2000 shim device lock waited %lu ms: waiter=%ld "
             "owner=%ld owner-depth=%ld rounds=%u backoff=%s (MADEIRA_D9_LOCK_DIAG=0 silences, "
             "MADEIRA_D9_LOCK_BACKOFF=0 pure yield)",
             (unsigned long)(GetTickCount64() - start), (long)self, (long)extra->lock_owner,
             (long)*(volatile LONG *)&extra->lock_depth, rounds,
             lock_flag(&backoff_flag, "MADEIRA_D9_LOCK_BACKOFF") ? "sleep1" : "yield");
    d3d9shim_trace(msg);
    return 1;
}

static void
lock_report_acquired(LONG self, ULONGLONG start)
{
    char msg[128];

    snprintf(msg, sizeof(msg), "[d3d9-lock-spin] ml2000 shim waiter=%ld acquired the device lock after %lu ms",
             (long)self, (unsigned long)(GetTickCount64() - start));
    d3d9shim_trace(msg);
}

void
d3d9shim_lock(struct d3d9shim_device *dev)
{
    struct d3d9shim_device_extra *extra;
    LONG self;
    unsigned int i, rounds = 0;
    ULONGLONG start = 0;
    int reported = 0;

    /* Called with a NULL device by every body on an object that has none
     * (IDirect3D9Ex), and by every body on a single-threaded device. */
    if (!dev || !dev->multithreaded)
        return;
    extra = d3d9shim_extra(dev);
    if (!extra)
        return;
    self = (LONG)GetCurrentThreadId();

    if (try_lock(extra, self))
        return;
    while (!try_lock(extra, self)) {
        for (i = 0; i < 2000; i++) {
            D3D9SHIM_YIELD_PROCESSOR();
            if (try_lock(extra, self)) {
                if (reported)
                    lock_report_acquired(self, start);
                return;
            }
        }
        rounds++;
        if (rounds == 1)
            start = GetTickCount64();
        /* ml2000: same backoff and diagnostic as D9RecursiveSpinlock. */
        if (rounds >= D3D9SHIM_BACKOFF_ROUNDS && lock_flag(&backoff_flag, "MADEIRA_D9_LOCK_BACKOFF"))
            Sleep(1);
        else
            SwitchToThread();
        if (!reported && !(rounds & 15) && lock_flag(&diag_flag, "MADEIRA_D9_LOCK_DIAG") &&
            GetTickCount64() - start > 1000)
            reported = lock_report_spin(extra, self, start, rounds);
    }
    if (reported)
        lock_report_acquired(self, start);
}

void
d3d9shim_unlock(struct d3d9shim_device *dev)
{
    struct d3d9shim_device_extra *extra;

    if (!dev || !dev->multithreaded)
        return;
    extra = d3d9shim_extra(dev);
    if (!extra)
        return;
    if (extra->lock_depth == 0)
        InterlockedExchange(&extra->lock_owner, 0);
    else
        extra->lock_depth -= 1;
}
