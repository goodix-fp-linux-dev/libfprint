
sigfm_sources = ['sigfm.cpp']

opencv = dependency('opencv4', required: true)
doctest = subproject('doctest').get_variable('doctest_dep')

libsigfm = static_library('sigfm',
        sigfm_sources,
        dependencies: [opencv],
)
sigfm_tests = executable('sigfm-tests', ['./tests.cpp'], dependencies: [doctest, opencv], link_with: [libsigfm])
