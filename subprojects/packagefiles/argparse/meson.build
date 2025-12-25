project('argparse', 'c',
  version : '1.0.0',
  license : 'MIT')

libargparse = static_library('argparse',
  'argparse.c',
  install : false)

argparse_dep = declare_dependency(
  link_with : libargparse,
  include_directories : include_directories('.')
)

meson.override_dependency('argparse', argparse_dep)
