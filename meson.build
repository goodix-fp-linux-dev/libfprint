project('libfprint', [ 'c', 'cpp' ],
    version: '1.94.5',
    license: 'LGPLv2.1+',
    default_options: [
        'buildtype=debugoptimized',
        'warning_level=1',
        'c_std=gnu99',
    ],
    meson_version: '>= 0.49.0')

gnome = import('gnome')

libfprint_conf = configuration_data()
libfprint_conf.set_quoted('LIBFPRINT_VERSION', meson.project_version())

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
host_system = host_machine.system()
glib_min_version = '2.56'

glib_version_def = 'GLIB_VERSION_@0@_@1@'.format(
    glib_min_version.split('.')[0], glib_min_version.split('.')[1])
common_cflags = cc.get_supported_arguments([
    '-Wall',
    '-Wcast-align',
    '-Wformat-nonliteral',
    '-Wformat-security',
    '-Wformat=2',
    '-Wignored-qualifiers',
    '-Wlogical-op',
    '-Wmissing-declarations',
    '-Wmissing-format-attribute',
    '-Wmissing-include-dirs',
    '-Wmissing-noreturn',
    '-Wpointer-arith',
    '-Wshadow',
    '-Wswitch-enum',
    '-Wtype-limits',
    '-Wundef',
    '-Wunused',
    '-Werror=address',
    '-Werror=array-bounds',
    '-Werror=empty-body',
    '-Werror=init-self',
    '-Werror=int-to-pointer-cast',
    '-Werror=main',
    '-Werror=missing-braces',
    '-Werror=nonnull',
    '-Werror=redundant-decls',
    '-Werror=return-type',
    '-Werror=sequence-point',
    '-Werror=trigraphs',
    '-Werror=write-strings',
    '-fno-strict-aliasing',
    '-DGLIB_VERSION_MIN_REQUIRED=' + glib_version_def,
    '-DGLIB_VERSION_MAX_ALLOWED=' + glib_version_def,
    '-D_GNU_SOURCE',
    '-DG_LOG_DOMAIN="@0@"'.format(meson.project_name()),
])
c_cflags = cc.get_supported_arguments([
    '-Wimplicit-function-declaration',
    '-Wmissing-prototypes',
    '-Wnested-externs',
    '-Wold-style-definition',
    '-Wstrict-prototypes',
    '-Werror=implicit',
    '-Werror=pointer-to-int-cast',
])
add_project_arguments(common_cflags + c_cflags, language: 'c')
add_project_arguments(common_cflags, language: 'cpp')

# maintaining compatibility with the previous libtool versioning
# current = binary - interface
# revision = interface
soversion = 2
current = 0
revision = 0
libversion = '@0@.@1@.@2@'.format(soversion, current, revision)
versioned_libname = meson.project_name() + '-' + soversion.to_string()

# Dependencies
glib_dep = dependency('glib-2.0', version: '>=' + glib_min_version)
gio_dep = dependency('gio-unix-2.0', version: '>=' + glib_min_version)
gobject_dep = dependency('gobject-2.0', version: '>=' + glib_min_version)
gusb_dep = dependency('gusb', version: '>= 0.2.0')
mathlib_dep = cc.find_library('m', required: false)

# The following dependencies are only used for tests
cairo_dep = dependency('cairo', required: false)

# introspection scanning and Gio-2.0.gir
gobject_introspection = dependency('gobject-introspection-1.0', required: get_option('introspection'))

# Drivers
drivers = get_option('drivers').split(',')
virtual_drivers = [
    'virtual_image',
    'virtual_device',
    'virtual_device_storage',
]

default_drivers = [
    'upektc_img',
    'vfs5011',
    'vfs7552',
    'aes3500',
    'aes4000',
    'aes1610',
    'aes1660',
    'aes2660',
    'aes2501',
    'aes2550',
    'vfs101',
    'vfs301',
    'vfs0050',
    'etes603',
    'egis0570',
    'vcom5s',
    'synaptics',
    'elan',
    'elanmoc',
    'uru4000',
    'upektc',
    'upeksonly',
    'upekts',
    'goodixmoc',
    'goodixtls511',
    'nb1010',
    'fpcmoc',

    # SPI
    'elanspi',
]

