libfst_tests = [
    'write_and_read',
    'empty_file',
    'empty_section',
    'flush_context',
    'no_value_change_section',
    'reader_truncated_file',
    'value_before_first_time_change',
    'varlen_before_first_time_change',
]

libfst_test_exes = []

foreach test : libfst_tests
    exe = executable(
        'test-' + test,
        test + '.c',
        dependencies: [libfst_dep, glib_dep],
    )

    libfst_test_exes += exe

    test(test, exe)
endforeach
