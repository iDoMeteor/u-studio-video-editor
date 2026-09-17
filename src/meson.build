# Include directory rooted at src/ itself, so every target below can keep
# using the existing "engine/mlt_engine.h"-style includes unchanged.
src_inc = include_directories('.')

subdir('core')
subdir('engine')
subdir('app')

if get_option('render_tool')
  subdir('render')
endif
