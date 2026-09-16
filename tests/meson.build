check_dep = dependency('check', required : false)
if not check_dep.found()
  cc = meson.get_compiler('c')
  check_dep = cc.find_library('check', required : false)
endif

if check_dep.found()
  test_data_dir = meson.current_source_dir() / 'data'

  test_yaml2 = executable(
    'test_yaml2',
    'lib/test_yaml2.c',
    files(
      '../src/lib/yaml2.c',
      '../src/lib/log.c',
      '../src/lib/utils.c',
    ),
    include_directories : [lib, core],
    c_args : ['-DTEST_DATA_DIR="' + test_data_dir + '"'],
    dependencies : [check_dep, yaml_dep],
  )

  test('yaml2', test_yaml2)
endif
