subdir('util')
subdir('airconv')

if dxmt_native
subdir('nativemetal')
else
subdir('winemetal')
endif

subdir('dxmt')
subdir('dxgi')
subdir('d3d11')
subdir('d3d10')

# The Direct3D 9 frontend is a Windows module only: it is built against wine's
# import libraries and reaches Metal through winemetal's unix calls, so there is
# no native (macOS) variant of it. Which architectures actually get built, and
# which of the built DLLs are installed into the app bundle, is decided by
# build/dxmt-ios/build-pe.sh (--targets / --install), not here.
#
# MADEIRA (WOW64_DESIGN.md section 8): dxmt_madeira_native is the exception --
# there the frontend IS the native half, compiled iOS-arm64 into
# libdxmt_unix.a behind a 32-bit shim, and the comment above was the only
# thing excluding it (section 8.2(a)). The objects that actually ship are
# produced by build/dxmt-ios/build.sh rather than by meson; this arm keeps the
# two descriptions of the configuration from drifting.
if (not dxmt_native) or dxmt_madeira
subdir('d3d9')
endif

# MADEIRA (WOW64_DESIGN.md section 8.5): the i386 shim that stands in front of
# the native frontend. It is a Windows PE module and nothing else -- its own
# meson.build builds nothing unless cpu_family == 'x86' -- so it is excluded
# from the native build the same way the line above includes the frontend
# there. build-pe.sh installs the shim AS d3d9.dll and the emulated frontend
# as d3d9-emulated.dll, which is the A/B pair madeira-d3d9.txt selects between.
if not dxmt_native
subdir('d3d9shim')
endif

if get_option('enable_nvapi')
subdir('nvapi')
endif

if get_option('enable_nvngx')
subdir('nvngx')
endif