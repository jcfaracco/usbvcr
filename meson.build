project(
  'usb_mentorship',
  'c',
  version : '0.1.0',
  license : 'MIT',
  default_options : [
    'warning_level=2',
    'c_std=c11',
    'buildtype=debugoptimized',
  ],
  meson_version: '>= 1.1.0',
)

if get_option('enable_coverage')
    # Check if gcovr is available
    gcovr_bin = find_program('gcovr', required: true)

    # Set manually the compilation flags
    cc = meson.get_compiler('c')
    if cc.get_id() == 'gcc' or cc.get_id() == 'clang'
        add_project_arguments('--coverage', language: 'c')
        add_project_link_arguments('--coverage', language: 'c')
    endif
endif

# Expose POSIX APIs (getaddrinfo, struct timeval, send, close, ...) under -std=c11.
add_project_arguments('-D_DEFAULT_SOURCE -Wpedantic', language : 'c')

subdir('src')
subdir('tests')
