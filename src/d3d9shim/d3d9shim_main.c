/*
 * d3d9shim_main.c -- DllMain, the ten exports, and the unix-call transport
 *
 * This is the i386 d3d9.dll of WOW64_DESIGN.md 8.5: a thin shim in front of
 * the native ARM64 D3D9 frontend.  It owns the A/B knob that decides whether
 * the process runs on the shim or on the previous emulated frontend (shipped
 * alongside as d3d9-emulated.dll), the unixlib binding, and the three export
 * groups that never cross at all -- D3DPERF_*, DebugSet* and the shader
 * validator's whole state machine (8.2(b)).
 *
 * PROVENANCE: the D3DPERF_* bodies and the Direct3DShaderValidatorCreate9
 * state machine are moved VERBATIM from research/dxmt/src/d3d9/d3d9.cpp (the
 * blocks the design calls `d3d9.cpp:44-67` and `d3d9.cpp:85-316`), which is
 * DXMT code under LGPL-2.1-or-later (COPYING.LIB), and rewritten from C++
 * into C without changing any observable behaviour.  The moved blocks keep
 * that licence; the rest of this file is GPL-3.0-or-later.  See
 * research/dxmt/LICENSE-MADEIRA.md.
 *
 * Copyright 2023-2026 Feifan He for CodeWeavers (the moved blocks)
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
 * SPDX-License-Identifier: GPL-3.0-or-later AND LGPL-2.1-or-later
 */

#define CINTERFACE
#define COBJMACROS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9shim_object.h"

/* ------------------------------------------------------------------------
 * diagnostics
 * ------------------------------------------------------------------------ */

void
d3d9shim_trace(const char *msg)
{
    if (!msg)
        return;
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
}

/* One line per distinct message.  The generated thunks pass string literals,
 * so comparing the pointer is enough and costs nothing on the hot path. */
void
d3d9shim_log_once(const char *what)
{
    static const char *seen[64];
    static LONG seen_count;
    LONG i, count;

    if (!what)
        return;
    count = seen_count;
    for (i = 0; i < count && i < 64; i++)
        if (seen[i] == what)
            return;
    if (count < 64) {
        seen[count] = what;
        InterlockedIncrement(&seen_count);
    }
    d3d9shim_trace(what);
}

/* ------------------------------------------------------------------------
 * the A/B knob (8.5)
 *
 * Every Madeira knob lives in Documents/madeira-<name>.txt, and every one of
 * them is read by the Swift app, which exports it into the process
 * environment (ContentView.swift does this for madeira-prof.txt,
 * madeira-wx.txt, madeira-dxmt.txt and the rest).  There is no PE-side reader
 * for those files -- Documents is not mapped into the prefix -- so the shim
 * reads MADEIRA_D3D9 from the environment and the app maps
 * Documents/madeira-d3d9.txt onto it, exactly like every other knob.  See the
 * step-3 report: that mapping is a one-line addition to ContentView.swift and
 * belongs to the APP track.
 * ------------------------------------------------------------------------ */

enum d3d9shim_mode {
    D3D9SHIM_MODE_UNSET = 0,
    D3D9SHIM_MODE_DEFAULT,     /* no knob: forward to the emulated frontend,
                                * which is the shipped default (see below) */
    D3D9SHIM_MODE_NATIVE,      /* MADEIRA_D3D9=native: native or nothing */
    D3D9SHIM_MODE_EMULATED     /* forward everything to d3d9-emulated.dll */
};

static LONG  shim_mode = D3D9SHIM_MODE_UNSET;
static HMODULE emulated_module;

