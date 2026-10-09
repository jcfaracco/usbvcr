project(
  'usbvcr',
  'c',
  version : '0.1.0',
  license : 'MIT',
  default_options : [
    'warning_level=3',
    'c_std=c11',
    'buildtype=debugoptimized',
  ],
  meson_version: '>= 1.1.0',
)

cc = meson.get_compiler('c')

# Probe gcovr regardless of enable_coverage, so `meson setup` always reports
# whether coverage reporting is possible on this system. Only hard-require it
# (fail configure) when coverage is actually requested.
gcovr_bin = find_program('gcovr', required: get_option('enable_coverage'))
if gcovr_bin.found()
    message('gcovr found: coverage reports available (-Denable_coverage=true)')
else
    message('gcovr not found: coverage reports unavailable (install gcovr to enable)')
endif

if get_option('enable_coverage')
    if cc.get_id() == 'gcc' or cc.get_id() == 'clang'
        add_project_arguments('--coverage', language: 'c')
        add_project_link_arguments('--coverage', language: 'c')
    endif
endif

# Expose POSIX APIs (getaddrinfo, struct timeval, send, close, ...) under -std=c11.
add_project_arguments('-D_DEFAULT_SOURCE -Wpedantic', language : 'c')

# local_engine/ffs use FunctionFS + ConfigFS (/sys/kernel/config,
# <linux/usb/functionfs.h>). Availability is probed on the running system
# regardless of enable_local_engine, so `meson setup` always reports whether
# the local engine CAN be built here; enable_local_engine only decides
# whether it IS, when available (mirrors the gcovr probe above).
fs = import('fs')
functionfs_header_found = host_machine.system() == 'linux' \
                          and cc.has_header('linux/usb/functionfs.h')
configfs_mounted = fs.is_dir('/sys/kernel/config')

if functionfs_header_found
    message('FunctionFS headers found' +
           (configfs_mounted ? ' (ConfigFS mounted)'
                              : ' (ConfigFS not mounted - needed at runtime, not build time)'))
else
    message('FunctionFS headers not found: local engine unavailable on this system')
endif

have_local_engine = get_option('enable_local_engine') and functionfs_header_found
if get_option('enable_local_engine') and not have_local_engine
    warning('local engine requested but unavailable on this system ' +
           '(FunctionFS headers missing or non-Linux): disabling')
endif
if have_local_engine
    add_project_arguments('-DUSBVCR_HAVE_LOCAL_ENGINE=1', language : 'c')
endif

subdir('src')
subdir('tests')
