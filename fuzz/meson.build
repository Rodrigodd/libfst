# libFuzzer targets.  Not built by default, see fuzz/README.md:
#
#     CC=clang meson setup build-fuzz -Dfuzzing=true -Db_sanitize=address,undefined \
#         -Db_lundef=false
#     meson compile -C build-fuzz
#     ./build-fuzz/fuzz/fuzz-roundtrip fuzz/corpus/

m_dep = cc.find_library('m', required: false)

libfst_fuzzers = [
    'roundtrip',
]

foreach fuzzer : libfst_fuzzers
    executable(
        'fuzz-' + fuzzer,
        'fuzz_' + fuzzer + '.c',
        dependencies: [libfst_dep, m_dep],
        c_args: ['-fsanitize=fuzzer'],
        link_args: ['-fsanitize=fuzzer'],
    )
endforeach
