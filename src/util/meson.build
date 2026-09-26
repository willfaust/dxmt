util_src = files([
  'util_env.cpp',
  'util_string.cpp',
  # 'util_fps_limiter.cpp',
  # 'util_flush.cpp',
  # 'util_gdi.cpp',
  # 'util_luid.cpp',
  # 'util_matrix.cpp',
  # 'util_shared_res.cpp',
  # 'util_sleep.cpp',
  'util_bloom.cpp',
  # MADEIRA (WOW64_DESIGN.md, ml1070): the wait-on-address backend behind
  # dxmt::atomic_wait / atomic_notify_*, and the one line that says which
  # backend a session is on.
  'util_futex.cpp',

  'thread.cpp',

  'com/com_guid.cpp',
  'com/com_private_data.cpp',

  'config/config.cpp',

  'log/log.cpp',

  'sha1/sha1.c',
  'sha1/sha1_util.cpp',
])

# MADEIRA (WOW64_DESIGN.md section 8.2(d)): the Madeira native build has no
# user32 at all -- wsi_window_headless.cpp still calls ::GetClientRect and
# ::GetSystemMetrics, which do not exist below the Win32 boundary. The client
# size comes from a per-HWND cache the shim fills instead. Checked BEFORE the
# dxmt_native arm because dxmt_madeira_native implies dxmt_native.
if dxmt_madeira
  util_src += files([
    'wsi_monitor_headless.cpp',
    'wsi_window_madeira.cpp',
    'wsi_platform_madeira.cpp',
  ])
elif dxmt_native
  util_src += files([
    'wsi_monitor_headless.cpp',
    'wsi_window_headless.cpp',
    'wsi_platform_darwin.cpp',
  ])
elif host_machine.cpu_family() == 'aarch64' and host_machine.system() == 'windows'
  # Madeira iOS target: aarch64-windows PE, but running inside our iOS Wine
  # with no real window manager. Use headless wsi to stop user32 calls from
  # returning garbage.
  util_src += files([
    'wsi_monitor_headless.cpp',
    'wsi_window_headless.cpp',
    'wsi_platform_win32.cpp',
  ])
else
  util_src += files([
    'wsi_monitor_win32.cpp',
    'wsi_window_win32.cpp',
    'wsi_platform_win32.cpp',
  ])
endif

util_lib = static_library('util', util_src,
  include_directories : [ dxmt_include_path ],
)

# MADEIRA (WOW64_DESIGN.md, ml1070): util_futex.cpp calls RtlWaitOnAddress /
# RtlWakeAddressSingle / RtlWakeAddressAll.  ntdll rather than the
# api-ms-win-core-synch-l1-2-0 API set on purpose: kernelbase's WaitOnAddress
# IS RtlWaitOnAddress and its two Wake entry points are literal forwards to
# the Rtl ones, but ntdll.dll is always in the module list under its own name
# while the API-set name has to survive an apiset lookup -- and a silently
# failed apiset lookup is precisely the libc++ failure mode this replaces.
# Declared on util_dep so every module that links util gets the import, and
# only on a Windows host so the native (-DDXMT_MADEIRA) build is untouched.
util_futex_link_args = []
if host_machine.system() == 'windows'
  util_futex_link_args += [ '-lntdll' ]
endif

util_dep = declare_dependency(
  link_with           : [ util_lib ],
  link_args           : util_futex_link_args,
  include_directories : [ include_directories('.') ],
)
