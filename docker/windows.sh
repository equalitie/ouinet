#!/bin/bash

set -e

host=
isolation=--isolation=process
host_core_count=
run_cpp_tests=()
with_ouisync=n
host_ouisync_dir=
excluded_test_targets=()

source $(dirname $0)/util.sh windows

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --host|-H)
            host="$2"; shift
            ;;
        --host-is-vm)
            # Use this when Windows is not running on "bare metal".
            isolation=--isolation=hyperv
            export DOCKER_BUILDKIT=0
            # TODO: Determine these values from the host
            host_core_count=15
            mem="-m 32G" # Keep at least 2G per core
            ;;
        --exclude-test)
            excluded_test_targets+=($2); shift
            ;;
        --run-cpp-test)
            run_cpp_tests+=($2); shift
            ;;
        --with-ouisync)
            with_ouisync=y
            ;;
        --use-ouisync-dir)
            host_ouisync_dir=$2; shift;
            ;;
        --clean) clean=y ;;
        *) error "Unknown option $1" ;;
    esac
    shift
done

echo "Using host \"$host\""

if [ -z "$host_core_count" ]; then
    host_core_count=$(ssh $host 'cmd /s /c echo %NUMBER_OF_PROCESSORS%' | tr -d '[:space:]')
fi

image_name=$(choose_docker_image_name)
container_name=$(choose_docker_container_name)

work_dir=/opt
ouinet_dir=$work_dir/ouinet
build_dir=$work_dir/build
test_dir=$build_dir/test

function container_id() (
    container_name=$1
    if [ -z "$container_name" ]; then
        echo "'container_id' function requires container name as argument";
        exit 1
    fi
    dock ps -a -q -f name=$container_name
)

function copy_sources (
    local srcdir=$1
    local dstdir=$2

    local exclude=(
        .git
        build
        target
        android
    )

    docker_rsync ${exclude[@]/#/-e } $srcdir $dstdir
)

dock build $isolation -t $image_name $isolation $mem - < docker/Dockerfile.windows-builder

function start_container (
    dock run $isolation $mem -d --rm --name $container_name --cpus $host_core_count -p 22 $image_name \
        bash -c 'sleep 24h'
)

if [ -z "$(container_id $container_name)" ]; then
    start_container
fi

trap on_exit EXIT
function on_exit() {
    exe -w $ouinet_dir -i -t bash
}

exe mkdir -p $ouinet_dir $build_dir
copy_sources . $ouinet_dir

if [ -n "$host_ouisync_dir" ]; then
    container_ouisync_dir=$work_dir/ouisync
    copy_sources $host_ouisync_dir $container_ouisync_dir
fi

#### Configure

cmake_configure_args=(
    -w $build_dir
    cmake $ouinet_dir -G '"Unix Makefiles"' 
    -DCMAKE_BUILD_TYPE=Debug
    -DWITH_OUISYNC=$([ "$with_ouisync" == y ] && echo ON || echo OFF)
)

if [ -n "$container_ouisync_dir" ]; then
    cmake_configure_args+=(-DOUISYNC_SRC_DIR=$container_ouisync_dir)
fi

exe ${cmake_configure_args[@]}

### Build

# If the container is being reused and you get an error that an executable or
# binary can't be opened for writing (e.g. because a test did not exit cleanly)
# use these commands to find and kill the apps.
#    tasklist | grep test_
#    taskkill //im test_fetch.exe //f //t

targets=(
    ${run_cpp_tests[@]}
)

cmake_build_args=(
    # Cargo's built in git has some ssl problems fetching.
    -e CARGO_NET_GIT_FETCH_WITH_CLI=true
    -w $build_dir
    cmake --build .
    ${targets[@]/#/--target=}
    #-v
    -j $host_core_count
)
exe ${cmake_build_args[@]}

### Test

# Dlls required for tests
dlls=(
    /c/Windows/System32/downlevel/api-ms-win-core-synch-l1-2-0.dll
    /c/Windows/System32/drivers/netio.sys
    $build_dir/libouinet_asio.dll
    $build_dir/libouinet_asio_ssl.dll
    $build_dir/libclient_lib.dll
    $build_dir/libinjector_lib.dll
    $build_dir/gcrypt/out/bin/libgcrypt-20.dll
    $build_dir/gpg_error/out/bin/libgpg-error-0.dll
)

make_dll_links=(
    "for dll in ${dlls[*]}; do"
    '    target=$(basename $dll);'
    '    rm -f $target;'
    '    MSYS_NO_PATHCONV=1 cmd /c mklink $target $(cygpath -w $dll);'
    'done'
)

# Note: when you get
#   "error while loading shared libraries: ?: cannot open shared object file: No such file or directory"
# and `ldd` is useless, try using `cygtest.exe <PATH_TO_BINARY>`.

exe -w $test_dir "${make_dll_links[*]}"

if [ -z "$run_cpp_tests" ]; then
    run_cpp_tests=$(list_all_test_targets $build_dir)
fi

for test in ${run_cpp_tests[@]}; do
    if is_in $test ${excluded_test_targets[@]} ; then
        echo "::: Skipping excluded test $test"
        continue
    fi
    echo "::: Running test: $test"
    exe -w $test_dir "./$test.exe --log_level=unit_scope"
done