# Not in default_drivers: pulls in OpenCV (see driver_helper_mapping's
# 'sigfm' helper below), which the rest of default_drivers does not
# require. Opt in explicitly with -Ddrivers=goodix533c or -Ddrivers=all.
all_drivers_only = [
    'goodix533c',
]

# FIXME: All the drivers should be fixed by adjusting the byte order.
# See https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/236
endian_independent_drivers = virtual_drivers + [
    'aes3500',
    'synaptics',
]

all_drivers = default_drivers + virtual_drivers + all_drivers_only

if drivers == [ 'all' ]
    drivers = all_drivers
endif

if drivers == [ 'default' ]
    drivers = default_drivers
endif

driver_helper_mapping = {
    'aes1610' : [ 'aeslib' ],
    'aes1660' : [ 'aeslib', 'aesx660' ],
    'aes2501' : [ 'aeslib' ],
    'aes2550' : [ 'aeslib' ],
    'aes2660' : [ 'aeslib', 'aesx660' ],
    'aes3500' : [ 'aeslib', 'aes3k' ],
    'aes4000' : [ 'aeslib', 'aes3k' ],
    'uru4000' : [ 'nss' ],
    'elanspi' : [ 'udev' ],
    'goodixtls511' : [ 'goodixtls' ],
    'goodix533c' : [ 'goodixtls', 'sigfm' ],
    'virtual_image'          : [ 'virtual' ],
    'virtual_device'         : [ 'virtual' ],
    'virtual_device_storage' : [ 'virtual' ],
}

driver_helpers = []
foreach driver : drivers
    if driver in driver_helper_mapping
        foreach helper : driver_helper_mapping[driver]
            if helper not in driver_helpers
                driver_helpers += helper
            endif
        endforeach
    endif
endforeach


if drivers.length() == 0 or drivers[0] == ''
    error('Cannot build libfprint without drivers, please specify a valid value for the drivers option')
endif

if drivers == all_drivers or drivers == default_drivers
    default_drivers_are_enabled = true
else
    default_drivers_are_enabled = true
    foreach driver: default_drivers
        if driver not in drivers
            default_drivers_are_enabled = false
            break
        endif
    endforeach
endif

udev_rules = get_option('udev_rules')
install_udev_rules = udev_rules.enabled()

optional_deps = []

# Set (and libsigfm declared) only when the 'sigfm' helper below actually
# runs, i.e. only when a driver that needs it (goodix533c) is enabled --
# OpenCV must never become a dependency of the whole libfprint build just
# because *some* driver happens to use SIGFM matching.
have_sigfm = false

