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

  test_utils = executable(
    'test_utils',
    'lib/test_utils.c',
    files(
      '../src/lib/utils.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep],
  )
  test('utils', test_utils)

  test_yaml_loader = executable(
    'test_yaml_loader',
    'lib/test_yaml_loader.c',
    files(
      '../src/lib/yaml/loader.c',
      '../src/lib/yaml/tree.c',
      '../src/lib/yaml/util.c',
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
      '../src/lib/yaml/tree.c',
      '../src/lib/yaml/util.c',
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

  test_descriptor = executable(
    'test_descriptor',
    'lib/test_descriptor.c',
    files(
      '../src/core/descriptor.c',
      '../src/core/usb_core.c',
      '../src/core/transport.c',
      '../src/lib/codec.c',
      '../src/lib/flow.c',
      '../src/lib/log.c',
      '../src/lib/pcap/pcap.c',
      '../src/lib/pcap/record.c',
      '../src/lib/schema.c',
      '../src/lib/yaml/loader.c',
      '../src/lib/yaml/tree.c',
      '../src/lib/yaml/util.c',
      '../src/lib/usb_node.c',
      '../src/lib/utils.c',
      '../src/lib/version.c',
      '../src/backends/yaml_backend.c',
    ),
    include_directories : [lib, core, backends],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('descriptor', test_descriptor)

  test_usb_core = executable(
    'test_usb_core',
    'core/test_usb_core.c',
    files(
      '../src/core/usb_core.c',
      '../src/core/transport.c',
      '../src/core/descriptor.c',
      '../src/lib/log.c',
      '../src/lib/pcap/pcap.c',
      '../src/lib/pcap/record.c',
    ),
    include_directories : [lib, core],
    dependencies : [check_dep, glib_dep, dependency('threads')],
  )
  test('usb_core', test_usb_core)

  test_transport = executable(
    'test_transport',
    'core/test_transport.c',
    files(
      '../src/core/transport.c',
      '../src/lib/log.c',
      '../src/lib/pcap/pcap.c',
      '../src/lib/pcap/record.c',
    ),
    include_directories : [lib, core],
    dependencies : [check_dep, glib_dep, dependency('threads')],
  )
  test('transport', test_transport)

  test_pcap_roundtrip = executable(
    'test_pcap_roundtrip',
    'integration/test_pcap_roundtrip.c',
    files(
      '../src/lib/pcap/pcap.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep],
  )
  test('pcap_roundtrip', test_pcap_roundtrip)

  test_record = executable(
    'test_record',
    'integration/test_record.c',
    files(
      '../src/lib/pcap/pcap.c',
      '../src/lib/pcap/record.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core],
    dependencies : [check_dep, glib_dep],
  )
  test('record', test_record)

  test_pcap_hid_capture = executable(
    'test_pcap_hid_capture',
    'integration/test_pcap_hid_capture.c',
    files(
      '../src/lib/pcap/pcap.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('pcap_hid_capture', test_pcap_hid_capture)

  test_pcap_backend_record = executable(
    'test_pcap_backend_record',
    'integration/test_pcap_backend.c',
    files(
      '../src/backends/pcap_backend.c',
      '../src/lib/pcap/pcap.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core, backends],
    dependencies : [check_dep, glib_dep],
  )
  test('pcap_backend_record', test_pcap_backend_record)

  test_pcap_backend_hid_capture = executable(
    'test_pcap_backend_hid_capture',
    'backends/test_pcap_backend.c',
    files(
      '../src/backends/pcap_backend.c',
      '../src/lib/pcap/pcap.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core, backends],
    dependencies : [check_dep, glib_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('pcap_backend_hid_capture', test_pcap_backend_hid_capture)

  test_yaml_backend = executable(
    'test_yaml_backend',
    'backends/test_yaml_backend.c',
    files(
      '../src/backends/yaml_backend.c',
      '../src/lib/yaml/loader.c',
      '../src/lib/yaml/tree.c',
      '../src/lib/yaml/util.c',
      '../src/lib/schema.c',
      '../src/lib/flow.c',
      '../src/lib/usb_node.c',
      '../src/lib/codec.c',
      '../src/lib/version.c',
      '../src/lib/utils.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core, backends],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('yaml_backend', test_yaml_backend)

  if have_local_engine
    test_ffs = executable(
      'test_ffs',
      'lib/test_ffs.c',
      files(
        '../src/lib/ffs/ffs.c',
        '../src/lib/log.c',
        '../src/backends/yaml_backend.c',
        '../src/lib/yaml/loader.c',
        '../src/lib/yaml/tree.c',
        '../src/lib/yaml/util.c',
        '../src/lib/schema.c',
        '../src/lib/flow.c',
        '../src/lib/usb_node.c',
        '../src/lib/codec.c',
        '../src/lib/version.c',
        '../src/lib/utils.c',
      ),
      include_directories : [lib, core, backends],
      dependencies : [check_dep, glib_dep, yaml_dep],
      c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
    )
    test('ffs', test_ffs)
  endif
endif

# TODO: canonical-dialect tests (codec / ValidationSuite).
