
sigfm_sources = ['sigfm.cpp']

opencv = dependency('opencv4', required: true, version: '>=4.5.0')
doctest = dependency('doctest', required: false, version: ['>=2.0.0', '<3.0.0'])

libsigfm = static_library('sigfm',
        sigfm_sources,
        dependencies: [opencv],
)
if doctest.found()
    sigfm_tests = executable('sigfm-tests', ['./tests.cpp'], dependencies: [doctest, opencv], link_with: [libsigfm])
endif
