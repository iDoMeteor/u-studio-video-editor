# ADR-017: the OS-specific calls, one implementation file per OS. Std and OS
# headers only (no GTK, GLib or MLT), so every layer may use it.
if host_machine.system() != 'linux'
  error('src/platform/ has only Linux implementations so far (ADR-017: the Windows port is scheduled work)')
endif

# ADR-019: EGL is loaded at run time, so only its headers are needed here;
# without them the GL context is a stub and the GPU path never turns on.
platform_gl_source = (cpp.has_header('EGL/egl.h') and cpp.has_header('EGL/eglext.h')
                      ? 'gl_context_linux.cpp' : 'gl_context_none.cpp')

libustudio_platform = static_library(
  'ustudio_platform',
  files('process_linux.cpp', 'console_linux.cpp', 'clock_linux.cpp', 'gpu_linux.cpp', platform_gl_source),
  include_directories: src_inc,
  install: false,
)
libustudio_platform_dep = declare_dependency(
  link_with: libustudio_platform,
  include_directories: src_inc,
)

# Nothing outside src/platform/ may use POSIX/Linux-only headers or /proc,
# except the files doc 14's migration list still names (they move here when
# a change touches them, and leave the allowlist in the same commit).
platform_boundary_check = custom_target(
  'platform-boundary-check',
  output: 'platform-boundary-check.stamp',
  command: [find_program('sh'), meson.project_source_root() / 'tools' / 'platform_boundary_check.sh',
            meson.project_source_root(), '@OUTPUT@'],
  build_by_default: true,
)
