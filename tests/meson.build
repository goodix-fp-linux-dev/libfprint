envs = environment()
# Enable debug messages and abort on warnings
envs.set('G_DEBUG', 'fatal-warnings')
envs.set('G_MESSAGES_DEBUG', 'all')

# Setup paths
envs.set('MESON_SOURCE_ROOT', meson.source_root())
envs.set('MESON_BUILD_ROOT', meson.build_root())
envs.prepend('LD_LIBRARY_PATH', join_paths(meson.build_root(), 'libfprint'))

# Set FP_DEVICE_EMULATION so that drivers can adapt (e.g. to use fixed
# random numbers rather than proper ones)
envs.set('FP_DEVICE_EMULATION', '1')

# Set a colon-separated list of native drivers we enable in tests
envs.set('FP_DRIVERS_WHITELIST', ':'.join([
    'virtual_image',
    'virtual_device',
    'virtual_device_storage',
]))

envs.set('NO_AT_BRIDGE', '1')

drivers_tests = [
    'aes2501',
    'aes3500',
    'elan',
    'elan-cobo',
    'elanmoc',
    'elanspi',
    'synaptics',
    'upektc_img',
    'uru4000-msv2',
    'uru4000-4500',
    'vfs0050',
    'vfs301',
    'vfs5011',
    'vfs7552',
    'goodixmoc',
    'nb1010',
    'egis0570',
    'fpcmoc',
]

if get_option('introspection')
  conf = configuration_data()
  conf.set('SRCDIR', meson.source_root())
  conf.set('BUILDDIR', meson.build_root())
  configure_file(configuration: conf,
      input: 'create-driver-test.py.in',
      output: 'create-driver-test.py')
endif

if get_option('introspection')
    envs.prepend('GI_TYPELIB_PATH', join_paths(meson.build_root(), 'libfprint'))
    virtual_devices_tests = [
        'virtual-image',
        'virtual-device',
    ]

    python3 = find_program('python3')
    unittest_inspector = find_program('unittest_inspector.py')
    umockdev_test = find_program('umockdev-test.py')

    foreach vdtest: virtual_devices_tests
        driver_name = '_'.join(vdtest.split('-'))
        if driver_name in drivers
            base_args = files(vdtest + '.py')
            suite = ['virtual-driver']

            r = run_command(unittest_inspector, files(vdtest + '.py'), check: true)
            unit_tests = r.stdout().strip().split('\n')

            if r.returncode() == 0 and unit_tests.length() > 0
                suite += vdtest
            else
                unit_tests = [vdtest]
            endif

            foreach ut: unit_tests
                ut_suite = suite
                ut_args = base_args
                if unit_tests.length() > 1
                    ut_args += ut
                    ut_suite += ut.split('.')[0]
                endif
                test(ut,
                    python3,
                    args: ut_args,
                    suite: ut_suite,
                    depends: libfprint_typelib,
                    env: envs,
                )
            endforeach
        else
            test(vdtest,
                find_program('sh'),
                args: ['-c', 'exit 77']
            )
        endif
    endforeach

    foreach driver_test: drivers_tests
        driver_name = driver_test.split('-')[0]
        driver_envs = envs
        driver_envs.set('FP_DRIVERS_WHITELIST', driver_name)

        if (driver_name in supported_drivers and
            gusb_dep.version().version_compare('>= 0.3.0'))
            test(driver_test,
                python3,
                args: [
                    umockdev_test.full_path(),
                    meson.current_source_dir() / driver_test,
                ],
                env: driver_envs,
                suite: ['drivers'],
                timeout: 15,
                depends: libfprint_typelib,
            )
        else
            test(driver_test,
                find_program('sh'),
                args: ['-c', 'exit 77']
            )
        endif
    endforeach
else
    warning('Skipping all driver tests as introspection bindings are missing')
    test('virtual-image',
        find_program('sh'),
        args: ['-c', 'exit 77']
    )

    foreach driver_test: drivers_tests
        test(driver_test,
            find_program('sh'),
            args: ['-c', 'exit 77']
        )
    endforeach
