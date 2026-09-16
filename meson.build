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
)

# Expose POSIX APIs (getaddrinfo, struct timeval, send, close, ...) under -std=c11.
add_project_arguments('-D_DEFAULT_SOURCE', language : 'c')

subdir('src')
subdir('tests')
