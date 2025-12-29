project('lotab', 'c', 'cpp', 'swift', 'objc',
  version : '0.1',
  default_options : ['warning_level=3', 'cpp_std=c++20'])

lws_proj = subproject('libwebsockets', default_options: ['default_library=static'])
lws_dep = lws_proj.get_variable('libwebsockets_dep')
cjson_proj = subproject('cjson')
cjson_dep = cjson_proj.get_variable('libcjson_dep')
carbon_dep = dependency('appleframeworks', modules : 'Carbon')
cocoa_dep = dependency('appleframeworks', modules : 'Cocoa')
argparse_dep = dependency('argparse')

engine_util_lib = static_library('daemon_util', ['daemon/util.c'], install: false)
engine_util_dep = declare_dependency(
  link_with : engine_util_lib,
  include_directories : include_directories('daemon'),
)

# --- GUI ---
# Link required frameworks for a SwiftUI app
apple_frameworks = dependency('appleframeworks', modules : ['Foundation', 'AppKit', 'SwiftUI'])

app_src = [
  'app/LotabApp.swift',
  'app/ContentView.swift'
]

daemon_engine_src = ['daemon/main.c']

daemon_deps = [lws_dep, cjson_dep, carbon_dep, cocoa_dep, argparse_dep]

info_plist_path = meson.current_source_dir() / 'app/Info.plist'
lotab_linker_args = ['-Xlinker', '-sectcreate', '-Xlinker', '__TEXT', '-Xlinker', '__info_plist', '-Xlinker', info_plist_path]
lotab_swift_args = ['-import-bridging-header', meson.current_source_dir() / 'app/bridge.h']
lotab_deps = [apple_frameworks, engine_util_dep]

app_target = executable('Lotab',
           app_src,
           dependencies : lotab_deps,
           link_args : lotab_linker_args,
           swift_args : lotab_swift_args,
           install : true)

# --- Daemon ---
# Pass the absolute path of the built Lotab binary to the daemon at build time via config.h
conf_data = configuration_data()
conf_data.set_quoted('APP_PATH', app_target.full_path())
configure_file(output : 'config.h',
               configuration : conf_data)


engine_lib = static_library('daemon_engine',
    ['daemon/engine.c', 'daemon/statusbar.m'],
    dependencies : [lws_dep, cjson_dep, cocoa_dep, carbon_dep, engine_util_dep],
    install : false)

engine_dep = declare_dependency(
  link_with : engine_lib,
  include_directories : include_directories('daemon'),
  dependencies : [lws_dep, cjson_dep, cocoa_dep, carbon_dep]
)

executable('lotab_daemon', daemon_engine_src,
           dependencies : [daemon_deps, engine_dep],
           install : true,
           install_rpath : get_option('prefix') / get_option('libdir'))

# --- Sanitizer Variants ---
sanitizers = {
  'address': '_asan',
  'thread' : '_tsan'
}

install_sanitizers = get_option('buildtype') != 'release'

foreach s, suffix : sanitizers
  # App Variant
  app_var = executable('Lotab' + suffix,
             app_src,
             dependencies : lotab_deps,
             swift_args : lotab_swift_args,
             override_options : ['b_sanitize=' + s],
             link_args : lotab_linker_args,
             install : install_sanitizers)

  # Daemon Variant
  conf_var_data = configuration_data()
  conf_var_data.set_quoted('APP_PATH', app_var.full_path())
  conf_var_output = 'config' + suffix + '.h'
  configure_file(output : conf_var_output,
                 configuration : conf_var_data)

  executable('lotab_daemon' + suffix, daemon_engine_src,
             dependencies : daemon_deps + [engine_dep],
             c_args : ['-DSANITIZER_CONFIG_H="' + conf_var_output + '"'],
             override_options : ['b_sanitize=' + s],
             install : install_sanitizers)
endforeach

# --- Unit Tests ---
gtest_dep = dependency('gtest', main : true, required : false)
if gtest_dep.found()
  e_test = executable('engine_test',
                      ['tests/engine_test.cpp'],
                      dependencies : [gtest_dep, engine_dep, lws_dep])
  test('engine_test', e_test)
else
  message('gtest not found, skipping unit tests')
endif

# --- Tests ---
uv_prog = find_program('uv', required : true)
if uv_prog.found()
  test('ui_test_quit', uv_prog,
       args : ['run', meson.current_source_dir() / 'tests/ui_test_quit.py', meson.current_build_dir() / 'lotab_daemon'],
       is_parallel : false)
  test('install_test', uv_prog,
       args : ['run', meson.current_source_dir() / 'tests/install_test.py'],
       is_parallel : false,
       timeout : 120)
else
  message('uv not found, skipping python UI tests')
endif

# --- Install Scripts / Data ---
# Configure com.mob.lotab.plist
# We need to install the daemon and the app to know where they end up.
# Configured plist will go to prefix/share/lotab/
pl_conf = configuration_data()
pl_conf.set('BINDIR', get_option('prefix') / get_option('bindir'))
pl_conf.set('APP_NAME', 'Lotab')

# Capture the user's home directory at configuration time
user_home = run_command('sh', '-c', 'echo $HOME', check: true).stdout().strip()
pl_conf.set('LOG_HOME', user_home)

configure_file(
  input : 'data/com.mob.lotab.plist.in',
  output : 'com.mob.lotab.plist',
  configuration : pl_conf,
  install : true,
  install_dir : get_option('datadir') / 'lotab'
)
