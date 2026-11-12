#!/usr/bin/env python3
"""Check that legacy and upstream Metal calls have stable dispatch slots."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
unix = (root / 'src/winemetal/unix/winemetal_unix.c').read_text()
thunks = (root / 'src/winemetal/winemetal_thunks.c').read_text()


def table(name):
    match = re.search(r'const void \*' + name + r'\[\] = \{(.*?)\n\};', unix, re.S)
    assert match, name
    return re.findall(r'^\s*(&(?:\w+)|NULL)\s*,', match.group(1), re.M)


native = table('__wine_unix_call_funcs')
wow64 = table('__wine_unix_call_wow64_funcs')
assert len(native) == len(wow64) == 170
assert native[127] == '&_madeira_ir_convert'
assert native[129] == '&_MadeiraMTLDevice_newResidencySet'
assert native[135] == '&_rmg_MadeiraMTLDevice_heapTextureSizeAndAlign'
assert native[139] == '&_MadeiraMTLDevice_heapBufferSizeAndAlign'
assert native[150] == wow64[150] == '&_d3d9_nop'
assert wow64[129] == '&_MadeiraMTLDevice_newResidencySet_wow64'

wrappers = {}
for match in re.finditer(r'\bUNIX_CALL\((1(?:5\d|6\d))\s*,', thunks):
    previous = thunks[:match.start()]
    functions = list(re.finditer(r'\n([A-Za-z_]\w*)\([^;{}]*\)\s*\{', previous))
    assert functions
    wrappers[int(match.group(1))] = functions[-1].group(1)
assert len(wrappers) == 17
pointer_calls = {
    'MTLCounterSampleBuffer_resolveCounterRange',
    'MTLCommandBuffer_blitCommandEncoderWithSampleBuffers',
    'MTLDevice_newTileRenderPipelineState',
    'MTLResidencySet_addAllocations', 'MTLResidencySet_removeAllocations',
    'MTLDevice_newHeap', 'MTLDevice_heapTextureSizeAndAlign',
    'MTLHeap_newBuffer', 'MTLHeap_newTexture', 'MTLDevice_newIndirectCommandBuffer',
    'MTLDevice_newLibraryWithSource', 'MTLTexture_getBytes',
}
routed = {'MTLDevice_newTileRenderPipelineState', 'MTLDevice_newIndirectCommandBuffer', 'MTLDevice_newLibraryWithSource', 'MTLCounterSampleBuffer_newTimestampBuffer', 'MTLCounterSampleBuffer_resolveCounterRange', 'MTLCommandBuffer_blitCommandEncoderWithSampleBuffers', 'MTLCommandBuffer_property', 'MTLDevice_heapBufferSizeAndAlign', 'MTLDevice_heapTextureSizeAndAlign', 'MTLDevice_newHeap', 'MTLHeap_newBuffer', 'MTLHeap_newTexture', 'MTLDevice_newResidencySet', 'MTLResidencySet_addAllocations',
          'MTLResidencySet_removeAllocations', 'MTLResidencySet_removeAllAllocations'}
for slot, name in wrappers.items():
    prefix = '&_' if name in routed else '&_rmg_upstream_'
    assert native[slot] == prefix + name, (slot, name, native[slot])
    expected = prefix + name + ('_wow64' if name in pointer_calls else '')
    assert wow64[slot] == expected, (slot, name, wow64[slot])
with tempfile.TemporaryDirectory(prefix='madeira-guard-test-') as tmp:
    src = root / 'src/winemetal'
    dest = Path(tmp) / 'src/winemetal'
    (dest / 'unix').mkdir(parents=True)
    for rel in ('gen_remote_guard.py', 'winemetal_thunks.h', 'unix/winemetal_unix.c', 'unix/wmt_remote_guard.h'):
        shutil.copyfile(src / rel, dest / rel)
    subprocess.run(['python3', str(dest / 'gen_remote_guard.py')], check=True, capture_output=True)
    for rel in ('unix/winemetal_unix.c', 'unix/wmt_remote_guard.h'):
        assert (dest / rel).read_bytes() == (src / rel).read_bytes(), rel
print('Metal dispatch slot mappings and guard regeneration pass')
