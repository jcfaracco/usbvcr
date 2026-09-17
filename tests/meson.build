check_dep = dependency('check', required : false)
if not check_dep.found()
  cc = meson.get_compiler('c')
  check_dep = cc.find_library('check', required : false)
endif

if check_dep.found()
  test_data_dir = meson.current_source_dir() / 'data'

  test_usb_yaml = executable(
    'test_usb_yaml',
    'lib/test_usb_yaml.c',
    files(
      '../src/lib/usb_yaml.c',
      '../src/lib/log.c',
      '../src/lib/utils.c',
    ),
    include_directories : [lib, core],
    c_args : ['-DTEST_DATA_DIR="' + test_data_dir + '"'],
    dependencies : [check_dep, yaml_dep, glib_dep],
  )

  test('usb_yaml', test_usb_yaml)
endif