static void
read_mode(void)
{
    char value[64];
    DWORD len;

    len = GetEnvironmentVariableA("MADEIRA_D3D9", value, sizeof(value));
    if (len == 0 || len >= sizeof(value)) {
        shim_mode = D3D9SHIM_MODE_DEFAULT;
        d3d9shim_trace("[d3d9] MADEIRA_D3D9 unset: forwarding to "
                       "d3d9-emulated.dll (Documents/madeira-d3d9.txt = "
                       "native selects the native ARM64 frontend)");
        return;
    }
    if (!_stricmp(value, "emulated") || !_stricmp(value, "pe")
        || !_stricmp(value, "0")) {
        shim_mode = D3D9SHIM_MODE_EMULATED;
        d3d9shim_trace("[d3d9] MADEIRA_D3D9=emulated: forwarding to "
                       "d3d9-emulated.dll");
        return;
    }
    if (!_stricmp(value, "native")) {
        shim_mode = D3D9SHIM_MODE_NATIVE;
        return;
    }
    shim_mode = D3D9SHIM_MODE_DEFAULT;
    d3d9shim_trace("[d3d9] MADEIRA_D3D9 is neither native nor emulated; "
                   "using the default");
}

/* THE SHIPPED DEFAULT IS THE EMULATED FRONTEND (native D3D9 plan step 4,
 * item 6).  The shim is what games import -- build-pe.sh installs it as
 * d3d9.dll with MADEIRA_D3D9_DEFAULT=shim -- but with no knob set it forwards
 * all ten exports to d3d9-emulated.dll, so the default path is byte for byte
 * the frontend section 6 measured and Log 41 ran at 30-40 fps.  The native
 * path is opt-in: Documents/madeira-d3d9.txt = `native`, which ContentView
 * exports as MADEIRA_D3D9.  Making native the default before it has run on
 * device would have staked the whole app on an untested path with no way to
 * compare, which is the opposite of what an A/B knob is for. */
static int
forwarding(void)
{
    return shim_mode == D3D9SHIM_MODE_EMULATED
           || shim_mode == D3D9SHIM_MODE_DEFAULT;
}

/* Resolve one export out of the emulated frontend.  A failure here is fatal
 * for the call, not silently papered over: the A/B knob asked for that
 * module, so pretending the shim answered would make the comparison
 * meaningless. */
static void *
emulated_entry(const char *name)
{
    static CRITICAL_SECTION cs;
    static LONG cs_ready;
    void *proc;

    if (InterlockedCompareExchange(&cs_ready, 1, 0) == 0)
        InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    if (!emulated_module) {
        emulated_module = LoadLibraryA("d3d9-emulated.dll");
        if (!emulated_module)
            d3d9shim_log_once("[d3d9] MADEIRA_D3D9=emulated but "
                              "d3d9-emulated.dll could not be loaded");
    }
    LeaveCriticalSection(&cs);
    if (!emulated_module)
        return NULL;
    proc = (void *)GetProcAddress(emulated_module, name);
    if (!proc)
        d3d9shim_log_once("[d3d9] d3d9-emulated.dll is missing an export");
    return proc;
}

/* ------------------------------------------------------------------------
 * the transport (8.3)
 *
 * The shim binds its OWN unixlib table, the way winemetal.dll does: winecrt0's
 * __wine_init_unix_call() asks NtQueryVirtualMemory(MemoryWineLoadUnixLib*)
 * about this module's own image base, and ntdll's
 * ios_bind_unixlib_table() picks the 64-bit or the wow64 table by the
 * CALLER's bitness (virtual_ios.c:7217).  Until step 4 adds the d3d9 branch
 * next to the winemetal one, the bind fails and every call fails with it --
 * loudly, and with no fake success.
 * ------------------------------------------------------------------------ */

typedef UINT64 d3d9shim_unixlib_handle_t;

extern d3d9shim_unixlib_handle_t __wine_unixlib_handle;
extern LONG (WINAPI *__wine_unix_call_dispatcher)(d3d9shim_unixlib_handle_t,
                                                  unsigned int, void *);
extern LONG WINAPI __wine_init_unix_call(void);

static LONG transport_state;   /* 0 = untried, 1 = ready, -1 = failed */

int
d3d9shim_transport_ready(void)
{
    return transport_state == 1;
}

