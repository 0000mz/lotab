project('tab_manager', 'c', 'swift',
  version : '0.1',
  default_options : ['warning_level=3'])

lws_dep = dependency('libwebsockets')

executable('daemon',
           'daemon/main.c',
           dependencies : lws_dep,
           install : true)

# --- GUI ---
apple_frameworks = dependency('appleframeworks', modules : ['Foundation', 'AppKit', 'SwiftUI'])

app_src = [
  'app/Sources/TabManager/TabManagerApp.swift',
  'app/Sources/TabManager/ContentView.swift'
]

executable('TabManager',
           app_src,
           dependencies : apple_frameworks,
           install : true)