endif

test_utils = static_library('fprint-test-utils',
    sources: [
        'test-utils.c',
        'test-device-fake.c',
    ],
    dependencies: libfprint_private_dep,
    install: false)

unit_tests = [
    'fpi-device',
    'fpi-ssm',
    'fpi-assembling',
]

if 'virtual_image' in drivers
    unit_tests += [
        'fp-context',
        'fp-device',
    ]
endif

unit_tests_deps = { 'fpi-assembling' : [cairo_dep] }

test_config = configuration_data()
test_config.set_quoted('SOURCE_ROOT', meson.source_root())
test_config_h = configure_file(output: 'test-config.h', configuration: test_config)

foreach test_name: unit_tests
    if unit_tests_deps.has_key(test_name)
        missing_deps = false
        foreach dep: unit_tests_deps[test_name]
            if not dep.found()
                missing_deps = true
                break
            endif
        endforeach

        if missing_deps
            # Create a dummy test that always skips instead
            warning('Test @0@ cannot be compiled due to missing dependencies'.format(test_name))
            test(test_name,
                find_program('sh'),
                suite: ['unit-tests'],
                args: ['-c', 'exit 77'],
            )
            continue
        endif
        extra_deps = unit_tests_deps[test_name]
    else
        extra_deps = []
    endif

    basename = 'test-' + test_name
    test_exe = executable(basename,
        sources: [basename + '.c', test_config_h],
        dependencies: [ libfprint_private_dep ] + extra_deps,
        c_args: common_cflags,
        link_with: test_utils,
    )
    test(test_name,
        test_exe,
        suite: ['unit-tests'],
        env: envs,
    )
endforeach

# Run udev rule generator with fatal warnings
envs.set('UDEV_HWDB', udev_hwdb.full_path())
envs.set('UDEV_HWDB_CHECK_CONTENTS', default_drivers_are_enabled ? '1' : '0')
test('udev-hwdb',
     find_program('test-generated-hwdb.sh'),
     depends: udev_hwdb,
     env: envs)

gdb = find_program('gdb', required: false)
if gdb.found()
    libfprint_wrapper = [
        gdb.path(),
        '-batch',
        '-ex', 'run',
        '--args',
    ]
    add_test_setup('gdb',
        timeout_multiplier: 1000,
        exe_wrapper: libfprint_wrapper,
        env: [
            'LIBFPRINT_TEST_WRAPPER=' + ' '.join(libfprint_wrapper),
        ])
endif

valgrind = find_program('valgrind', required: false)
if valgrind.found()
    glib_share = glib_dep.get_pkgconfig_variable('prefix') / 'share' / glib_dep.name()
    glib_suppressions = glib_share + '/valgrind/glib.supp'
    libfprint_suppressions = '@0@/@1@'.format(meson.source_root(),
        files('libfprint.supp')[0])
    python_suppressions = '@0@/@1@'.format(meson.source_root(),
        files('valgrind-python.supp')[0])
    libfprint_wrapper = [
        valgrind.path(),
        '--tool=memcheck',
        '--leak-check=full',
        '--leak-resolution=high',
        '--error-exitcode=1',
        '--errors-for-leak-kinds=definite',
        '--track-origins=yes',
        '--show-leak-kinds=definite,possible',
        '--show-error-list=yes',
        '--suppressions=' + libfprint_suppressions,
        '--suppressions=' + glib_suppressions,
        '--suppressions=' + python_suppressions,
    ]
    add_test_setup('valgrind',
        timeout_multiplier: 15,
        exe_wrapper: libfprint_wrapper,
        env: [
            'G_SLICE=always-malloc',
            'UNDER_VALGRIND=1',
            'LIBFPRINT_TEST_WRAPPER=' + ' '.join(libfprint_wrapper),
        ])
endif
