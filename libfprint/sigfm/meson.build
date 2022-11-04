
sigfm_sources = ['sigfm.cpp']

opencv = dependency('opencv4', required: true)

libsigfm = static_library('sigfm',
        sigfm_sources,
        dependencies: [opencv]
        )
