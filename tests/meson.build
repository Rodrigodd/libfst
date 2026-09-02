libfst_tests = [
    'write_and_read',
    'empty_file',
    'empty_section',
    'flush_context',
    'no_value_change_section',
    'reader_truncated_file',
    'value_before_first_time_change',
    'varlen_before_first_time_change',
    'utility_escape',
    'writer_metadata',
    'writer_hierarchy',
    'enum_table',
    'vector_value_change',
    'reader_iter_blocks',
    'writer_misc_api',
    'reader_value_at_time',
    'wide_vector_value_change',
    'word_aligned_vector_value_change',
    'process_hier_fac_mask',
    'real_value_from_frame',
    'fastlz_pack_value_change',
    'value_change_handle_zero',
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
