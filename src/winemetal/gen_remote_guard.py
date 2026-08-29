#!/usr/bin/env python3
"""Generate unix/wmt_remote_guard.h from the unix dispatch table.

ntdll indexes __wine_unix_call_funcs[] directly, so there is no central
dispatch function to hook: in remote mode the guard has to BE the table entry.
Every handler not yet routed to the host fails BY NAME rather than running its
local ObjC body on a handle owned by another address space -- that derefs a
host pointer and dies as a wrong-looking frame instead of an error.

Generating this rather than hand-writing it keeps the table's ORDER intact.
The slot number is the ABI: a single inserted or dropped line silently sends
every later call to the wrong function.
"""
import re, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'unix', 'winemetal_unix.c')
THUNKS = os.path.join(HERE, 'winemetal_thunks.h')


def ret_fields():
    """Map each unixcall params struct to its OUTPUT members.

    An unrouted call must not leave its output untouched. Returning
    NOT_IMPLEMENTED while the caller's `ret` still held uninitialised stack gave
    DXMT a non-zero garbage handle (0x1002a), which sailed through its own
    `cbz` null check and faulted on the store that followed. A clean NULL turns
    that into the graceful failure the null check was written for.
    """
    text = open(THUNKS).read()
    out = {}
    for m in re.finditer(r'struct\s+(unixcall_[A-Za-z0-9_]+)\s*\{(.*?)\}\s*;', text, re.S):
        members = re.findall(r'^\s*[A-Za-z_][A-Za-z0-9_ *]*?\**\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*;',
                             m.group(2), re.M)
        outs = [x for x in members if x == 'ret' or x.startswith('ret_')]
        if outs:
            out[m.group(1)] = outs
    return out

# Handlers with a remote implementation. Anything absent here fails by name.
ROUTED = {
    '_NSObject_retain', '_NSObject_release', '_NSArray_object', '_NSArray_count',
    '_MTLCopyAllDevices', '_MTLDevice_newCommandQueue',
    # capability group -- these gate shader and format selection downstream
    '_MTLDevice_registryID', '_MTLDevice_hasUnifiedMemory',
    '_MTLDevice_recommendedMaxWorkingSetSize', '_MTLDevice_currentAllocatedSize',
    '_MTLDevice_supportsBCTextureCompression', '_MTLDevice_supportsFamily',
    '_WMTGetOSVersion', '_MTLDevice_name',
    '_MTLDevice_setShouldMaximizeConcurrentCompilation',
    # shader path: metallib BYTES cross, the DispatchData container does not
    '_MTLDevice_newLibrary', '_MTLLibrary_newFunction',
    '_MTLLibrary_newFunctionWithConstants',
    # descriptors travel verbatim; the host builds the real Metal objects
    '_MTLDevice_newRenderPipelineState', '_MTLDevice_newComputePipelineState',
    '_MTLDevice_newDepthStencilState', '_MTLDevice_newSamplerState',
    # resources and sync
    '_MTLDevice_newBuffer', '_MTLBuffer_updateContents', '_MTLDevice_newSharedEvent',
    # presentation seam: the HWND stays guest-local, the host layer crosses
    '_CreateMetalViewFromHWND', '_ReleaseMetalView',
    '_MetalLayer_nextDrawable', '_MetalDrawable_texture',
    # frame path: persistent command buffer and encoder, packed batches replayed
    '_MTLCommandQueue_commandBuffer', '_MTLCommandBuffer_renderCommandEncoder',
    '_MTLRenderCommandEncoder_encodeCommands', '_MTLCommandEncoder_endEncoding',
    '_MTLCommandBuffer_commit', '_MTLCommandBuffer_waitUntilCompleted',
    '_MTLCommandBuffer_status', '_MTLCommandBuffer_presentDrawable',
    '_MTLCommandBuffer_encodeSignalEvent', '_MTLCommandBuffer_encodeWaitForEvent',
    '_MTLSharedEvent_signaledValue', '_MTLTexture_width', '_MTLTexture_height',
    # textures: without these a pass has colour but no depth while the pipeline
    # declares one, and building the encoder aborts inside Metal's compiler
    '_MTLDevice_newTexture', '_MTLTexture_replaceRegion', '_MTLTexture_newTextureView',
    # layer properties: the guest's requested drawable size must reach the host
    '_MetalLayer_setProps', '_MetalLayer_getProps',
    # a real title's delta: blit stream and the device queries it divides by
    '_MTLCommandBuffer_blitCommandEncoder', '_MTLBlitCommandEncoder_encodeCommands',
    '_MTLDevice_minimumLinearTextureAlignmentForPixelFormat',
    '_MTLDevice_supportsTextureSampleCount', '_MTLBuffer_newTexture',
    '_MTLCommandEncoder_setLabel',
}

