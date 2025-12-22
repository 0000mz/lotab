project('tab_manager', 'c', 'swift',
  version : '0.1',
  default_options : ['warning_level=3'])

lws_dep = dependency('libwebsockets')
carbon_dep = dependency('appleframeworks', modules : 'Carbon')

# --- GUI ---
# Link required frameworks for a SwiftUI app
apple_frameworks = dependency('appleframeworks', modules : ['Foundation', 'AppKit', 'SwiftUI'])

app_src = [
  'app/TabManagerApp.swift',
  'app/ContentView.swift'
]

app_target = executable('TabManager',
           app_src,
           dependencies : apple_frameworks,
           install : true)

# --- Daemon ---
# Pass the absolute path of the built TabManager binary to the daemon at build time via config.h
conf_data = configuration_data()
conf_data.set_quoted('APP_PATH', app_target.full_path())
configure_file(output : 'config.h',
               configuration : conf_data)

executable('daemon',
           'daemon/main.c',
           dependencies : [lws_dep, carbon_dep],
           install : true)

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
             install : true)

  # Daemon Variant
  conf_var_data = configuration_data()
  conf_var_data.set_quoted('APP_PATH', app_var.full_path())
  conf_var_output = 'config' + suffix + '.h'
  configure_file(output : conf_var_output,
                 configuration : conf_var_data)

  executable('daemon' + suffix,
             'daemon/main.c',
             dependencies : [lws_dep, carbon_dep],
             c_args : ['-DSANITIZER_CONFIG_H="' + conf_var_output + '"'],
             override_options : ['b_sanitize=' + s],
             install : true)
endforeach
