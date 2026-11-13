#!/usr/bin/env python3
"""Exercise the production 32-bit pointer wrapper with a shifted guest base."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/winemetal/unix/winemetal_unix.c').read_text()
start = source.index('#define WMT_WOW64_POINTER_WRAPPER(name, type, field)')
end = source.index('\n\nWMT_WOW64_POINTER_WRAPPER(', start)
macro = source[start:end]
harness = r'''
#include <assert.h>
#include <stdint.h>
static unsigned char guest_memory[32];
static void *expected;
typedef int NTSTATUS;
static void *UInt32ToPtr(uint32_t offset) {
  return offset ? guest_memory + offset : 0;
}
struct pointer { void *ptr; };
struct args { struct pointer output; int result; };
static NTSTATUS handler(void *obj) {
  struct args *a = obj;
  assert(a->output.ptr == expected);
  if (a->output.ptr) *(unsigned char *)a->output.ptr = 42;
  a->result = 17;
  return 0;
}
'''
main = r'''
WMT_WOW64_POINTER_WRAPPER(handler, args, output)
int main(void) {
  struct args a = { .output = { .ptr = (void *)(uintptr_t)7 }, .result = 0 };
  expected = guest_memory + 7;
  assert(handler_wow64(&a) == 0);
  assert(a.output.ptr == (void *)(uintptr_t)7);
  assert(a.result == 17 && guest_memory[7] == 42);
  a.output.ptr = 0; expected = 0; a.result = 0;
  assert(handler_wow64(&a) == 0);
  assert(a.output.ptr == 0 && a.result == 17);
}
'''
with tempfile.TemporaryDirectory(prefix='madeira-wow64-test-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-x', 'c', '-', '-o', binary],
                   input=harness + macro + '\n' + main, text=True, check=True)
    subprocess.run([binary], check=True)
print('32-bit embedded pointer translation passes')
