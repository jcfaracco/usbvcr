client_sources = files(
  'main.c',
  'connection.c',
  'usb_core.c',
  'log.c',
)

# Pure POSIX sockets client — no external library dependencies.
executable(
  'usb_redir_hello',
  client_sources,
  include_directories : include_directories('.'),
  install : true,
)