int
d3d9shim_transport_init(void)
{
    struct d3d9_init_params params;
    LONG status;

    if (transport_state)
        return transport_state == 1;

    status = __wine_init_unix_call();
    if (status) {
        d3d9shim_trace("[d3d9] no unix side: __wine_init_unix_call failed "
                       "(is the d3d9 branch of load_builtin_unixlib in place?)");
        transport_state = -1;
        return 0;
    }

    memset(&params, 0, sizeof(params));
    params.api_hash = D3D9SHIM_API_HASH;
    params.api_version = D3D9SHIM_API_VERSION;
    params.ptr_size = (uint32_t)sizeof(void *);
    params.op_count = D3D9SHIM_OP_COUNT;
    status = __wine_unix_call_dispatcher(__wine_unixlib_handle, D3D9OP_init,
                                         &params);
    if (status || params.status != D3D9SHIM_INIT_OK) {
        char line[192];

        snprintf(line, sizeof(line),
                 "[d3d9] init handshake refused: status 0x%08x, native hash "
                 "0x%08x%08x, shim hash 0x%08x%08x -- regenerate both halves "
                 "from d3d9_api.py",
                 (unsigned int)status,
                 (unsigned int)(params.native_hash >> 32),
                 (unsigned int)params.native_hash,
                 (unsigned int)(D3D9SHIM_API_HASH >> 32),
                 (unsigned int)D3D9SHIM_API_HASH);
        d3d9shim_trace(line);
        transport_state = -1;
        return 0;
    }
    transport_state = 1;
    d3d9shim_arena_init();
    return 1;
}

/* The one crossing.  Everything generated funnels through here, which is also
 * why the shadow update and the creation context hang off it. */
uint32_t
d3d9shim_native_call(unsigned int slot, void *block, unsigned int size)
{
    LONG status;

    (void)size;
    if (transport_state != 1 && !d3d9shim_transport_init())
        return 1;

    /* Remember what is in flight so d3d9shim_obj_from_native() can find the
     * new object's parent device without a second crossing. */
    d3d9shim_obj_note_call(slot, block);

    status = __wine_unix_call_dispatcher(__wine_unixlib_handle, slot, block);
    if ((uint32_t)status == D3D9SHIM_STATUS_ARENA_EXHAUSTED) {
        /* The native sub-allocator ran out of arena.  Grow and retry once;
         * the design is explicit that this needs no upcall machinery. */
        if (SUCCEEDED(d3d9shim_arena_grow(0)))
            status = __wine_unix_call_dispatcher(__wine_unixlib_handle, slot,
                                                 block);
    }
    if (status)
        return (uint32_t)status ? (uint32_t)status : 1u;

    /* No shadow update here.  The generated deferred bodies apply the shadow
     * on BOTH arms -- the phase-2 arm before the ring append, the phase-1 arm
     * after a successful synchronous call -- so doing it again for every
     * crossing applied the same block twice, and applied it on the resolve
     * and sync crossings too, where it matches no opcode. */
    return 0;
}

/* ------------------------------------------------------------------------
 * the command ring -- declared for phase 2 (8.6), inert in phase 1
 *
 * Phase 1 keeps append at "it did not fit", so every deferred body falls
 * through to its synchronous arm, and flush has nothing to replay.  The
 * record header and the ring header are the ones d3d9shim_ops.h and
 * d3d9_ring_replay() already agree on.
 * ------------------------------------------------------------------------ */

struct d3d9shim_ring {
    unsigned char * base;    /* VirtualAlloc'd in the window, one per device */
    uint32_t        head;    /* write offset */
    uint32_t        tail;    /* replay offset */
    uint32_t        size;
    uint32_t        seq;     /* monotonic, asserted at replay */
};

int
d3d9shim_ring_append(struct d3d9shim_device *dev, unsigned int op,
                     const void *block, unsigned int size)
{
    (void)dev;
    (void)op;
    (void)block;
    (void)size;
    return 0;   /* phase 1: never fits, so the caller calls synchronously */
}