# Resolve extra dependencies
foreach i : driver_helpers
    foreach d, helpers : driver_helper_mapping
        if i in helpers
            driver = d
            break
        endif
    endforeach

    if i == 'aes3k'
        imaging_dep = dependency('pixman-1', required: false)
        if not imaging_dep.found()
            error('pixman is required for @0@ and possibly others'.format(driver))
        endif

        libfprint_conf.set10('HAVE_PIXMAN', true)
        optional_deps += imaging_dep
    elif i == 'goodixtls'
        openssl_dep = dependency('openssl', required: false)
        if not openssl_dep.found()
            error('openssl is required for @0@ and possibly others'.format(driver))
        endif
        optional_deps += openssl_dep

        threads_dep = dependency('threads', required: false)
        if not threads_dep.found()
            error('threads is required for @0@ and possibly others'.format(driver))
        endif
        optional_deps += threads_dep
    elif i == 'nss'
        nss_dep = dependency('nss', required: false)
        if not nss_dep.found()
            error('nss is required for @0@ and possibly others'.format(driver))
        endif

        optional_deps += nss_dep
    elif i == 'udev'
        install_udev_rules = true

        gudev_dep = dependency('gudev-1.0', required: false)
        if not gudev_dep.found()
            error('udev is required for SPI support')
        endif

        libfprint_conf.set10('HAVE_UDEV', true)

        optional_deps += gudev_dep
    elif i == 'sigfm'
        have_sigfm = true

        # SIGFM: SIFT-based fingerprint matching for small sensors (used by
        # goodix533c's enroll/verify/identify). Use pkg-config only for the
        # include path; link only the specific OpenCV modules needed (the
        # full opencv pkg-config pulls in modules like viz/hdf that can have
        # missing transitive deps on some distros).
        opencv_pc = dependency('opencv5', required: false)
        if not opencv_pc.found()
            opencv_pc = dependency('opencv4', required: false)
        endif
        if not opencv_pc.found()
            error('opencv (opencv5 or opencv4) is required for @0@ and possibly others'.format(driver))
        endif
        opencv_includes = opencv_pc.partial_dependency(compile_args: true, includes: true)

        opencv_core = cpp.find_library('opencv_core')
        # OpenCV 5 renamed the features2d module to features.
        opencv_features2d = cpp.find_library('opencv_features2d', required: false)
        if not opencv_features2d.found()
            opencv_features2d = cpp.find_library('opencv_features')
        endif
        opencv_flann = cpp.find_library('opencv_flann')
        opencv_imgproc = cpp.find_library('opencv_imgproc')

        opencv_dep = declare_dependency(
            dependencies: [opencv_includes, opencv_core, opencv_features2d, opencv_flann, opencv_imgproc],
        )
        optional_deps += opencv_dep

        libsigfm = static_library('sigfm',
            'sigfm/sigfm.cpp',
            dependencies: [opencv_dep],
            cpp_args: ['-std=c++17'],
            install: false)
    endif
endforeach

if udev_rules.disabled()
    install_udev_rules = false
endif

if install_udev_rules
    udev_rules_dir = get_option('udev_rules_dir')
    if udev_rules_dir == 'auto'
        udev_dep = dependency('udev')
        udev_rules_dir = udev_dep.get_pkgconfig_variable('udevdir') + '/rules.d'
    endif
endif

supported_drivers = []
foreach driver: drivers
    if build_machine.endian() == 'little' or driver in endian_independent_drivers
        supported_drivers += driver
    else
        warning('Driver @0@ is not supported by big endian cpu @1@. Please, fix it!'.format(
            driver, build_machine.cpu()))
    endif
endforeach

if default_drivers_are_enabled and supported_drivers != drivers
    default_drivers_are_enabled = false
endif

root_inc = include_directories('.')

udev_hwdb = get_option('udev_hwdb')
if not udev_hwdb.disabled()
    # systemd v248 includes our autosuspend rules
    udev_versioned_dep = dependency('udev', version: '>= 248', required: false)
    udev_hwdb_dir = get_option('udev_hwdb_dir')
    if udev_hwdb.auto()
        if udev_versioned_dep.found()
            udev_hwdb_dir = ''
        endif
    else
        if udev_versioned_dep.found()
            warning('udev hwdb will be installed by both systemd and libfprint')
        endif
    endif

    if udev_hwdb_dir == 'auto'
        udev_dep = dependency('udev')
        udev_hwdb_dir = udev_dep.get_pkgconfig_variable('udevdir') + '/hwdb.d'
    endif
else
    udev_hwdb_dir = ''
endif

if get_option('gtk-examples')
    gnome = import('gnome')

    gtk_dep = dependency('gtk+-3.0', required: false)
    if not gtk_dep.found()
        error('GTK+ 3.x is required for GTK+ examples')
    endif
endif

# Some dependency resolving happens inside here
subdir('libfprint')

configure_file(output: 'config.h', configuration: libfprint_conf)

subdir('examples')
if get_option('doc')
    gnome = import('gnome')
    subdir('doc')
endif
if get_option('gtk-examples')
    subdir('demo')
endif

subdir('data')
subdir('tests')

pkgconfig = import('pkgconfig')
pkgconfig.generate(
    name: versioned_libname,
    description: 'Generic C API for fingerprint reader access',
    version: meson.project_version(),
    libraries: libfprint,
    requires: [gio_dep, gobject_dep],
    subdirs: versioned_libname,
    filebase: versioned_libname,
)

summary({'Drivers': drivers, }, section: 'Drivers')
