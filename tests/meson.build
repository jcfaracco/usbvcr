check_dep = dependency('check', required : false)
if not check_dep.found()
  cc = meson.get_compiler('c')
  check_dep = cc.find_library('check', required : false)
endif

# TODO: canonical-dialect tests (yaml_loader / codec / flow / ValidationSuite).
# Legacy pcap-replay parser + its test removed.
