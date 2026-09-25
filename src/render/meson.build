render_sources = files(
  'main.cpp',
)

executable(
  'u-studio-render',
  render_sources,
  dependencies: [libustudio_engine_dep, drop_ins_dep],
  export_dynamic: dropin_any_module,
  install: true,
)
