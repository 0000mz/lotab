project('tomlc99', 'c', default_options : ['warning_level=0'])

cc = meson.get_compiler('c')
lib = static_library('toml', 'toml.c', install: false)

tomlc99_dep = declare_dependency(
  link_with: lib,
  include_directories: include_directories('.')
)
