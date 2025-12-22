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
