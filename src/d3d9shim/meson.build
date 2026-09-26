# The i386 D3D9 shim (WOW64_DESIGN.md 8.5).
#
# It is an i386-only module by construction: the wire format in
# d3d9shim_ops.h can only be written by a 32-bit caller (guest pointers are
# uint32_t), and _d3d9_init refuses a caller whose pointer width is not 4.
# Building it for any other architecture would produce a module that cannot
# talk to its own unix half, so it is simply not built there.
#
# It links NOTHING from DXMT: no winemetal, no util, no dxmt.  Its whole
# import set is KERNEL32 / USER32 / GDI32 / api-ms-win-crt-* plus ntdll for
# the unix-call binding, which is the check WOW64_DESIGN.md 8.8-1 asks for.
# lib_winecrt0 supplies __wine_init_unix_call and the dispatcher; both come
# from src/winemetal/meson.build, which subdir()s before this one.
#
# The module is built as d3d9shim.dll and INSTALLED AS d3d9.dll by
# build/dxmt-ios/build-pe.sh, which installs the emulated frontend as
# d3d9-emulated.dll in the same pass -- the A/B pair the madeira-d3d9.txt knob
# selects between.  Two meson targets cannot both be called `d3d9`.

if cpu_family == 'x86'

d3d9shim_src = [
  'd3d9shim_main.c',
  'd3d9shim_object.c',
  'd3d9shim_custom.c',
  'd3d9shim_window.c',
  'd3d9shim_fpu.c',
  'd3d9shim_arena.c',
  'd3d9shim_lock.c',
  # generated from d3d9_api.py by gen_d3d9_thunks.py -- regenerate, do not edit
  'd3d9shim_thunks.c',
]

d3d9shim_dll = shared_library('d3d9shim', d3d9shim_src,
  name_prefix         : '',
  dependencies        : [ lib_winecrt0, lib_ntdll ],
  include_directories : include_directories('.'),
  install             : true,
  install_dir         : windows_native_install_dir,
  install_tag         : 'runtime-metal3',
  vs_module_defs      : 'd3d9.def',
  link_args           : [ '-L' + windows_native_install_dir ],
)

if wine_builtin_dll
custom_target('postprocess_libd3d9shim',
  input : d3d9shim_dll,
  output: 'd3d9shim.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : d3d9shim_dll,
  build_by_default : true
)
endif

endif
