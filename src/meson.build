# Include directory rooted at src/ itself, so every target below can keep
# using the existing "engine/engine_sync.h"-style includes unchanged.
src_inc = include_directories('.')

subdir('core')
subdir('engine')
subdir('dropins')
# src/app and src/render are entered from the top-level meson.build, after
# drop-ins/: their programs link the built-in drop-ins.
