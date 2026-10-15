project(
  'elder-terms',
  'cpp',
  version: '0.0.1',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',
    'werror=true',
  ],
)

elder_terms_private_install_dir = get_option('libdir') / 'elder-terms'
elder_terms_private_install_rpath = '$ORIGIN/..'

boxrig_prepare = files('scripts/prepare-boxrig.mts')
boxrig_node = find_program('node')
run_command(boxrig_node, boxrig_prepare, meson.project_build_root(), check: true)
boxrig_sdk_target = custom_target(
  'boxrig-shared-sdk',
  output: 'boxrig-sdk.stamp',
  command: [boxrig_node, boxrig_prepare, meson.project_build_root()],
  build_by_default: true,
  build_always_stale: true,
)
boxrig_sdk_dir = meson.project_build_root() / 'boxrig/sdk'
boxrig_pkg_config = find_program('pkg-config')
boxrig_pkg_env = environment()
boxrig_pkg_env.prepend('PKG_CONFIG_PATH', boxrig_sdk_dir / 'lib/pkgconfig')
boxrig_deps = {}
foreach component : ['boxrig', 'boxrig-ui', 'boxrig-session', 'boxrig-posix', 'boxrig-gtk3']
  boxrig_deps += {component: declare_dependency(
    compile_args: run_command(boxrig_pkg_config, '--cflags', component,
      env: boxrig_pkg_env, check: true).stdout().strip().split(),
    link_args: run_command(boxrig_pkg_config, '--libs', component,
      env: boxrig_pkg_env, check: true).stdout().strip().split() +
      ['-Wl,-rpath,' + boxrig_sdk_dir / 'lib'],
    sources: boxrig_sdk_target,
  )}
endforeach

cardio_root_dir = meson.project_source_root() / 'deps/cardio'
cardio_include_dir = include_directories('deps/cardio/include')
cardio_gio_dep = dependency('gio-2.0')
cardio_liburing_dep = dependency('liburing')
cardio_cpp_args = [
  '-DCARDIO_SHARED_LIB=1',
  '-DCARDIO_WITH_GLIB=1',
  '-DCARDIO_WITH_GIO=1',
  '-DCARDIO_WITH_LINUX_IO_URING=1',
]

cardio_shared_library = shared_library(
  'cardio',
  'deps/cardio/samples/libcardio/libcardio.cpp',
  include_directories: cardio_include_dir,
  cpp_args: cardio_cpp_args + [
    '-DCARDIO_BUILD_SHARED_LIB=1',
  ],
  dependencies: [
    cardio_gio_dep,
    cardio_liburing_dep,
  ],
  install: true,
  install_dir: elder_terms_private_install_dir,
)

cardio_dep = declare_dependency(
  include_directories: cardio_include_dir,
  compile_args: cardio_cpp_args,
  link_with: cardio_shared_library,
  dependencies: [
    cardio_gio_dep,
    cardio_liburing_dep,
  ],
)

subdir('shared')
subdir('elder-terms')
subdir('elder-terms-vte')
subdir('po')

install_data(
  [
    'README.md',
    'README_ja.md',
    'packaging/copyright',
  ],
  install_dir: get_option('datadir') / 'doc/elder-terms',
)

foreach language : ['ja', 'en']
  install_data(
    'docs' / language / 'webdav.md',
    install_dir: get_option('datadir') / 'doc/elder-terms/docs' / language,
  )
endforeach

install_data(
  'docs/en/webdav-validation.md',
  install_dir: get_option('datadir') / 'doc/elder-terms/docs/en',
)
