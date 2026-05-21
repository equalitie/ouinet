#/usr/bin/env bash

set -e

BUILD_DIR=
SRC_DIR=$(dirname $(dirname $0))
TEST_SPECS=()
SKIP_CMAKE_CONFIGURE=
EXCLUDED_TESTS=()

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
        --skip-cmake-configure)
            SKIP_CMAKE_CONFIGURE=y
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

TEST_PATH=$BUILD_DIR/test

# --- Define helper functions ---

function get_target  {( echo $1 | cut -d':' -f1 )}
function get_subtest {( echo $1 | cut -s -d':' -f2 )}

function cmake_configure {(
    configure_args=(
        -DCMAKE_BUILD_TYPE=Debug
        -DWITH_ASAN=OFF
        -DWITH_OUISYNC=OFF
        -DCORROSION_BUILD_TESTS=ON
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        # Uncomment for verbose
        #--trace
        # Use if Ouisync sources are to be taken from local directory
        #-DOUISYNC_SRC_DIR=$<PATH-TO-OUISYNC-SOURCE-DIR>
        -DOUINET_MEASURE_BUILD_TIMES=ON
    )
    if [ ! -d $BUILD_DIR ]; then
        mkdir -p 
    fi
    cd $BUILD_DIR
    cmake $SRC_DIR ${configure_args[@]}
)}

function cmake_build {(
    targets="$@"
    if [ -n "$targets" ]; then
        targets=${targets[@]/#/--target }
    fi
    local build_args=(
        # Uncomment for verbose
        # -v
        ${targets[@]}
        -j $(nproc)
    )
    cmake --build $BUILD_DIR ${build_args[@]}
)}

# If no tests are provided, this must run after configuration phase.
function collect_targets {(
    if [ -z "${TEST_SPECS[@]}" ]; then
        cmake --build $TEST_PATH --target help | grep '^\.\.\. test' | sed 's/^\.\.\. \(.*\)/\1/g' | grep -v '\.'
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

function run_test {(
    # TODO: Use the commented parts to run tests on Windows
    # binary_suffix=
    # launcher="wine"
    # binary_suffix=.exe
    # winepaths=(
    #     $build_dir
    #     /usr/lib/gcc/x86_64-w64-mingw32/14-win32
    # )
    # WINEPATH="$(IFS=';'; echo "${winepaths[*]}")"
        
    test=$1; shift
    no_gdb_tests=(
        # Uses address sanitizer which fails when run in gdb
        test_util
        # Internally uses SIGINT which messes with gdb. TODO: Maybe
        # it doesn't need to use SIGINT?
        test_cache_announcer
    )
    if [ ! $(which gdb) ] || is_in $test ${no_gdb_tests[@]}; then
        $TEST_PATH/$test "$@"
    else
        run_test_in_gdb $TEST_PATH/$test "$@"
    fi
)}

# --- Main ---

if [ "$SKIP_CMAKE_CONFIGURE" != "y" ]; then
    cmake_configure
fi

TEST_TARGETS=$(collect_targets)

if [ -z "${TEST_SPECS[*]}" ]; then
    TEST_SPECS="${TEST_TARGETS}"
fi

cmake_build ${TEST_TARGETS[@]}

TEST_RESULTS=()

RESULT_OK="OK"
RESULT_SKIPPED="SKIPPED"
RESULT_FAILED="FAILED"

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

    if ! run_test $target $subtest_arg --log_level=unit_scope; then
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
    printf '    %-8s %s\n' "$result" "$spec"
done

exit $EXIT_CODE
