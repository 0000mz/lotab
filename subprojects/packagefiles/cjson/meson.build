project('cjson', 'c', version : '1.7.19', license : 'MIT')

headers = files('cJSON.h', 'cJSON_Utils.h')
sources = files('cJSON.c', 'cJSON_Utils.c')

libcjson = static_library('cjson',
  sources,
  install : true,
  c_args : ['-D_CRT_SECURE_NO_WARNINGS']
)

libcjson_dep = declare_dependency(
  include_directories : include_directories('.'),
  link_with : libcjson
)
