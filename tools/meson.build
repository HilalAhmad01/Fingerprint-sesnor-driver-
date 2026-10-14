if 'elanpress' in drivers
    executable('elanpress-eval',
        sources: [
            'elanpress-eval.c',
            '../libfprint/drivers/elanpress-match.c',
            '../libfprint/drivers/elanpress-sift.c',
        ],
        include_directories: include_directories('../libfprint/drivers'),
        dependencies: [ glib_dep, mathlib_dep ],
        c_args: common_cflags,
        install: false,
    )
endif