HRESULT
d3d9shim_flush(struct d3d9shim_device *dev)
{
    (void)dev;
    return D3D_OK;
}

/* ------------------------------------------------------------------------
 * DllMain
 * ------------------------------------------------------------------------ */

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        read_mode();
        /* The unixlib bind and the init handshake are deliberately NOT done
         * here: they are real work under the loader lock, and a failure would
         * turn a missing unix side into a failed LoadLibrary rather than a
         * diagnosable Direct3DCreate9 returning NULL.  Direct3DCreate9 does
         * both on first use (8.5). */
    } else if (reason == DLL_PROCESS_DETACH) {
        d3d9shim_arena_report();
    }
    return TRUE;
}

/* ------------------------------------------------------------------------
 * Direct3DCreate9 / Direct3DCreate9Ex
 * ------------------------------------------------------------------------ */

typedef IDirect3D9 *(WINAPI *create9_fn)(UINT);
typedef HRESULT (WINAPI *create9ex_fn)(UINT, IDirect3D9Ex **);

static struct d3d9shim_object *
create_interface(UINT sdk_version, int is_ex)
{
    struct d3d9_create_interface_params params;
    struct d3d9shim_impl *impl;

    if (!d3d9shim_transport_init())
        return NULL;

    memset(&params, 0, sizeof(params));
    params.sdk_version = sdk_version;
    params.is_ex = (uint32_t)is_ex;
    if (d3d9shim_native_call(D3D9SHIM_OP_create_interface, &params, sizeof(params))) {
        d3d9shim_log_once("[d3d9] the native side could not create an "
                          "IDirect3D9 (is the create-interface slot bound?)");
        return NULL;
    }
    if (FAILED((HRESULT)params.ret) || !params.iface)
        return NULL;

    impl = d3d9shim_obj_alloc(D3D9SHIM_KIND_D3D9, params.iface, NULL,
                              is_ex ? D3D9SHIM_F_IS_EX : 0);
    return impl ? &impl->o.hdr : NULL;
}

/* MADEIRA_D3D9=native asks for the native frontend and nothing else: a
 * missing or broken unix side has to be a visible failure, not a silently
 * slower frame, or the A/B measures the wrong thing.  Every other mode is
 * already forwarding by the time it gets here, so there is nothing to fall
 * back FROM. */
static int
fall_back_to_emulated(void)
{
    if (shim_mode != D3D9SHIM_MODE_DEFAULT)
        return 0;
    d3d9shim_trace("[d3d9] the native frontend is unavailable; falling back to "
                   "d3d9-emulated.dll (set MADEIRA_D3D9=native to fail instead)");
    shim_mode = D3D9SHIM_MODE_EMULATED;
    return 1;
}

IDirect3D9 *WINAPI
Direct3DCreate9(UINT SDKVersion)
{
    struct d3d9shim_object *obj;
    create9_fn fn;

    if (!forwarding()) {
        obj = create_interface(SDKVersion, 0);
        if (obj)
            return (IDirect3D9 *)obj;
        if (!fall_back_to_emulated())
            return NULL;
    }
    fn = (create9_fn)emulated_entry("Direct3DCreate9");
    return fn ? fn(SDKVersion) : NULL;
}

HRESULT WINAPI
Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppD3D)
{
    struct d3d9shim_object *obj;
    create9ex_fn fn;

    if (!ppD3D)
        return D3DERR_INVALIDCALL;
    if (!forwarding()) {
        obj = create_interface(SDKVersion, 1);
        if (obj) {
            *ppD3D = (IDirect3D9Ex *)obj;
            return D3D_OK;
        }
        if (!fall_back_to_emulated())
            return D3DERR_NOTAVAILABLE;
    }
    fn = (create9ex_fn)emulated_entry("Direct3DCreate9Ex");
    return fn ? fn(SDKVersion, ppD3D) : E_FAIL;
}

