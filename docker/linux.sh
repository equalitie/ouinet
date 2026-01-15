#!/bin/bash

set -e

host=
docker_host=
clean=
target_oss=()
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
            target_oss+=($(parse_target_os $2)); shift
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

if [ -z "$target_oss" ]; then
    error "Missing --target-os parameter"
fi

image_name=$USER.ouinet.build
container_name=$USER.ouinet.build

work_dir=/opt
src_dir=$work_dir/ouinet

echo "Host:           $docker_host"
echo "Target OS:      ${target_oss[*]}"
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

    rust_version=1.92.0

    rust_target=(
        aarch64-linux-android
        armv7-linux-androideabi
        x86_64-pc-windows-gnu
        x86_64-linux-android
    )

    apt_dependencies=(
        rsync build-essential cmake zlib1g-dev libssl-dev git curl nlohmann-json3-dev
        # For building Ouisync
        pkg-config libfuse3-dev
        # For building Windows binaries
        mingw-w64-x86-64-dev g++-mingw-w64-x86-64 libz-mingw-w64-dev gettext locales wine64
        # For building Android binaries
        wget unzip openjdk-21-jdk ninja-build
        # For integration tests
        python3 python3-pip python-is-python3
    )

    # These would be downloaded automatically during building of Android
    # binaries, but it's good to have them in the image
    android_sdk_packages=(
        "cmdline-tools;latest"
        "build-tools;35.0.0"
        "ndk;28.2.13676358"
        "platform-tools"
        "platforms;android-36"
    )

    # https://developer.android.com/studio#command-tools
    android_sdk_version=13114758
    android_home=$src_dir/sdk

    dockerfile=(
        "FROM debian:trixie-slim"

        "RUN apt update"
        "RUN apt install -y ${apt_dependencies[*]}"

        # Install Rust
        "RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain=$rust_version"
        'ENV PATH="${PATH}:/root/.cargo/bin"'
        "RUN rustup target add ${rust_target[*]}"

        # Setup Android dev environment
        "ENV ANDROID_HOME=$android_home"
        "RUN mkdir -p ${android_home}/cmdline-tools"
        "RUN wget -q https://dl.google.com/android/repository/commandlinetools-linux-${android_sdk_version}_latest.zip"
        "RUN unzip *tools*linux*.zip -d ${android_home}/cmdline-tools"
        "RUN mv ${android_home}/cmdline-tools/cmdline-tools ${android_home}/cmdline-tools/tools"
        "RUN rm commandlinetools-linux-*_latest.zip"
        'ENV PATH="${ANDROID_HOME}/cmdline-tools/tools/bin:${PATH}"'
        "RUN yes | sdkmanager --licenses"
        "RUN sdkmanager --install $(printf '"%s" ' "${android_sdk_packages[@]}")"
        'ENV PATH="${ANDROID_HOME}/platform-tools:${ANDROID_HOME}/emulator:${PATH}"'

        'WORKDIR $work_dir'
        "RUN echo 'PS1=\"\\h/$container_name:\\W \\u$ \"' >> ~/.bashrc"
    )

    echo -e "${dockerfile[@]/*/&'\n'}" | dock build -t $image_name -
)

# Shortcut for `docker $docker_host exec ... $container_name ...`
function exe (
    opt_w=
    opt_i=
    opt_t=
    opt_e=()
    while [[ "$#" -gt 0 ]]; do
        case $1 in
            -w) opt_w="-w=$2"; shift ;;
            -i) opt_i="-i" ;;
            -t) opt_t="-t" ;;
            -it) opt_i="-i"; opt_t="-t" ;;
            -e) opt_e+=("$2"); shift ;;
            *) break ;;
        esac
        shift
    done
    dock exec $opt_w $opt_i $opt_t ${opt_e[@]/#/'-e '} $container_name "$@"
)

function enter (
    exe -it bash
)

function is_container_running (
    [ -n "$(dock ps -a -q -f name=$container_name 2>/dev/null)" ]
)

function copy_local_sources (
    rsync_exclude_dirs=(
        '/build'
        '/rust/target'
        '/sdk'
        '/_gradle-home'
        '/build-android-*'
        '/gradle-*'
        '.git'
    )

    rsync -e "docker $docker_host exec -i" \
        -av --no-links --delete \
        ${rsync_exclude_dirs[@]/#/--exclude=} \
        $(pwd)/ $container_name:$src_dir
)

# Test whether the first arg is in the rest of the args
function is_in (
    item=$1; shift
    list="$@"
    for i in ${list[@]}; do
        if [ "$item" == "$i" ]; then return 0; fi
    done
    return 1
)

# ---

if [ "$clean" = y ]; then
    dock container rm -f $container_name 2>/dev/null || true
fi

build_image

if ! is_container_running; then
    dock run -d --rm --name $container_name $image_name sleep 1d
fi

if [ "$enter_on_exit" = y ]; then
    trap enter EXIT
fi

exe bash -c "mkdir -p $src_dir"

copy_local_sources


for target_os in ${target_oss[@]}; do
    ### Build

    build_dir=$work_dir/build.$target_os

    exe bash -c "mkdir -p $build_dir"

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
            -DCMAKE_TOOLCHAIN_FILE=$src_dir/cmake/toolchain-mingw64.cmake
        )
    fi
    
    if [ "$target_os" == linux -o "$target_os" == windows ]; then
        exe -w $build_dir cmake $src_dir "${cmake_configure_options[@]}"
        exe -w $build_dir cmake --build . -v -j $(exe nproc)
    else
        exe -w $src_dir ./scripts/build-android.sh
    fi
    
    ### Rust Tests
    
    # Only on Linux because `cargo` would look for libouinet_asio.so which is not
    # built for Windows (only dll).
    if [ "$target_os" == linux ]; then
        env=(
            CXXFLAGS="-I$build_dir/boost/install/include"
            LD_LIBRARY_PATH="$build_dir"
            LIBRARY_PATH="$build_dir"
            RUST_BACKTRACE=1
            RUST_LOG=ouinet_rs=debug
        )
        exe ${env[@]/#/-e } cargo test --verbose --manifest-path $src_dir/rust/Cargo.toml -- --nocapture
    fi
    
    ### C++ Tests
    
    if [ "$run_tests" = y -a "$target_os" != android ]; then
        test_targets=$(exe cmake --build $build_dir --target help | grep '^\.\.\. test' | sed 's/^\.\.\. \(.*\)/\1/g')
    
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
            env+=(WINEPATH="$(IFS=';'; echo "${winepaths[*]}")")
        fi
    
        for test in ${test_targets[@]}; do
            if is_in $test ${excluded_test_targets[@]} ; then
                echo "::: Skipping excluded test $test"
                continue
            fi
            echo "::: Running test: $test"
            exe ${env[@]/#/-e } $launcher $build_dir/test/$test$binary_suffix --log_level=unit_scope
        done
    fi
done
