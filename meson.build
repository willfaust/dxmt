project('dxmt', ['c', 'cpp'], version : 'v0.1', meson_version: '>=1.3.0', default_options : [ 
  'cpp_std=c++20', 
  'warning_level=2', 
  'build.cpp_std=c++20',
  'b_ndebug=if-release',
])

fs = import('fs')

dxmt_native = get_option('dxmt_native')

# MADEIRA (WOW64_DESIGN.md section 8): the native ARM64 D3D9 frontend behind a
# 32-bit shim. A strict superset of dxmt_native -- same unixlib shape, same
# nativemetal boundary -- plus the guest arena, util_madeira_compat.h and the
# madeira wsi. Selecting it implies dxmt_native rather than being a third
# mode, so every existing `if dxmt_native` arm keeps applying.
dxmt_madeira = get_option('dxmt_madeira_native')
if dxmt_madeira
  dxmt_native = true
endif

cpp = meson.get_compiler('cpp')
cc = meson.get_compiler('c')
clang = meson.get_compiler('c', native: true)
clangpp = meson.get_compiler('cpp', native: true)

cpu_family = target_machine.cpu_family()

# ARM64EC: arm64ec-w64-mingw32-clang reports cpu_family() = 'aarch64' but is
# really an x86_64-callable hybrid PE — we need to install to arm64ec-windows
# (matches our wine/build-arm64ec layout) rather than aarch64-windows.
is_arm64ec = false
if cpu_family == 'aarch64'
  is_arm64ec = cpp.compiles(
    '#if !defined(__arm64ec__) && !defined(_M_ARM64EC)\n#error not arm64ec\n#endif\nint main(void){return 0;}',
    name: 'arm64ec target')
endif

dxmt_include_dirs = [
  './include',
  './libs'
]

dxmt_native_include_dirs = [
  './include/native/windows',
  './include/native/directx',
]

if dxmt_native
  dxmt_include_dirs += dxmt_native_include_dirs
endif

source_prefix_path = fs.relative_to(
    meson.project_source_root(),
    meson.project_build_root()
)

compiler_args = [
  '-Wimplicit-fallthrough',
  # gcc
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-cast-function-type', # Needed for GetProcAddress.
  # clang
  '-Wno-unused-private-field',
  '-Wno-microsoft-exception-spec',
  '-Wno-extern-c-compat',
  '-Wno-unused-const-variable',
  '-Wno-missing-braces',
  '-fblocks',
  '-fmacro-prefix-map=' + source_prefix_path + '/=',
]

if cpu_family == 'x86'
  compiler_args += [
    '-msse',
    '-msse2',
    '-msse3',
    '-mfpmath=sse',
  ]
endif

native_compiler_args = [
  '-Wimplicit-fallthrough',
  '-Wno-unused-parameter',
  '-Wno-unused-private-field',
  '-Wno-unused-const-variable',
  '-Wno-missing-braces',
  '-fblocks',
]

link_args = []

compiler_args += [
  '-DNOMINMAX',
  '-D_WIN32_WINNT=0xa00',
]

link_args += [
  '-static',
  # -static is sufficient for clang, while c runtime will be linked to api-ms-win-crt-* (ucrt)
  # '-static-libgcc',
  # '-static-libstdc++',
  # We need to set the section alignment for debug symbols to
  # work properly as well as avoiding a memcpy from the Wine loader.
  '-Wl,--file-alignment=4096',
]

# Wine's built-in back traces only work with dwarf4 symbols
if get_option('debug')
  compiler_args += [
    '-gdwarf-4',
  ]
endif

dxmt_include_path = include_directories(dxmt_include_dirs)
dxmt_native_include_path = include_directories(dxmt_native_include_dirs)

if not fs.is_file('include/native/directx/d3d11.h')
  error('Don\'t forget to clone submodules. `git submodule update --init --recursive`')
endif

if not dxmt_native
  res_ext = '.o'
  wrc = find_program('windres')
  wrc_generator = generator(wrc,
    output    : [ '@BASENAME@' + res_ext ],
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@' ],
  )
else
  wrc           = find_program('touch')
  wrc_generator = generator(wrc, output : [ '@BASENAME@_ignored.h' ], arguments : [ '@OUTPUT@' ] )
endif

xcrun = find_program('xcrun')

metalir_generator = generator(xcrun,
  output    : [ '@BASENAME@.air'],
  arguments : [ '-sdk', 'macosx', 'metal', '-o', '@OUTPUT@', '-c', '@INPUT@', '@EXTRA_ARGS@'],
)

metallib_generator = generator(xcrun,
  output    : [ '@BASENAME@.metallib'],
  arguments : [ '-sdk', 'macosx', 'metallib', '-o', '@OUTPUT@', '@INPUT@'],
)

xxd = find_program('xxd')

hexdump_generator = generator(xxd,
  output    : [ '@BASENAME@.h'],
  arguments : [ '-n', '@BASENAME@', '-i', '@INPUT@', '@OUTPUT@'],
)

dxmt_debug = get_option('dxmt_debug')

