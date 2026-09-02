project(
    'libfst',
    'c',
    version: '1.0.0',
)

pkg = import('pkgconfig')
cc = meson.get_compiler('c')

add_project_arguments('-D_GNU_SOURCE', language: 'c')

# Dependencies

zlib_req = '>=1.2.0'
bzip2_req = '>=1.0.0'
glib_req = '>=2.64.0' # only used for tests

zlib_dep = dependency('zlib', version: zlib_req)

bzip2_dep = dependency('bzip2', version: bzip2_req, required: false)
if not bzip2_dep.found()
    # pkg-config files for bzip2 aren't installed on all distributions,
    # try to manually find the library as a fallback
    bzip2_dep = cc.find_library('bz2', has_headers: 'bzlib.h')
endif

glib_dep = dependency('glib-2.0', version: glib_req, required: false)

thread_dep = dependency('threads', required: false)

# Check if functions exist

config = configuration_data()

functions = {
    'fseeko': 'stdio.h',
    'realpath': 'stdlib.h',
}

foreach function, include_file : functions
    exists = cc.has_function(
        function,
        args: '-D_GNU_SOURCE',
        prefix: '#include <@0@>'.format(include_file),
    )
    config.set('HAVE_' + function.to_upper(), exists)
endforeach

# Generate config.h

config.set('FST_WRITER_PARALLEL', thread_dep.found())
config.set('HAVE_LIBPTHREAD', thread_dep.found())

configure_file(output: 'config.h', configuration: config)
config_inc = include_directories('.')

add_project_arguments('-DFST_INCLUDE_CONFIG', language: 'c')

# Clang source-based coverage
#
# With -Dllvm_coverage=true the tests write raw profiles into the build
# directory and the `coverage-json` target turns them into an llvm-cov JSON
# report:
#
#     CC=clang meson setup build-cov -Dllvm_coverage=true
#     meson test -C build-cov
#     ninja -C build-cov coverage-json

llvm_coverage = get_option('llvm_coverage')

if llvm_coverage
    if cc.get_id() != 'clang'
        error('llvm_coverage requires clang, configure the build directory with CC=clang')
    endif
    if get_option('b_coverage')
        error('llvm_coverage and b_coverage are mutually exclusive')
    endif

    llvm_profdata = find_program('llvm-profdata')
    llvm_cov = find_program('llvm-cov')
    coverage_json_script = find_program('scripts/llvm_coverage_json.py')

    add_project_arguments('-fprofile-instr-generate', '-fcoverage-mapping', language: 'c')
    add_project_link_arguments('-fprofile-instr-generate', language: 'c')

    profraw_dir = meson.project_build_root() / 'profraw'

    add_test_setup(
        'coverage',
        env: {'LLVM_PROFILE_FILE': profraw_dir / '%p-%m.profraw'},
        is_default: true,
    )
endif


# libFuzzer targets, see fuzz/README.md
#
# The library is instrumented with -fsanitize=fuzzer-no-link as well, otherwise
# libFuzzer only sees the coverage of the target itself and cannot steer.

fuzzing = get_option('fuzzing')

if fuzzing
    if cc.get_id() != 'clang'
        error('fuzzing requires clang, configure the build directory with CC=clang')
    endif

    add_project_arguments('-fsanitize=fuzzer-no-link', language: 'c')
    add_project_link_arguments('-fsanitize=fuzzer-no-link', language: 'c')
endif

subdir('src')
subdir('tests')

if fuzzing
    subdir('fuzz')
endif

if llvm_coverage
    coverage_objects = [libfst] + libfst_test_exes

    run_target(
        'coverage-json',
        command: [
            coverage_json_script,
            '--llvm-profdata', llvm_profdata,
            '--llvm-cov', llvm_cov,
            '--profraw-dir', profraw_dir,
            '--output', meson.project_build_root() / 'coverage.json',
            '--ignore-filename-regex', '^/usr/',
            coverage_objects,
        ],
        depends: coverage_objects,
    )
endif
