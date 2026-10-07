main_sources = files(
  'main.c',
  'core/transport.c',
  'core/usb_core.c',
  'core/descriptor.c',
  'fuzz/fuzz.c',
  'fuzz/fuzz_wrap.c',
  'backends/yaml_backend.c',
  'backends/pcap_backend.c',
  'engine/qemu_engine.c',
  'engine/local_engine.c',
)

backends = include_directories('backends')
engine = include_directories('engine')

# Go into individual module directories
subdir('core')
subdir('lib')
subdir('fuzz')

# Pure POSIX sockets client — no external library dependencies.
executable(
  'usbvcr',
  main_sources + lib_sources,
  include_directories : [lib, core, fuzz, backends, engine],
  dependencies: [yaml_dep, glib_dep],
  install : true,
)