if dxmt_debug
add_project_arguments('-DDXMT_DEBUG=1', language: 'cpp')
endif

if dxmt_native
add_project_arguments('-DDXMT_NATIVE=1', language: 'cpp')
add_project_arguments('-DDXMT_NATIVE=1', language: 'c')
endif

# MADEIRA (WOW64_DESIGN.md section 8): every change to an imported DXMT file
# is gated on this, so a diff against the v0.4-d3d9 tag stays readable and
# every gate is listed in research/dxmt/LICENSE-MADEIRA.md (section 8.9 risk 2).
if dxmt_madeira
add_project_arguments('-DDXMT_MADEIRA=1', language: 'cpp')
add_project_arguments('-DDXMT_MADEIRA=1', language: 'c')
endif

add_project_arguments('-DDXMT_PAGE_SIZE=4096', language: 'cpp')

# On iOS all Metal storage modes that are Managed on macOS must become
# Shared — iOS has unified memory and raises an assertion for storageMode 1
# (Managed). Every PE module in this fork is loaded by Wine inside the iOS
# app, so the switch is a property of "cross-building a Windows module", not
# of the module's architecture.
#
# MADEIRA (WOW64_DESIGN.md section 7): this used to test cpu_family ==
# 'aarch64' because aarch64-windows was the only cross target. The 32-bit
# (WoW64) D3D9 path adds an i386-windows target whose cpu_family is 'x86', so
# that test left DXMT_IOS undefined for it — an i386 d3d11/d3d9 would then ask
# Metal for storageMode Managed and trip the same iOS assertion the remap
# exists to avoid. Keyed on the Windows host system instead, which covers
# i386, aarch64 and arm64ec alike and still excludes the native macOS build
# (dxmt_native, host system 'darwin').
#
# MADEIRA (WOW64_DESIGN.md section 8): dxmt_madeira_native is the one target
# that runs on iOS WITHOUT being a Windows module -- it is the native ARM64
# half of the D3D9 path, host system 'darwin'. The storage-mode remap is a
# property of the Metal device it talks to, not of the module's own format,
# so it has to apply here too or the frontend asks Metal for storageMode
# Managed and trips the same iOS assertion.
if host_machine.system() == 'windows' or dxmt_madeira
  add_project_arguments('-DDXMT_IOS=1', language: 'cpp')
  add_project_arguments('-DDXMT_IOS=1', language: 'c')
endif

dxmt_version = vcs_tag(
  command: ['git', 'describe', '--always'],
  input:  'version.h.in',
  output: 'version.h',
)

if not dxmt_native
  wine_builtin_dll = get_option('wine_builtin_dll')

  if cpu_family == 'x86'
    # Enable stdcall fixup on 32-bit
    link_args += [
      '-Wl,--enable-stdcall-fixup',
      '-Wl,--kill-at',
    ]
    # Fix stack alignment issues with mingw on 32-bit
    compiler_args += [
      '-mpreferred-stack-boundary=2'
    ]
    if wine_builtin_dll
      windows_native_install_dir = 'i386-windows'
    else
      windows_native_install_dir = 'syswow64'
    endif
    windows_builtin_install_dir = 'i386-windows'
  elif cpu_family == 'x86_64'
    if wine_builtin_dll
      windows_native_install_dir = 'x86_64-windows'
    else
      windows_native_install_dir = 'system32'
    endif
    windows_builtin_install_dir = 'x86_64-windows'
  elif cpu_family == 'aarch64'
    if is_arm64ec
      if wine_builtin_dll
        windows_native_install_dir = 'arm64ec-windows'
      else
        windows_native_install_dir = 'system32'
      endif
      windows_builtin_install_dir = 'arm64ec-windows'
    else
      if wine_builtin_dll
        windows_native_install_dir = 'aarch64-windows'
      else
        windows_native_install_dir = 'system32'
      endif
      windows_builtin_install_dir = 'aarch64-windows'
    endif
  else
    error('invalid cpu family')
  endif

  unix_install_dir = 'x86_64-unix'
else
  wine_builtin_dll = false
  windows_native_install_dir = ''
endif

add_project_arguments(cpp.get_supported_arguments(compiler_args), language: 'cpp')
add_project_arguments(clangpp.get_supported_arguments(native_compiler_args), language: 'cpp', native: true)
add_project_arguments(cc.get_supported_arguments(compiler_args), language: 'c')
add_project_arguments(clang.get_supported_arguments(native_compiler_args), language: 'c', native: true)
add_project_link_arguments(cpp.get_supported_link_arguments(link_args), language: 'cpp')
add_project_link_arguments(cc.get_supported_link_arguments(link_args), language: 'c')
if not dxmt_native
  add_project_arguments(['-arch', 'x86_64'], language: 'c',  native: true)
  add_project_arguments(['-arch', 'x86_64'], language: 'cpp',  native: true)
  add_project_link_arguments(['-arch', 'x86_64'], language: 'cpp',  native: true)
endif

subdir('libs')
subdir('src')

if get_option('enable_tests')
subdir('tests')
endif
