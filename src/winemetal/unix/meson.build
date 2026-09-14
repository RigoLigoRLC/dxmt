winemetal_unix_src = [
  'winemetal_unix.c',
  'cache.c',
]

if wine_build_path != ''
  winemac_so = join_paths(wine_build_path, 'dlls/winemac.drv/winemac.so')
  ntdll_so = join_paths(wine_build_path, 'dlls/ntdll/ntdll.so')
elif wine_install_path != ''
  winemac_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'winemac.so')
  ntdll_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'ntdll.so')
else
  error('One of the options -Dwine_build_path or -Dwine_install_path is required')
endif

winemetal_unix_ld_args = [
  '-Wl,-install_name,@rpath/winemetal.so',
  winemac_so, ntdll_so,
  '-weak_framework', 'CoreFoundation', '-weak_framework', 'Metal',
  '-framework', 'MetalFX', '-weak_framework', 'Foundation',
  '-weak_framework', 'CoreGraphics', '-weak_framework', 'QuartzCore',
  '-weak_framework', 'ColorSync',  '-weak_framework', 'Cocoa',
  '-lsqlite3',
]
winemetal_unix_link_depends = []

# Keep ARC local to the callback bridge. The existing WineMetal sources use
# manual retain/release. Use the native C compiler in Objective-C mode so this
# target inherits the same -arch flags as the rest of WineMetal.
presentation_bridge = static_library('presentation_bridge',
  'presentation_bridge.c',
  c_args : ['-ObjC', '-fobjc-arc', '-fblocks'],
  native : true,
  pic : true,
  install : false,
)

shared_library('winemetal', winemetal_unix_src,
  name_prefix         : '',
  dependencies        : [ airconv_dep_darwin ],
  link_whole          : [ presentation_bridge ],
  include_directories : dxmt_include_path,
  c_args              : ['-ObjC'],
  link_args           : winemetal_unix_ld_args,
  native              : true,
  name_suffix         : 'so',
  install_rpath       : '',
  build_rpath         : '@loader_path/:@loader_path/../../',
  override_options    : { 'b_asneeded' : false },
)

meson.add_install_script(
  'install.sh', get_option('strip').to_string(), unix_install_dir, 
  install_tag: 'runtime'
)