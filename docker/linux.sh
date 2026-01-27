#!/bin/bash

set -e

host=
docker_host=
clean=
target_oss=()
run_all_tests=
run_cpp_tests=()
run_python_tests=
enter_on_exit=
excluded_test_targets=()
artifact_dir=

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
        --run-cpp-test)
            run_cpp_tests+=($2); shift
            ;;
        --run-python-tests)
            run_python_tests=y
            ;;
        --run-all-tests)
            run_all_tests=y
            ;;
        --exclude-test)
            excluded_test_targets+=($2); shift
            ;;
        --enter-on-exit)
            enter_on_exit=y
            ;;
        --artifact-dir)
            artifact_dir=$2; shift;
            mkdir -p $artifact_dir
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

function build_image (
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
        python3 python3-pip python3.13-venv python-is-python3
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

        "WORKDIR $work_dir"
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

# Test whether the first arg is in the rest of the args
function is_in (
    item=$1; shift
    list="$@"
    for i in ${list[@]}; do
        if [ "$item" == "$i" ]; then return 0; fi
    done
    return 1
)

function copy_local_sources (
    rsync_exclude_dirs=(
        '/build'
        '/rust/target'
        '/sdk'
        '/_gradle-home'
        '/build-android-*'
        '/gradle-*'
    )

    if ! is_in android ${target_oss[@]}; then
        # Only Android building requires the .git/ directory
        rsync_exclude_dirs+=(.git)
    fi

    rsync -e "docker $docker_host exec -i" \
        -av --no-links --delete \
        ${rsync_exclude_dirs[@]/#/--exclude=} \
        $(pwd)/ $container_name:$src_dir
)

function list_artifacts_for_target_os (
    target_os=$1
    case "$target_os" in
        linux)
            artifacts=(
                $build_dir/client
                $build_dir/injector
                $build_dir/libgcrypt.so.20.5.0
                $build_dir/libgpg-error.so.0.38.0
                $build_dir/libouinet_asio.so
                $build_dir/libouinet_asio_ssl.so
                $build_dir/libouinet_asio_ssl.so
                $build_dir/libclient.so
                $build_dir/libinjector.so
            )
            ;;
        windows)
            artifacts=(
                $build_dir/client.exe
                $build_dir/injector.exe
                $build_dir/gcrypt/out/bin/libgcrypt-20.dll
                $build_dir/gpg_error/out/bin/libgpg-error-0.dll
                $build_dir/libouinet_asio.dll
                $build_dir/libouinet_asio_ssl.dll
                $build_dir/libclient_lib.dll
                $build_dir/libinjector_lib.dll
            )
            ;;
        android)
            artifacts=(
                $src_dir/build-android-omni-debug/ouinet/outputs/aar/ouinet-debug.aar
            )
            ;;
        *) error "Invalid target_os ($target_os) in 'list_artifacts_for_target_os'"
    esac
    echo ${artifacts[@]}
)

function check_artifacts_exist_for_target_os (
    target_os=$1
    missing=()
    for artifact in $(list_artifacts_for_target_os $target_os); do
        if [ ! -f "$artifact" ]; then
            missing+=($artifact)
        fi
    done
    if [ -f "${missing[*]}" ]; then
        error "Missing artifacts for $target_os: ${missing[@]}"
    fi

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

# ---

for target_os in ${target_oss[@]}; do
    ### Build

    if [ "$target_os" == linux -o "$target_os" == windows ]; then
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
    
        exe -w $build_dir cmake $src_dir "${cmake_configure_options[@]}"
        exe -w $build_dir cmake --build . -v -j $(exe nproc)
    else
        exe -w $src_dir ./scripts/build-android.sh
    fi

    check_artifacts_exist_for_target_os $target_os
    
    ### Rust Tests

    if [ "$run_all_tests" == y ]; then
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
    fi
        
    ### C++ Tests
    if [ "$run_all_tests" == y -o -n "${run_cpp_tests[*]}" ]; then
        if [ "$target_os" != android ]; then
            if [ "$run_all_tests" == y ]; then
                test_targets=$(exe cmake --build $build_dir --target help | grep '^\.\.\. test' | sed 's/^\.\.\. \(.*\)/\1/g')
            else
                test_targets=${run_cpp_tests[@]}
            fi
        
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
    fi

    ### Python Tests
    
    # TODO: Run these when `$target_os = windows` as well (through Wine)
    if [ "$target_os" = linux ]; then
        if [ "$run_python_tests" = y -o "$run_all_tests" = y ]; then
            script=(
                "if [ ! -d $build_dir/venv ]; then"
                "    python3 -m venv $build_dir/venv;"
                "fi;"
                "source $build_dir/venv/bin/activate;"
                "pip install twisted pytest requests pytest_asyncio;"
            
                "export OUINET_BUILD_DIR=$build_dir;"
                "export OUINET_REPO_DIR=$src_dir;"
            
                "$src_dir/scripts/run_integration_tests.sh;"
            )
            
            exe bash -c "${script[*]}"
        fi
    fi

    ### Download artifacts

    if [ -n "$artifact_dir" ]; then
        artifacts=$(list_artifacts_for_target_os $target_os)

        dst_dir=$artifact_dir/$target_os
        mkdir -p $dst_dir

        for artifact in "${artifacts[@]}"; do
            dock container cp $container_name:$artifact $dst_dir/$(basename $artifact)
        done
    fi
done
