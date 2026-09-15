client_sources = files(
  'main.c',
  'transport.c',
  'usb_core.c',
  'descriptor.c',
  'fuzz.c',
  'fuzz_wrap.c',
  'record.c',
  'log.c',
  'backends/hid_keyboard.c',
)

# Pure POSIX sockets client — no external library dependencies.
executable(
  'usb_redir_hello',
  client_sources,
  include_directories : include_directories('.'),
  install : true,
)
