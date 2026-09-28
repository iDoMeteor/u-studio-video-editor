# One folder per drop-in (ADR-013); each builds only when its option isn't
# 'disabled' (ADR-014). A drop-in folder's meson.build defines
#   dropin_<name>_dep       its built-in library (for 'builtin'), and
#   installs libustudio-dropin-<name>.so to dropin_install_dir (for 'module').
fs = import('fs')

builtin_dropin_names = []
builtin_dropin_deps = []
# A module resolves the app's symbols against the running program, so the
# programs export theirs when any drop-in is a module (src/app, src/render),
# or when separately packaged ones may be installed (dropin_extension_dir).
dropin_any_module = get_option('dropin_extension_dir') != ''
known_dropin_names = ['effects', 'titles'] # one dropin_<name> option each
foreach name : known_dropin_names
  mode = get_option('dropin_' + name)
  if mode == 'disabled'
    continue
  endif
  if not fs.is_dir(name)
    error('-Ddropin_' + name + '=' + mode + ', but drop-ins/' + name + '/ is not in this tree')
  endif
  subdir(name)
  if mode == 'module'
    dropin_any_module = true
  endif
  if mode == 'builtin'
    builtin_dropin_names += name
    builtin_dropin_deps += get_variable('dropin_' + name + '_dep')
  endif
endforeach

# drop_ins.h: the one link from src/ back to drop-ins (doc 15, "Where the
# code lives"), generated so src/ never names a drop-in itself.
declarations = ''
list = ''
foreach name : builtin_dropin_names
  declarations += 'extern "C" const UStudioDropInDescription *ustudio_dropin_' + name + '_describe(void);\n'
  list += '        ustudio_dropin_' + name + '_describe(),\n'
endforeach
drop_ins_conf = configuration_data()
drop_ins_conf.set('DECLARATIONS', declarations)
drop_ins_conf.set('LIST', list)
known = ''
foreach name : known_dropin_names
  known += '        "' + name + '",\n'
endforeach
drop_ins_conf.set('KNOWN', known)
configure_file(input: 'drop_ins.h.in', output: 'drop_ins.h', configuration: drop_ins_conf)

drop_ins_dep = declare_dependency(
  include_directories: include_directories('.'),
  dependencies: builtin_dropin_deps + [libustudio_dropins_dep],
)

# Layer rules inside drop-ins (doc 02, doc 15), enforced by the build like
# src/app's app-boundary-check: tools/dropin_boundary_check.sh.
dropin_boundary_check = custom_target(
  'dropin-boundary-check',
  output: 'dropin-boundary-check.stamp',
  command: [find_program('sh'), meson.project_source_root() / 'tools' / 'dropin_boundary_check.sh',
            meson.project_source_root(), '@OUTPUT@'],
  build_by_default: true,
)
