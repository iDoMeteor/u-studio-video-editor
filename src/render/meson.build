# The command line, as a library so tests drive it with the test drop-in.
libustudio_render_cli = static_library(
  'ustudio_render_cli',
  files('render_cli.cpp', 'proxy_command.cpp', 'gpu_probe_command.cpp'),
  dependencies: [libustudio_dropins_dep],
  install: false,
)
libustudio_render_cli_dep = declare_dependency(
  link_with: libustudio_render_cli,
  dependencies: [libustudio_dropins_dep],
)

executable(
  'u-studio-render',
  files('main.cpp'),
  dependencies: [libustudio_engine_dep, drop_ins_dep, libustudio_render_cli_dep],
  export_dynamic: dropin_any_module,
  link_whole: dropin_any_module ? [libustudio_core, libustudio_engine, libustudio_dropins] : [], # as src/app
  install: true,
)
