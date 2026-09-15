client_sources = files(
  'main.c',
  'connection.c',
  'usb_core.c',
)

client_deps = []

# Optional: SPICE client + glib, used when talking to a real QEMU SPICE server.
glib_dep = dependency('glib-2.0', required : false)
if glib_dep.found()
  client_deps += glib_dep
endif

spice_dep = dependency('spice-client-glib-2.0', required : false)
if spice_dep.found()
  client_deps += spice_dep
endif

executable(
  'usb_redir_hello',
  client_sources,
  include_directories : include_directories('.'),
  dependencies : client_deps,
  install : true,
)
