check_dep = dependency('check', required : false)
if not check_dep.found()
  cc = meson.get_compiler('c')
  check_dep = cc.find_library('check', required : false)
endif

if check_dep.found()
  test_env = environment()
  test_env.set('USBVCR_DATA_DIR', meson.project_source_root() / 'data')

  test_version = executable(
    'test_version',
    'lib/test_version.c',
    files(
      '../src/lib/version.c',
      '../src/lib/utils.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep],
  )
  test('version', test_version, env : test_env)

  test_utils = executable(
    'test_utils',
    'lib/test_utils.c',
    files(
      '../src/lib/utils.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep],
  )
  test('utils', test_utils, env : test_env)

  test_stream = executable(
    'test_stream',
    'lib/test_stream.c',
    files(
      '../src/lib/stream/stream_provider.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep],
  )
  test('stream', test_stream, env : test_env)

  test_disk_provider = executable(
    'test_disk_provider',
    'lib/test_disk_provider.c',
    files(
      '../src/lib/log.c',
    ),
    include_directories : [lib, core],
    dependencies : [check_dep, glib_dep],
    link_args : ['-Wl,--wrap=fstat64', '-Wl,--wrap=pread64', '-Wl,--wrap=pwrite64'],
  )
  test('disk_provider', test_disk_provider, env : test_env)

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
      '../src/lib/stream/video_parser.c',
      '../src/lib/stream/stream_provider.c',
      '../src/lib/storage/disk_provider.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('yaml_loader', test_yaml_loader, env : test_env)

  test_yaml_tree = executable(
    'test_yaml_tree',
    'lib/test_yaml_tree.c',
    files(
      '../src/lib/yaml/tree.c',
      '../src/lib/yaml/util.c',
      '../src/lib/schema.c',
      '../src/lib/codec.c',
      '../src/lib/usb_node.c',
      '../src/lib/version.c',
      '../src/lib/utils.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('yaml_tree', test_yaml_tree, env : test_env)

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
      '../src/lib/stream/video_parser.c',
      '../src/lib/stream/stream_provider.c',
      '../src/lib/storage/disk_provider.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('flow', test_flow, env : test_env)

  test_usb_node = executable(
    'test_usb_node',
    'lib/test_usb_node.c',
    files(
      '../src/lib/usb_node.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep],
  )
  test('usb_node', test_usb_node)

  test_codec = executable(
    'test_codec',
    'lib/test_codec.c',
    files(
      '../src/lib/yaml/util.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
    link_args : ['-Wl,--wrap=yaml_parser_initialize'],
  )
  test('codec', test_codec, env : test_env)

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
      '../src/lib/stream/video_parser.c',
      '../src/lib/stream/stream_provider.c',
      '../src/lib/storage/disk_provider.c',
      '../src/backends/yaml_backend.c',
    ),
    include_directories : [lib, core, backends],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('descriptor', test_descriptor, env : test_env)

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

  test_fuzz = executable(
    'test_fuzz',
    'core/test_fuzz.c',
    files(
      '../src/fuzz/fuzz.c',
    ),
    include_directories : [core, fuzz],
    dependencies : [check_dep, glib_dep],
  )
  test('fuzz', test_fuzz)

  test_pcap_roundtrip = executable(
    'test_pcap_roundtrip',
    'integration/test_pcap_roundtrip.c',
    files(
      '../src/lib/pcap/pcap.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib],
    dependencies : [check_dep, glib_dep],
    link_args : ['-Wl,--wrap=fopen', '-Wl,--wrap=fopen64', '-Wl,--wrap=fwrite', '-Wl,--wrap=fflush', '-Wl,--wrap=fseek'],
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

  test_record_ext = executable(
    'test_record_ext',
    'integration/test_record_ext.c',
    files(
      '../src/lib/pcap/pcap.c',
      '../src/lib/pcap/record.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core],
    dependencies : [check_dep, glib_dep],
  )
  test('record_ext', test_record_ext)

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
      '../src/lib/stream/video_parser.c',
      '../src/lib/stream/stream_provider.c',
      '../src/lib/storage/disk_provider.c',
      '../src/lib/log.c',
    ),
    include_directories : [lib, core, backends],
    dependencies : [check_dep, glib_dep, yaml_dep],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('yaml_backend', test_yaml_backend, env : test_env)

  engine_test_sources = files(
    '../src/engine/engine.c',
    '../src/engine/qemu_engine.c',
    '../src/core/usb_core.c',
    '../src/core/transport.c',
    '../src/core/descriptor.c',
    '../src/lib/log.c',
    '../src/lib/pcap/pcap.c',
    '../src/lib/pcap/record.c',
  )
  if have_local_engine
    engine_test_sources += files(
      '../src/engine/local_engine.c',
      '../src/lib/ffs/ffs.c',
      '../src/lib/utils.c',
      '../src/lib/version.c',
      '../src/backends/yaml_backend.c',
      '../src/lib/yaml/loader.c',
      '../src/lib/yaml/tree.c',
      '../src/lib/yaml/util.c',
      '../src/lib/schema.c',
      '../src/lib/flow.c',
      '../src/lib/usb_node.c',
      '../src/lib/codec.c',
      '../src/lib/stream/video_parser.c',
      '../src/lib/stream/stream_provider.c',
      '../src/lib/storage/disk_provider.c',
    )
  endif

  test_engine = executable(
    'test_engine',
    'engine/test_engine.c',
    engine_test_sources,
    include_directories : [lib, core, engine, backends],
    dependencies : [check_dep, glib_dep, yaml_dep, dependency('threads')],
    c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
  )
  test('engine', test_engine, env : test_env)

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
        '../src/lib/stream/video_parser.c',
        '../src/lib/stream/stream_provider.c',
        '../src/lib/storage/disk_provider.c',
      ),
      include_directories : [lib, core, backends],
      dependencies : [check_dep, glib_dep, yaml_dep],
      c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
      link_args : ['-Wl,--wrap=mount', '-Wl,--wrap=umount', '-Wl,--wrap=opendir'],
    )
    test('ffs', test_ffs, env : test_env)

    test_local_engine = executable(
      'test_local_engine',
      'engine/test_local_engine.c',
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
        '../src/lib/stream/video_parser.c',
        '../src/lib/stream/stream_provider.c',
        '../src/lib/storage/disk_provider.c',
      ),
      include_directories : [lib, core, engine, backends],
      dependencies : [check_dep, glib_dep, yaml_dep],
      c_args : ['-DTEST_DATA_DIR="' + meson.current_source_dir() + '/data"'],
    )
    test('local_engine', test_local_engine, env : test_env)
  endif
endif

# TODO: canonical-dialect tests (codec / ValidationSuite).
