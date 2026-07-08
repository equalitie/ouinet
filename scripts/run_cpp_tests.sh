#/usr/bin/env bash

set -e

BUILD_DIR=
SRC_DIR=$(dirname $(dirname $0))
TEST_SPECS=()
EXCLUDED_TESTS=()
WITH_GDB=y # TODO: Only works with Linux binaries at the moment

function error {(
    echo "$@"
    exit 1
)}

# --- Parse and validate arguments ---

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --build-dir)
            BUILD_DIR="$2"; shift
            ;;
        --run-test)
            TEST_SPECS+=($2); shift
            ;;
        --exclude-test)
            EXCLUDED_TESTS+=($2); shift
            ;;
        --without-gdb)
            # Useful e.g. when ASAN is enabled
            WITH_GDB=n
            ;;
        *) error "Unknown option $1" ;;
    esac
    shift
done

if [ -z "$BUILD_DIR" ]; then
    error "Use --build-dir to point to the build directory"
fi

if [ ! -d $BUILD_DIR ]; then
    error "Build dir '$BUILD_DIR' does not exist"
fi
   
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    error "Cannot find the CMakeCache.txt in the build directory '$BUILD_DIR'"
fi

TEST_DIR=$BUILD_DIR/test

# --- Define helper functions ---

function get_target  {( echo $1 | cut -d':' -f1 )}
function get_subtest {( echo $1 | cut -s -d':' -f2 )}

# If no tests are provided, this must run after configuration phase.
function collect_targets {(
    if [ -z "${TEST_SPECS[@]}" ]; then
        cmake --build $TEST_DIR --target help | grep '^\.\.\. test' | sed 's/^\.\.\. \(.*\)/\1/g' | grep -v '\.'
    else
        for spec in ${TEST_SPECS[@]}; do echo "$(get_target $spec)"; done
    fi
)}

function is_in (
    item=$1; shift
    list="$@"
    for i in ${list[@]}; do
        if [ "$item" == "$i" ]; then
            return 0
        fi
    done
    return 1
)

function run_test_in_gdb {(
    gdb_args=(
        -return-child-result
        -ex='set print thread-events off'
        # Don't prompt "Quit anyway?" on error
        --batch -ex="set confirm off"
        -ex=run
        # Show backtrace on error
        -ex=bt
        -ex=quit
    )
    gdb ${gdb_args[@]} --args $@
)}

function run_linux_test {(
    test=$1; shift
    no_gdb_tests=(
        # Uses address sanitizer which fails when run in gdb
        test_util
        # Internally uses SIGINT which messes with gdb. TODO: Maybe
        # it doesn't need to use SIGINT?
        test_cache_announcer
    )

    # TODO: Enable detection of odr violation once the tests don't violate it
    export ASAN_OPTIONS=halt_on_error=0:detect_odr_violation=0

    if [ "$WITH_GDB" == n ] || [ ! $(which gdb) ] || is_in $test ${no_gdb_tests[@]} ; then
        $test "$@"
    else
        run_test_in_gdb $test "$@"
    fi
)}

function run_windows_test {(
    test=$1; shift
    winepaths=(
        $BUILD_DIR
        /usr/lib/gcc/x86_64-w64-mingw32/14-win32
    )
    export WINEPATH="$(IFS=';'; echo "${winepaths[*]}")"
    wine $test "$@"
)}

function is_configured_for_windows {(
    cmake_cache_file=$BUILD_DIR/CMakeCache.txt 
    if [ ! -f $cmake_cache_file ]; then
        error "CMakeCache.txt not found in $BUILD_DIR"
    fi
    if cat $cmake_cache_file | grep 'CMAKE_LINKER:FILEPATH' | grep mingw32 > /dev/null; then
        echo y
    else
        echo n
    fi
)}

function run_test {(
    bin=$1
    if [[ "$bin" == *.exe ]]; then
        run_windows_test "$@"
    else
        run_linux_test "$@"
    fi
)}

# --- Main ---

TEST_TARGETS=$(collect_targets)

if [ -z "${TEST_SPECS[*]}" ]; then
    TEST_SPECS="${TEST_TARGETS}"
fi

if [ "$(is_configured_for_windows)" == y ]; then
    BIN_SUFFIX=.exe
fi

TEST_RESULTS=()

RESULT_OK="OK"
RESULT_SKIPPED="SKIPPED"
RESULT_FAILED="FAILED"
RESULT_NOT_FOUND="NOT_FOUND"

for spec in ${TEST_SPECS[@]}; do
    target=$(get_target $spec)
    subtest=$(get_subtest $spec)

    if is_in $target ${EXCLUDED_TESTS[@]}; then
        echo "Skipped test $spec"
        TEST_RESULTS+=("$RESULT_SKIPPED" $spec)
        continue
    fi

    if [ -n "$subtest" ]; then
        subtest_arg="--run_test=$subtest"
    fi

    test_binary=$TEST_DIR/$target$BIN_SUFFIX

    if [ ! -f $test_binary ]; then
        TEST_RESULTS+=("$RESULT_NOT_FOUND" $spec)
        continue
    fi

    if ! run_test $test_binary $subtest_arg --log_level=unit_scope; then
        echo "Test $spec failed"
        TEST_RESULTS+=("$RESULT_FAILED" $spec)
    else
        TEST_RESULTS+=("$RESULT_OK" $spec)
    fi
done

EXIT_CODE=0

echo "Test summary:"
while [[ "${#TEST_RESULTS[@]}" -gt 0 ]]; do
    result=${TEST_RESULTS[0]}
    spec=${TEST_RESULTS[1]}
    TEST_RESULTS=(${TEST_RESULTS[@]:2})
    if [ "$result" == $RESULT_FAILED ]; then
        EXIT_CODE=1
    fi
    printf '    %-12s %s\n' "$result" "$spec"
done

exit $EXIT_CODE