/* ------------------------------------------------------------------------
 * D3DPERF_* and DebugSet* (d3d9.cpp:44-74, moved verbatim)
 * ------------------------------------------------------------------------ */

/* PIX event nesting counter. Mirrors wined3d d3d9_main.c.
 * BeginEvent returns the depth the new event sits at (the prior nesting
 * level, returned before incrementing); EndEvent returns the depth left
 * after popping (the level after decrementing). Apps inspect the level to
 * validate nesting; returning 0 unconditionally would silently break that
 * contract. Atomic because PIX events are documented as callable from any
 * thread holding the device. */
static LONG s_d3dperfEventLevel;

int WINAPI
D3DPERF_BeginEvent(D3DCOLOR color, const WCHAR *name)
{
    (void)color;
    (void)name;
    if (forwarding()) {
        int (WINAPI *fn)(D3DCOLOR, const WCHAR *) =
            (int (WINAPI *)(D3DCOLOR, const WCHAR *))emulated_entry("D3DPERF_BeginEvent");

        return fn ? fn(color, name) : 0;
    }
    return (int)(InterlockedIncrement(&s_d3dperfEventLevel) - 1);
}

int WINAPI
D3DPERF_EndEvent(void)
{
    if (forwarding()) {
        int (WINAPI *fn)(void) = (int (WINAPI *)(void))emulated_entry("D3DPERF_EndEvent");

        return fn ? fn() : 0;
    }
    return (int)InterlockedDecrement(&s_d3dperfEventLevel);
}

/* The remaining seven are constants in both modules, but they are forwarded
 * anyway when the knob says `emulated`: the A/B is only meaningful if every
 * one of the ten exports comes from the module under test. */
DWORD WINAPI
D3DPERF_GetStatus(void)
{
    if (forwarding()) {
        DWORD (WINAPI *fn)(void) = (DWORD (WINAPI *)(void))emulated_entry("D3DPERF_GetStatus");

        return fn ? fn() : 0;
    }
    return 0;
}

BOOL WINAPI
D3DPERF_QueryRepeatFrame(void)
{
    if (forwarding()) {
        BOOL (WINAPI *fn)(void) = (BOOL (WINAPI *)(void))emulated_entry("D3DPERF_QueryRepeatFrame");

        return fn ? fn() : FALSE;
    }
    return FALSE;
}

void WINAPI
D3DPERF_SetMarker(D3DCOLOR color, const WCHAR *name)
{
    if (forwarding()) {
        void (WINAPI *fn)(D3DCOLOR, const WCHAR *) =
            (void (WINAPI *)(D3DCOLOR, const WCHAR *))emulated_entry("D3DPERF_SetMarker");

        if (fn)
            fn(color, name);
        return;
    }
    (void)color;
    (void)name;
}

void WINAPI
D3DPERF_SetOptions(DWORD options)
{
    if (forwarding()) {
        void (WINAPI *fn)(DWORD) = (void (WINAPI *)(DWORD))emulated_entry("D3DPERF_SetOptions");

        if (fn)
            fn(options);
        return;
    }
    (void)options;
}

void WINAPI
D3DPERF_SetRegion(D3DCOLOR color, const WCHAR *name)
{
    if (forwarding()) {
        void (WINAPI *fn)(D3DCOLOR, const WCHAR *) =
            (void (WINAPI *)(D3DCOLOR, const WCHAR *))emulated_entry("D3DPERF_SetRegion");

        if (fn)
            fn(color, name);
        return;
    }
    (void)color;
    (void)name;
}

void WINAPI
DebugSetLevel(DWORD level)
{
    if (forwarding()) {
        void (WINAPI *fn)(DWORD) = (void (WINAPI *)(DWORD))emulated_entry("DebugSetLevel");

        if (fn)
            fn(level);
        return;
    }
    (void)level;
}

