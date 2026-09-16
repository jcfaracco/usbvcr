lib = include_directories('.')

yaml_dep = dependency('yaml-0.1', fallback: ['libyaml'], required : true)
if not yaml_dep.found()
  cc = meson.get_compiler('c')
  yaml_dep = cc.find_library('yaml-0.1', required : true)
endif

