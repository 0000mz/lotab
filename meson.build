project('tab_manager', 'c', 'cpp', 'swift', 'objc',
  version : '0.1',
  default_options : ['warning_level=3', 'cpp_std=c++17'])

lws_dep = dependency('libwebsockets')
cjson_dep = dependency('libcjson')
carbon_dep = dependency('appleframeworks', modules : 'Carbon')
cocoa_dep = dependency('appleframeworks', modules : 'Cocoa')
argparse_dep = dependency('argparse')

# --- GUI ---
# Link required frameworks for a SwiftUI app
apple_frameworks = dependency('appleframeworks', modules : ['Foundation', 'AppKit', 'SwiftUI'])

app_src = [
  'app/TabManagerApp.swift',
  'app/ContentView.swift'
]

daemon_src = [
  'daemon/main.c',
  'daemon/statusbar.m'
]

daemon_deps = [lws_dep, cjson_dep, carbon_dep, cocoa_dep, argparse_dep]

info_plist_path = meson.current_source_dir() / 'app/Info.plist'
tab_manager_linker_args = ['-Xlinker', '-sectcreate', '-Xlinker', '__TEXT', '-Xlinker', '__info_plist', '-Xlinker', info_plist_path]

app_target = executable('TabManager',
           app_src,
           dependencies : apple_frameworks,
           link_args : tab_manager_linker_args,
           install : true)

# --- Daemon ---
# Pass the absolute path of the built TabManager binary to the daemon at build time via config.h
conf_data = configuration_data()
conf_data.set_quoted('APP_PATH', app_target.full_path())
configure_file(output : 'config.h',
               configuration : conf_data)

engine_lib = static_library('daemon_engine',
    'daemon/engine.c',
    dependencies : [lws_dep, cjson_dep],
    install : false)

engine_dep = declare_dependency(
  link_with : engine_lib,
  include_directories : include_directories('daemon'),
  dependencies : [lws_dep, cjson_dep]
)

executable('daemon', daemon_src, dependencies : [daemon_deps, engine_dep], install : true)

# --- Sanitizer Variants ---
sanitizers = {
  'address': '_asan',
  'thread' : '_tsan'
}

foreach s, suffix : sanitizers
  # App Variant
  app_var = executable('TabManager' + suffix,
             app_src,
             dependencies : apple_frameworks,
             override_options : ['b_sanitize=' + s],
             link_args : tab_manager_linker_args,
             install : true)

  # Daemon Variant
  conf_var_data = configuration_data()
  conf_var_data.set_quoted('APP_PATH', app_var.full_path())
  conf_var_output = 'config' + suffix + '.h'
  configure_file(output : conf_var_output,
                 configuration : conf_var_data)

  executable('daemon' + suffix, daemon_src + ['daemon/engine.c'],
             dependencies : daemon_deps,
             c_args : ['-DSANITIZER_CONFIG_H="' + conf_var_output + '"'],
             override_options : ['b_sanitize=' + s],
             install : true)
endforeach

# --- Unit Tests ---
gtest_dep = dependency('gtest', main : true, required : false)
if gtest_dep.found()
  e_test = executable('engine_test',
                      ['tests/engine_test.cpp'],
                      dependencies : [gtest_dep, engine_dep])
  test('engine_test', e_test)
else
  message('gtest not found, skipping unit tests')
endif

# --- Tests ---
uv_prog = find_program('uv', required : false)
if uv_prog.found()
  test('ui_test_quit', uv_prog,
       args : ['run', meson.current_source_dir() / 'tests/ui_test_quit.py', meson.current_build_dir() / 'daemon'],
       is_parallel : false)
else
  message('uv not found, skipping python UI tests')
endif
