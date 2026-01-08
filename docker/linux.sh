#!/bin/bash

set -e

host=
docker_host=
clean=
target_os=
run_tests=
enter_on_exit=
excluded_test_targets=()

function print_help (
    echo "Utility to build Ouinet in a Docker container"
    echo
)

function error (
    msg="$*"
    print_help
    echo "Error: $msg"
    exit 1
)

function parse_target_os (
    case $1 in
        win|windows) echo windows ;;
        lin|linux) echo linux ;;
        android) echo android ;;
        *) error "Invalid target OS \"$1\", must be one of {windows,linux,android}"
    esac
)

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --host|-H)
            host="$2"; shift
            docker_host="--host=ssh://$host"
            ;;
        --target-os|-t)
            target_os=$(parse_target_os $2); shift
            ;;
        --run-tests)
            run_tests=y
            ;;
        --exclude-test)
            excluded_test_targets+=($2); shift
            ;;
        --enter-on-exit)
            enter_on_exit=y
            ;;
        --clean) clean=y ;;
        *) error "Unknown option $1" ;;
    esac
    shift
done

if [ -z "$target_os" ]; then
    error "Missing --target-os parameter"
fi

image_name=$USER.ouinet.build
container_name=$USER.ouinet.build

home_dir=/opt
work_dir=$home_dir/ouinet
build_dir=$home_dir/build.$target_os

echo "Host:           $docker_host"
echo "Target OS:      $target_os"
echo "Image name:     $image_name"
echo "Container name: $container_name"
echo ""

function dock (
    docker $docker_host "$@"
)

function map_cpu_arch_for_rust (
    arch=$1
    case $arch in
        x86_64) echo $arch; ;;
        arm64) echo aarch64; ;;
        *) error "Unrecognized host CPU architecture: $arch"
    esac
)

function host_cpu_arch (
    if [ -n "$host" ]; then
        ssh $host uname -m
    else
        uname -m
    fi
)

function build_image (
    rust_arch=$(map_cpu_arch_for_rust $(host_cpu_arch))

    rust_toolchains=(
        1.92.0-$rust_arch-unknown-linux-gnu
        aarch64-linux-android
        x86_64-pc-windows-gnu
    )

    dockerfile=(
        "FROM rust:slim-trixie"
        "RUN apt update"
        "RUN apt install -y rsync build-essential cmake zlib1g-dev libssl-dev git gdb"
        "RUN apt install -y pkg-config libfuse3-dev"
    
        # MingW requirements
        "RUN apt install -y mingw-w64-x86-64-dev g++-mingw-w64-x86-64 libz-mingw-w64-dev gettext locales wine64"
        "RUN rustup target add --toolchain ${rust_toolchains[*]}"
    
        # Needed when building for Android
        "RUN apt install -y wget unzip openjdk-25-jdk ninja-build"
    
        "RUN git config --global --add safe.directory '*'"
    
        "ENV HOME=$home_dir"
        'WORKDIR $HOME'
        "RUN echo 'PS1=\"\\h/$container_name:\\W \\u$ \"' >> ~/.bashrc"
    )

    echo -e "${dockerfile[@]/*/&'\n'}" | dock build -t $image_name -
)

# Shortcut for `docker $docker_host exec ... $container_name ...`
function exe (
    opt_workdir=
    opt_i=
    opt_t=
    opt_e=()
    while [[ "$#" -gt 0 ]]; do
        case $1 in
            -w) opt_workdir="-w=$2"; shift ;;
            -i) opt_i="-i" ;;
            -t) opt_t="-t" ;;
            -it) opt_i="-i"; opt_t="-t" ;;
            -e) opt_e+=("$2"); shift ;;
            *) break ;;
        esac
        shift
    done
    #echo "$*" | dock exec -i $container_name dd of="$home_dir/.bash_history" conv=notrunc oflag=append
    dock exec $opt_workdir $opt_i $opt_t ${opt_e[@]/#/'-e '} $container_name "$@"
)

function enter (
    exe -it bash
)

function is_container_running (
    [ -n "$(dock ps -a -q -f name=$container_name 2>/dev/null)" ]
)

function copy_local_sources (
    rsync_exclude_dirs=('/build' '.git')

    rsync -e "docker $docker_host exec -i" \
        -av --no-links --delete \
        ${rsync_exclude_dirs[@]/#/--exclude=} \
        $(pwd)/ $container_name:$work_dir
)

if [ "$clean" = y ]; then
    dock container rm -f $container_name 2>/dev/null || true
    #dock image rm $image_name || true
fi

build_image

if ! is_container_running; then
    dock run -d --rm --name $container_name $image_name sleep 1d
fi

if [ "$enter_on_exit" = y ]; then
    trap enter EXIT
fi

exe bash -c "mkdir -p $work_dir $build_dir"

copy_local_sources

### Build

cmake_configure_options=(
    -DCMAKE_BUILD_TYPE=Debug
    -DWITH_ASAN=OFF
    -DWITH_EXPERIMENTAL=OFF
    -DCORROSION_BUILD_TESTS=ON
    -DWITH_OUISYNC=OFF
    # For testing with custom Ouisync sources
    #-DOUISYNC_SRC_DIR=$HOME/work/ouisync
    -DOUINET_MEASURE_BUILD_TIMES=OFF
)

if [ "$target_os" == windows ]; then
    cmake_configure_options+=(
        -DCMAKE_TOOLCHAIN_FILE=$work_dir/cmake/toolchain-mingw64.cmake
    )
fi

exe -w $build_dir cmake $work_dir "${cmake_configure_options[@]}"
exe -w $build_dir cmake --build . -v -j $(exe nproc)

### Test
### C++ Tests

if [ "$run_tests" = y ]; then
    test_targets=$(exe cmake --build $build_dir --target help | grep '^\.\.\. test' | sed 's/^\.\.\. \(.*\)/\1/g')

    # Test whether the first arg is in the rest of the args
    function is_in (
        item=$1; shift
        list="$@"
        for i in ${list[@]}; do
            if [ "$item" == "$i" ]; then return 0; fi
        done
        return 1
    )

    binary_suffix=
    env=()
    lanucher=

    if [ "$target_os" == windows ]; then
        launcher="wine"
        binary_suffix=.exe
        winepaths=(
            $build_dir
            $build_dir/gcrypt/out/bin
            $build_dir/gpg_error/out/bin
            /usr/lib/gcc/x86_64-w64-mingw32/14-win32
        )
        env+=(-e WINEPATH="$(IFS=';'; echo "${winepaths[*]}")")
    fi

    for test in ${test_targets[@]}; do
        if is_in $test ${excluded_test_targets[@]} ; then
            echo "::: Skipping excluded test $test"
            continue
        fi
        echo "::: Running test: $test"
        exe ${env[@]} $launcher $build_dir/test/$test$binary_suffix --log_level=unit_scope
    done
fi
