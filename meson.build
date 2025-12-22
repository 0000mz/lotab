project('tab_manager_daemon', 'c',
  version : '0.1',
  default_options : ['warning_level=3'])

lws_dep = dependency('libwebsockets')

executable('daemon',
           'daemon/main.c',
           dependencies : lws_dep,
           install : true)