# Deliberately NOT guarded -- these belong on whichever machine runs the guest.
#
# airconv's DXBC->AIR compiler (thunk_SM50*): CPU-only, takes and returns plain
# memory, touches no Metal object. Its output reaches the host as BYTES when a
# library is created, which is also why the shader cache stays here.
#   KNOWN GAP: compilation is targeted at the LOCAL device's feature set. On the
#   VM that device reports no GPU family at all and falls back to the Apple7
#   table (ml752), while the remote host is Apple9. Host capability reporting has
#   to drive that selection before shaders are routed.
#
# The object families below are guest-local BY DESIGN, and routing them was what
# put untagged pointers on the wire:
#   CacheReader/CacheWriter  - the on-disk (SQLite) shader cache, guest storage
#   DispatchData             - a byte container; the bytes get copied into the
#                              RPC, so the container itself must never travel
#   NSAutoreleasePool        - an ObjC scope on the calling thread
#   SharedEventListener      - dispatches callbacks onto guest threads
#   NSString                 - guest-side string storage
#
# Also local: calls whose iOS implementation is a safe no-op that WRITES ITS
# OUTPUTS. Guarding them was worse than running them -- the guard returns
# without touching the output fields, and these have outputs that are not named
# `ret`, so the caller read uninitialised stack. WMTQueryDisplaySettingForLayer
# is called ONCE PER FRAME and yields `version`, `colorspace` and `edr_value`;
# a version that changes at random makes DXMT reconfigure presentation
# repeatedly. Their layer dereferences are all inside `#if !TARGET_OS_IOS`, so
# running them locally never touches a host handle.
LOCAL_OK = re.compile(r'^(thunk_SM50|CacheReader_|CacheWriter_|DispatchData_|'
                      r'NSAutoreleasePool_|SharedEventListener_|NSString_|'
                      r'WMTSetMetalShaderCachePath|WMTQueryDisplaySettingForLayer|'
                      r'MetalLayer_getEDRValue|DeveloperHUDProperties_|'
                      r'WMTGetPrimaryDisplayId|WMTGetDisplayDescription)')
#
# ⛔ A call may only be listed above if it NEVER DEREFERENCES ITS HANDLE on iOS.
# In remote mode every handle is a host pointer, so running such a call locally
# dereferences another machine's address. MetalLayer_setColorSpace,
# MTLCommandBuffer_logs and MTLCommandEncoder_setLabel were briefly listed here
# and crashed the instant a window was created. The two that remain are safe:
# WMTGetPrimaryDisplayId ignores its handle entirely and returns CGMainDisplayID,
# and WMTGetDisplayDescription's body is #if !TARGET_OS_IOS.

def main():
    text = open(SRC).read()
    rets = ret_fields()
    # struct used by each handler, read from its own body
    bodies = dict(re.findall(r'^(_[A-Za-z0-9_]+)\(void \*obj\) \{\s*\n\s*struct (unixcall_[A-Za-z0-9_]+) \*params = obj;',
                             text, re.M))
    m = re.search(r'const void \*__wine_unix_call_funcs\[\] = \{(.*?)\n\};', text, re.S)
    if not m:
        sys.exit('dispatch table not found in %s' % SRC)
    raw = re.findall(r'^\s*(&?[A-Za-z0-9_]+|NULL)\s*,', m.group(1), re.M)

    out, n, guarded = [], 0, set()
    for e in raw:
        if e == 'NULL' or not e.startswith('&'):
            continue
        name = e.lstrip('&')
        if name.startswith('_rmg'):
            name = '_' + name[len('_rmg_'):]
        if name in ROUTED or LOCAL_OK.match(name.lstrip('_')):
            continue
        # A declaration is enough to take the address; several handlers are
        # defined in another translation unit and only declared here.
        if not re.search(r'^\s*(static\s+)?(NTSTATUS\s+)?%s\(' % re.escape(name), text, re.M):
            sys.exit('unresolvable handler: %s' % name)
        st = bodies.get(name)
        zero = rets.get(st, []) if st else []
        body = ['static NTSTATUS _rmg%s(void *obj) {' % name,
                '  if (wmtr_enabled()) {']
        if zero:
            body.append('    struct %s *p = obj;' % st)
            for f in zero:
                body.append('    p->%s = 0;   /* never hand back uninitialised stack */' % f)
        body += ['    return wmtr_unimplemented("%s");' % name[1:],
                 '  }',
                 '  return %s(obj);' % name, '}']
        out += body
        guarded.add(name)
        n += 1

    # Rewrite the table entries too. Emitting guards without owning the table
    # let the two drift: names that became local kept pointing at wrappers that
    # were no longer generated. The generator owns both or neither.
    def fix_table(m):
        body = m.group(1)
        def one(mm):
            lead, name = mm.group(1), mm.group(2)
            base = '_' + name[len('_rmg_'):] if name.startswith('_rmg_') else name
            want = '_rmg' + base if base in guarded else base
            return '%s&%s,' % (lead, want)
        return m.group(0)[:m.start(1)-m.start(0)] + \
               re.sub(r'^(\s*)&(_[A-Za-z0-9_]+),\s*$', one, body, flags=re.M) + \
               m.group(0)[m.end(1)-m.start(0):]

    text2 = re.sub(r'const void \*__wine_unix_call_funcs\[\] = \{(.*?)\n\};',
                   fix_table, text, flags=re.S)
    text2 = re.sub(r'const void \*__wine_unix_call_wow64_funcs\[\] = \{(.*?)\n\};',
                   fix_table, text2, flags=re.S)
    if text2 != text:
        open(SRC, 'w').write(text2)

    dst = os.path.join(HERE, 'unix', 'wmt_remote_guard.h')
    with open(dst, 'w') as f:
        f.write('/* GENERATED by gen_remote_guard.py from the unix dispatch table.\n'
                ' * Regenerate whenever __wine_unix_call_funcs[] or the routed set changes.\n'
                ' * Do not hand-edit. */\n\n')
        f.write('\n'.join(out) + '\n')
    print('wrote %s with %d guards (%d routed, SM50 thunks intentionally local)'
          % (os.path.relpath(dst, HERE), n, len(ROUTED)))

main()
