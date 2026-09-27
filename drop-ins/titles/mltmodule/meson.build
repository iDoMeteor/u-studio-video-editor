# libmltustudio.so: the ustudio_title MLT producer (ADR-012). Its own build
# directory, so the directory FactoryPolicy links modules from (IP4) holds
# only this module. Self-contained: everything but MLT, Pango, Cairo,
# fontconfig and libxml2 is linked in with hidden visibility, so it loads
# into any MLT program.
titles_mlt_install_dir = get_option('prefix') / get_option('libdir') / 'u-studio' / 'mlt'
# The static libraries' symbols stay inside the module too (GNU ld; a
# Windows DLL exports nothing unasked).
titles_mlt_link_args = meson.get_compiler('cpp').get_supported_link_arguments('-Wl,--exclude-libs,ALL')
titles_mlt_module = shared_module(
  'mltustudio',
  files('producer_ustudio_title.cpp'),
  include_directories: titles_inc,
  dependencies: [titles_render_dep, mlt_dep],
  gnu_symbol_visibility: 'hidden',
  link_args: titles_mlt_link_args,
  install: true,
  install_dir: titles_mlt_install_dir,
)
titles_mlt_build_dir = meson.current_build_dir()
