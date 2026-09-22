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
      '../src/lib/yaml_tree.c',
      '../src/lib/schema.c',
      '../src/lib/flow.c',
      '../src/lib/usb_node.c',
      '../src/lib/codec.c',
      '../src/lib/version.c',
      '../src/lib/utils.c',
      '../src/lib/log.c',      
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('yaml_loader', test_yaml_loader)

  test_flow = executable(
    'test_flow',
    'lib/test_flow.c',
    files(
      '../src/lib/flow.c',
      '../src/lib/yaml_tree.c',
      '../src/lib/usb_node.c',
      '../src/lib/schema.c',
      '../src/lib/codec.c',
      '../src/lib/version.c',
      '../src/lib/utils.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('flow', test_flow)
endif

# TODO: canonical-dialect tests (codec / ValidationSuite).