BOOL WINAPI
DebugSetMute(void)
{
    if (forwarding()) {
        BOOL (WINAPI *fn)(void) = (BOOL (WINAPI *)(void))emulated_entry("DebugSetMute");

        return fn ? fn() : TRUE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------------
 * Direct3DShaderValidatorCreate9 (d3d9.cpp:85-316, moved verbatim)
 *
 * Undocumented MS export (fxc.exe drives it to validate a shader token stream
 * out of process).  It is a small COM-style state machine: Begin, then one
 * Instruction per token group (version token, then the body, then the
 * 0x0000ffff end token), then End.  A malformed stream fires the caller's
 * message callback.  The exact contract (message ids, the two messages a bad
 * version token reports, the register errors that fire a message yet return
 * S_OK and only make End fail, and the stuck Error state) is pinned against
 * native by the wine d3d9 test.  DXVK's d3d9_shader_validator is a partial
 * reference: it stops after the first header error where native reports both.
 *
 * It never crosses: no device, no Metal, no guest-pointer conversion (8.2(b)).
 * ------------------------------------------------------------------------ */

typedef HRESULT (WINAPI *shader_validator_cb)(const char *file, int line,
                                              DWORD_PTR arg3, DWORD_PTR message_id,
                                              const char *message, void *context);

enum shader_validator_state {
    SV_BEGIN = 0, /* awaiting Begin */
    SV_HEADER,    /* Begin done, awaiting the version token */
    SV_BODY,      /* version accepted, validating instructions */
    SV_END_TOKEN, /* end token seen, awaiting End */
    SV_ERROR      /* a fatal error latched; only a fresh validator recovers */
};

/* Message ids match the values the wine d3d9 test pins against native for the
 * cases it exercises (0xeb, 0xef, 0xf0, 0x12c, 0x167); the remaining ids
 * follow the same numbering DXVK uses and only surface on malformed API call
 * order fxc never produces, so they are consistent but not independently
 * native-verified. */
enum shader_validator_message {
    SVM_BEGIN_OUT_OF_ORDER = 0xeb,
    SVM_INSTRUCTION_OUT_OF_ORDER = 0xec,
    SVM_INSTRUCTION_AFTER_END = 0xed,
    SVM_INSTRUCTION_NULL_ARGS = 0xee,
    SVM_BAD_VERSION_LENGTH = 0xef,
    SVM_BAD_VERSION_TYPE = 0xf0,
    SVM_BAD_END_TOKEN = 0xf1,
    SVM_END_OUT_OF_ORDER = 0xf2,
    SVM_MISSING_END_TOKEN = 0xf3,
    SVM_BAD_INPUT_REGISTER_DECL = 0x12c,
    SVM_BAD_INPUT_REGISTER = 0x167
};

struct IDirect3DShaderValidator9;

struct IDirect3DShaderValidator9Vtbl {
    HRESULT (WINAPI *QueryInterface)(struct IDirect3DShaderValidator9 *iface,
                                     REFIID iid, void **out);
    ULONG (WINAPI *AddRef)(struct IDirect3DShaderValidator9 *iface);
    ULONG (WINAPI *Release)(struct IDirect3DShaderValidator9 *iface);
    HRESULT (WINAPI *Begin)(struct IDirect3DShaderValidator9 *iface,
                            shader_validator_cb callback, void *context,
                            DWORD_PTR arg3);
    HRESULT (WINAPI *Instruction)(struct IDirect3DShaderValidator9 *iface,
                                  const char *file, int line,
                                  const DWORD *tokens, unsigned int token_count);
    HRESULT (WINAPI *End)(struct IDirect3DShaderValidator9 *iface);
};

struct IDirect3DShaderValidator9 {
    const struct IDirect3DShaderValidator9Vtbl *vtbl;
    LONG                 refcount;
    int                  state;
    shader_validator_cb  callback;
    void *               context;
    int                  is_pixel_shader;
    int                  error_seen;
};

static void
shader_validator_emit(struct IDirect3DShaderValidator9 *v, const char *file,
                      int line, DWORD_PTR message_id, const char *message)
{
    v->error_seen = 1;
    if (v->callback)
        v->callback(file, line, 0, message_id, message, v->context);
}

static HRESULT WINAPI
shader_validator_QueryInterface(struct IDirect3DShaderValidator9 *iface,
                                REFIID iid, void **out)
{
    (void)iface;
    (void)iid;
    if (out)
        *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
shader_validator_AddRef(struct IDirect3DShaderValidator9 *v)
{
    return (ULONG)InterlockedIncrement(&v->refcount);
}

static ULONG WINAPI
shader_validator_Release(struct IDirect3DShaderValidator9 *v)
{
    ULONG rc = (ULONG)InterlockedDecrement(&v->refcount);

    if (rc == 0)
        HeapFree(GetProcessHeap(), 0, v);
    return rc;
}

static HRESULT WINAPI
shader_validator_Begin(struct IDirect3DShaderValidator9 *v,
                       shader_validator_cb callback, void *context,
                       DWORD_PTR arg3)
{
    (void)arg3;
    v->callback = callback;
    v->context = context;
    /* Begin is legal only on a fresh validator or once ::End has reset the
     * state to SV_BEGIN. A second Begin mid-stream, or one after a latched
     * error, is out of order: report it and latch, so only ::End or a fresh
     * validator recovers. */
    if (v->state != SV_BEGIN) {
        shader_validator_emit(v, NULL, 0, SVM_BEGIN_OUT_OF_ORDER,
                              "IDirect3DShaderValidator9::Begin called out of order. "
                              "::End must be called first.");
        v->state = SV_ERROR;
        return E_FAIL;
    }
    v->state = SV_HEADER;
    v->error_seen = 0;
    return S_OK;
}

static HRESULT WINAPI
shader_validator_Instruction(struct IDirect3DShaderValidator9 *v, const char *file,
                             int line, const DWORD *tokens, unsigned int token_count)
{
    DWORD kind, opcode;

    if (!tokens || !token_count) {
        shader_validator_emit(v, file, line, SVM_INSTRUCTION_NULL_ARGS,
                              "IDirect3DShaderValidator9::Instruction called with "
                              "NULL tokens or a zero token count.");
        return E_FAIL;
    }
    if (v->state == SV_ERROR)
        return E_FAIL;
    if (v->state == SV_BEGIN) {
        shader_validator_emit(v, file, line, SVM_INSTRUCTION_OUT_OF_ORDER,
                              "IDirect3DShaderValidator9::Instruction called out of "
                              "order. ::Begin must be called first.");
        return E_FAIL;
    }
    if (v->state == SV_END_TOKEN) {
        shader_validator_emit(v, file, line, SVM_INSTRUCTION_AFTER_END,
                              "IDirect3DShaderValidator9::Instruction called after the "
                              "end token. Call ::End next.");
        return E_FAIL;
    }
    if (v->state == SV_HEADER) {
        /* The version token is a single DWORD whose high word selects VS
         * (0xfffe) or PS (0xffff). A malformed one reports up to two
         * messages, both with a null file: the length arm carries line -1,
         * the type arm line 0. */
        int bad = 0;

        if (token_count != 1) {
            shader_validator_emit(v, NULL, -1, SVM_BAD_VERSION_LENGTH,
                                  "IDirect3DShaderValidator9::Instruction: bad version "
                                  "token, expected a single DWORD.");
            bad = 1;
        }
        kind = tokens[0] & 0xffff0000u;
        if (kind != 0xffff0000u && kind != 0xfffe0000u) {
            shader_validator_emit(v, NULL, 0, SVM_BAD_VERSION_TYPE,
                                  "IDirect3DShaderValidator9::Instruction: bad version "
                                  "token, neither a pixel nor a vertex shader.");
            bad = 1;
        }
        if (bad) {
            v->state = SV_ERROR;
            return E_FAIL;
        }
        v->is_pixel_shader = kind == 0xffff0000u;
        v->state = SV_BODY;
        return S_OK;
    }
    /* SV_BODY. The end token closes the stream. */
    if (tokens[0] == 0x0000ffffu) {
        if (token_count != 1) {
            shader_validator_emit(v, file, line, SVM_BAD_END_TOKEN,
                                  "IDirect3DShaderValidator9::Instruction: bad end "
                                  "token, expected a single DWORD.");
            v->state = SV_ERROR;
            return E_FAIL;
        }
        v->state = SV_END_TOKEN;
        return S_OK;
    }
    /* PS 3.0 exposes v0..v9 only; a source or dcl referencing an input
     * register >= 10 is reported (dcl gets its own message id) but is not
     * fatal: the call still succeeds and only ::End then fails. Register
     * tokens have bit 31 set; def / defb / defi / comment carry immediate
     * data, not registers, so skip them. */
    opcode = tokens[0] & 0x0000ffffu;
    if (v->is_pixel_shader && opcode != 0xfffeu /* comment */
        && opcode != 0x51u /* def */ && opcode != 0x52u /* defb */
        && opcode != 0x53u /* defi */) {
        unsigned int i;

        for (i = 1; i < token_count && (tokens[i] >> 31); ++i) {
            DWORD reg_type = ((tokens[i] & 0x70000000u) >> 28) | ((tokens[i] & 0x00001800u) >> 8);
            DWORD reg_index = tokens[i] & 0x000007ffu;

            if (reg_type == 1u /* D3DSPR_INPUT */ && reg_index >= 10u) {
                shader_validator_emit(v, file, line,
                                      opcode == 0x1fu /* dcl */
                                          ? SVM_BAD_INPUT_REGISTER_DECL
                                          : SVM_BAD_INPUT_REGISTER,
                                      "IDirect3DShaderValidator9::Instruction: pixel "
                                      "shader input register index out of range.");
                break;
            }
        }
    }
    return S_OK;
}

static HRESULT WINAPI
shader_validator_End(struct IDirect3DShaderValidator9 *v)
{
    int had_error;

    if (v->state == SV_ERROR)
        return E_FAIL;
    if (v->state == SV_BEGIN) {
        shader_validator_emit(v, NULL, 0, SVM_END_OUT_OF_ORDER,
                              "IDirect3DShaderValidator9::End called out of order. "
                              "::Begin must be called first.");
        return E_FAIL;
    }
    if (v->state != SV_END_TOKEN) {
        shader_validator_emit(v, NULL, 0, SVM_MISSING_END_TOKEN,
                              "IDirect3DShaderValidator9::End: the shader is missing "
                              "its end token.");
        v->state = SV_BEGIN;
        return E_FAIL;
    }
    had_error = v->error_seen;
    v->state = SV_BEGIN;
    v->error_seen = 0;
    return had_error ? E_FAIL : S_OK;
}

static const struct IDirect3DShaderValidator9Vtbl shader_validator_vtbl = {
    shader_validator_QueryInterface, shader_validator_AddRef,
    shader_validator_Release,        shader_validator_Begin,
    shader_validator_Instruction,    shader_validator_End,
};

struct IDirect3DShaderValidator9 *WINAPI
Direct3DShaderValidatorCreate9(void)
{
    struct IDirect3DShaderValidator9 *v;

    if (forwarding()) {
        struct IDirect3DShaderValidator9 *(WINAPI *fn)(void) =
            (struct IDirect3DShaderValidator9 *(WINAPI *)(void))
                emulated_entry("Direct3DShaderValidatorCreate9");

        return fn ? fn() : NULL;
    }
    v = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*v));
    if (!v)
        return NULL;
    v->vtbl = &shader_validator_vtbl;
    v->refcount = 1;
    v->state = SV_BEGIN;
    return v;
}
