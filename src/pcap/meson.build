# usb_pcap_filter: standalone usbmon pcap reader/filter (needs libpcap).
pcap_dep = dependency('libpcap', required : false)
if not pcap_dep.found()
  cc = meson.get_compiler('c')
  pcap_dep = cc.find_library('pcap', required : false)
endif

executable(
  'usb_pcap_filter',
  files('usb_pcap.c'),
  include_directories : include_directories('.'),
  dependencies : pcap_dep,
  install : true,
)
