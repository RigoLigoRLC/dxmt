winemetal_src = [
  '../winemetal/winemetal_thunks.c',
  '../winemetal/unix/winemetal_unix.c',
  '../winemetal/unix/cache.c',
]
winemetal_link_depends = []

winemetal_ld_args = [
  '-weak_framework', 'CoreFoundation', '-weak_framework', 'Metal',
  '-framework', 'MetalFX', '-weak_framework', 'Foundation',
  '-weak_framework', 'CoreGraphics', '-weak_framework', 'QuartzCore',
  '-weak_framework', 'ColorSync',   '-weak_framework', 'Cocoa',
  '-lsqlite3',
]

presentation_feedback = static_library('presentation_feedback',
  '../winemetal/unix/presentation_feedback.c',
  c_args : ['-ObjC', '-fobjc-arc', '-fblocks'],
  pic : true,
  install : false,
)

winemetal_dll = shared_library('winemetal', winemetal_src,
  link_whole          : [ presentation_feedback ],
  name_prefix         : '',
  dependencies        : [ airconv_dep_darwin ],
  include_directories : [ dxmt_include_path, include_directories('.') ],
  c_args              : ['-ObjC', '-DDXMT_NATIVE=1'],
  link_args           : winemetal_ld_args,
  build_rpath         : '@loader_path/:@loader_path/../../',
)

winemetal_dep = declare_dependency(
  link_with           : [ winemetal_dll ],
  include_directories : [ include_directories('../winemetal') ],
)
