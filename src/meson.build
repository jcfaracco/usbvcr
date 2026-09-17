main_sources = files(
  'main.c',
  'core/transport.c',
  'core/usb_core.c',
  'core/descriptor.c',
  'fuzz/fuzz.c',
  'fuzz/fuzz_wrap.c',
  'backends/hid_keyboard.c',
)

# Go into individual module directories
subdir('core')
subdir('lib')
subdir('fuzz')

# Pure POSIX sockets client — no external library dependencies.
executable(
  'usb_redir_hello',
  main_sources + lib_sources,
  include_directories : [lib, core, fuzz],
  dependencies: [yaml_dep, glib_dep],
  install : true,
)
