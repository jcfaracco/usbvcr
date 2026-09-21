check_dep = dependency('check', required : false)
if not check_dep.found()
  cc = meson.get_compiler('c')
  check_dep = cc.find_library('check', required : false)
endif

if check_dep.found()
  test_version = executable(
    'test_version',
    'lib/test_version.c',
    files(
      '../src/lib/version.c',
      '../src/lib/utils.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep],
  )
  test('version', test_version)

  test_yaml_loader = executable(
    'test_yaml_loader',
    'lib/test_yaml_loader.c',
    files(
      '../src/lib/yaml_loader.c',
      '../src/lib/usb_node.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep, yaml_dep],
  )
  test('yaml_loader', test_yaml_loader)
endif

# TODO: canonical-dialect tests (yaml_loader / codec / flow / ValidationSuite).
