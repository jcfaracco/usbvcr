lib = include_directories('.')

lib_sources = files(
  'record.c',
  'log.c',
  'utils.c',
  'usb_node.c',
  'yaml_loader.c',
  'codec.c',
  'flow.c',
  'version.c',
)

glib_dep = dependency('glib-2.0')
if not glib_dep.found()
  cc = meson.get_compiler('c')
  glib_dep = cc.find_library('glib-2.0', required : true)
endif

yaml_dep = dependency('yaml-0.1', fallback: ['libyaml'], required : true)
if not yaml_dep.found()
  cc = meson.get_compiler('c')
  yaml_dep = cc.find_library('yaml-0.1', required : true)
endif

