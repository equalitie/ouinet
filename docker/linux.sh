#!/bin/bash

set -e

host=
clean=
target_oss=()
run_all_tests=
run_cpp_tests=()
run_cpp_rust_tests=()
run_python_tests=
enter_on_exit=
excluded_test_targets=()
artifact_dir=
artifacts_extra=()
with_ouisync=n
host_ouisync_dir=
with_asan=n
container_name=
image_name=
container_duration=1d
cmake_build_type=Debug
android_abi=arm64-v8a
android_publish=n
windows_sign_artifacts=n
sign_directory=/opt/sign
env=()

source $(dirname $0)/util.sh linux

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
            ;;
        --target-os|-t)
            target_oss+=($(parse_target_os $2)); shift
            ;;
        --run-cpp-test)
            run_cpp_tests+=($2); shift
            ;;
        --run-cpp-rust-tests)
            run_cpp_rust_tests=y
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
        --with-ouisync)
            with_ouisync=y
            ;;
        --use-ouisync-dir)
            host_ouisync_dir=$2; shift;
            ;;
        --with-asan)
            with_asan=y
            ;;
        --artifact-dir)
            artifact_dir=$2; shift;
            mkdir -p $artifact_dir
            ;;
        --artifact-extra)
            artifacts_extra+=($2); shift;
            ;;
        --container-name)
            container_name=($2); shift
            ;;
        --image-name)
            image_name=($2); shift
            ;;
        --container-duration)
            container_duration=($2); shift
            ;;
        --cmake-build-type)
            cmake_build_type=($2); shift
            ;;
        --android-abi)
            android_abi=($2); shift
            ;;
        --android-publish)
            android_publish=y
            cmake_build_type=Release
            ;;
        --windows-sign-artifacts)
          windows_sign_artifacts=y
            ;;
        --env-var|-e)
            env+=("$2"); shift
            ;;
        --clean) clean=y ;;
        *) error "Unknown option $1" ;;
    esac
    shift
done

if [ -z "$target_oss" ]; then
    error "Missing --target-os parameter"
fi

if [[ "$windows_sign_artifacts" == y ]]; then
  if ! [[ " ${target_oss[*]} " == *" windows "* ]]; then
      error "Option \`--windows-sign-artifacts\` can be used" \
            "only with \`--target-os windows\`."
  fi
  if ! [[ " ${env[*]} " == *" SIGN_CERT_BASE64="* ]] || \
     ! [[ " ${env[*]} " == *" SIGN_CERT_PASSWORD="* ]]; then
      error 'Pass `-e SIGN_CERT_BASE64="$(base64 cert.pfx)"`' \
            'and `-e SIGN_CERT_PASSWORD=abcd1234`' \
            'when using `--windows-sign-artifacts`.'
  fi
fi

# Most likely returns 'amd64' or 'arm64'
docker_default_platform=$(dock version --format '{{.Server.Arch}}')

# Being explicit about docker platform is useful when building on Arm based
# Mackintosh PCs as cross compilation for Android or Windows doesn't work on
# Arm.
function docker_choose_platform {(
    # Windows and Android don't build on arm
    if is_in android ${target_oss[@]} || is_in windows ${target_oss[@]} ; then
        echo 'amd64'
    else
        echo $docker_default_platform
    fi
)}

docker_platform=$(docker_choose_platform)
name_suffix=$([ "$docker_platform" = "$docker_default_platform" ] && echo "" || echo ".$docker_platform")
image_name=$(choose_docker_image_name $image_name)$name_suffix
container_name=$(choose_docker_container_name $container_name)$name_suffix

work_dir=/opt
ouinet_dir=$work_dir/ouinet

echo "Host:           $host"
echo "Target OS:      ${target_oss[*]}"
echo "Image name:     $image_name"
echo "Container name: $container_name"
echo "Clean:          $([ "$clean" = y ] && echo yes || echo no)"
echo ""

function build_image (
    rust_version=1.96.0

    rust_target=(
        aarch64-linux-android
        armv7-linux-androideabi
        x86_64-pc-windows-gnu
        x86_64-linux-android
    )

    apt_dependencies=(
        rsync build-essential cmake zlib1g-dev libssl-dev git curl nlohmann-json3-dev gdb
        # For building Ouisync
        pkg-config
        # For building and testing Windows binaries
        mingw-w64-x86-64-dev g++-mingw-w64-x86-64 libz-mingw-w64-dev gettext locales wine64
        # For signing Windows artifacts
        osslsigncode
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
    android_home=$ouinet_dir/sdk

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

    echo -e "${dockerfile[@]/*/&'\n'}" | dock build --platform linux/$docker_platform -t $image_name -
)

function enter (
    exe -it bash
)

function is_container_running (
    [ -n "$(dock ps -a -q -f name=^$container_name$ 2>/dev/null)" ]
)

function list_artifacts_for_target_os (
    target_os=$1
    case "$target_os" in
        linux|android)
            exe_suffix=""
            lib_suffix=".so"
            ;;
        windows)
            exe_suffix=".exe"
            lib_suffix=".dll"
            ;;
        *) error "Invalid target_os ($target_os) in 'list_artifacts_for_target_os'"
    esac
    case "$target_os" in
        linux|windows)
            artifacts=(
                $build_dir/client$exe_suffix
                $build_dir/injector$exe_suffix
                $build_dir/libouinet_asio$lib_suffix
                $build_dir/libouinet_asio_ssl$lib_suffix
                $build_dir/libasio_utp$lib_suffix
            )

            if [[ "$target_os" == linux ]]; then
                artifacts+=(
                    $build_dir/libclient$lib_suffix
                    $build_dir/libinjector$lib_suffix
                    $build_dir/libgcrypt.so.20.5.0
                    $build_dir/libgcrypt.so.20
                    $build_dir/libgcrypt.so
                    $build_dir/libgpg-error.so.0.38.0
                    $build_dir/libgpg-error.so.0
                    $build_dir/libgpg-error.so
                )
            fi

            if [[ "$target_os" == windows ]]; then
                gcrypt_bin_dir=$build_dir/gcrypt/out/bin
                gpg_error_bin_dir=$build_dir/gpg_error/out/bin
                mingw_gcc_dir=/usr/lib/gcc/x86_64-w64-mingw32/14-win32
                mingw_lib_dir=/usr/x86_64-w64-mingw32/lib

                # client header
                artifacts+=(
                  $ouinet_dir/src/client_lib.h
                )
                # dll files
                artifacts+=(
                    $build_dir/libclient_lib$lib_suffix
                    $build_dir/libinjector_lib$lib_suffix
                    $gcrypt_bin_dir/libgcrypt-20$lib_suffix
                    $gpg_error_bin_dir/libgpg-error-0$lib_suffix
                )
                # dll.a files
                gcrypt_lib_dir=$build_dir/gcrypt/out/lib
                gpg_error_lib_dir=$build_dir/gpg_error/out/lib
                artifacts+=(
                  $build_dir/libclient.dll.a
                  $build_dir/libclient_lib.dll.a
                  $build_dir/libinjector.dll.a
                  $build_dir/libinjector_lib.dll.a
                  $build_dir/libouinet_asio.dll.a
                  $build_dir/libouinet_asio_ssl.dll.a
                  $gcrypt_lib_dir/libgcrypt.dll.a
                  $gpg_error_lib_dir/libgpg-error.dll.a
                )
                # .a files
                artifacts+=(
                  $build_dir/libcpp_upnp.a
                  $build_dir/libouinet_common.a
                  $build_dir/libouinet_common_public.a
                  $build_dir/libouinet_rs.a
                )
                # third party dlls required
                artifacts+=(
                  $mingw_gcc_dir/libgcc_s_seh-1.dll
                  $mingw_gcc_dir/libstdc++-6.dll
                  $mingw_lib_dir/libwinpthread-1.dll
                  $mingw_lib_dir/zlib1.dll
                )
            fi

            if [[ "$with_ouisync" == y ]]; then
                artifacts+=(
                  $build_dir/libcpp_ouisync_client$lib_suffix
                  $build_dir/libcpp_ouisync_service$lib_suffix
                )
            fi

            if [ ${#artifacts_extra[@]} -gt 0 ]; then
              for art in "${artifacts_extra[@]}"; do
                  artifacts+=(
                    $build_dir/$art
                  )
              done
            fi
            ;;
        android)
            artifacts=(
                $ouinet_dir/build-android-$android_abi-${cmake_build_type,,}/ouinet/outputs/aar/ouinet-${cmake_build_type,,}.aar
            )
            ;;
        *) error "Invalid target_os ($target_os) in 'list_artifacts_for_target_os'"
    esac
    echo ${artifacts[@]}
)

function check_artifacts_exist_for_target_os (
    target_os=$1

    script=(
        "missing=();"
        "for artifact in $(list_artifacts_for_target_os $target_os); do"
        "    if [ ! -f \$artifact ]; then"
        "       missing+=(\$artifact);"
        "    fi"
        "done;"
        "echo \${missing[*]}"
    )

    missing=$(exe bash -c "${script[*]}")

    if [ -n "${missing[*]}" ]; then
        error "Missing artifacts for $target_os: ${missing[@]}"
    fi
)

# ---

build_image

if ! is_container_running; then
    dock run --platform linux/$docker_platform \
             -d --rm --tmpfs $sign_directory \
             --name $container_name $image_name \
             sleep $container_duration
fi

if [ "$enter_on_exit" = y ]; then
    trap enter EXIT
fi

# ---

exe bash -c "mkdir -p $ouinet_dir"

function copy_local_sources (
    host_src_dir=${1%/}
    container_dst_dir=$2

    exclude=(
        '/build'
        '/rust/target'
        '/sdk'
        '/_gradle-home'
        '/build-android-*'
        '/gradle-*'
        '/target'
        '/bindings/cpp/build'
        '/bindings/cpp/examples/build'
        '/bindings/kotlin/build'
        '/cmake-build-*'
    )

    if ! is_in android ${target_oss[@]}; then
        # Only Android building requires the .git/ directory
        exclude+=(.git)
    fi

    docker_rsync ${exclude[@]/#/-e } $host_src_dir $container_dst_dir
)

copy_local_sources $(pwd) $ouinet_dir

# Prevent "dubious ownership" errors
exe git config --global --add safe.directory $ouinet_dir

if [ -n "$host_ouisync_dir" ]; then
    container_ouisync_dir=$work_dir/ouisync
    copy_local_sources $host_ouisync_dir $container_ouisync_dir
fi

# ---

for target_os in ${target_oss[@]}; do
    ### Build

    if [ "$target_os" == linux -o "$target_os" == windows ]; then
        build_dir=$work_dir/build.$target_os

        if [ "$clean" = y ]; then
            exe rm -rf $build_dir
        fi

        exe bash -c "mkdir -p $build_dir"

        cmake_configure_options=(
            -DCMAKE_BUILD_TYPE=$cmake_build_type
            -DWITH_ASAN=$([ "$with_asan" == y ] && echo ON || echo OFF)
            -DCORROSION_BUILD_TESTS=ON
            -DWITH_OUISYNC=$([ "$with_ouisync" == y ] && echo ON || echo OFF)
            -DOUINET_MEASURE_BUILD_TIMES=OFF
        )

        if [ "$target_os" == windows ]; then
            cmake_configure_options+=(
                -DCMAKE_TOOLCHAIN_FILE=$ouinet_dir/cmake/toolchain-mingw64.cmake
            )
        fi

        if [ -n "$container_ouisync_dir" ]; then
            cmake_configure_options+=(-DOUISYNC_SRC_DIR=$container_ouisync_dir)
        fi

        exe -w $build_dir cmake $ouinet_dir "${cmake_configure_options[@]}"
        exe -w $build_dir cmake --build . -j $(exe nproc) ${run_cpp_tests[@]/#/--target }
    else
        if [ "$clean" = y ]; then
            exe -w $ouinet_dir git clean -dfX
        fi

        env+=(
          ABI="$android_abi"
        )

        opt_env=()
        for v in "${env[@]}"; do
            opt_env+=(-e "$v")
        done

        exe "${opt_env[@]}" -w $ouinet_dir ./scripts/build-android.sh \
            $([ "$cmake_build_type" = "Release" ] && echo " -r") \
            $([ "$android_publish" = "y" ] && echo " bootstrap build publish")
    fi

    if [ -n "$artifact_dir" ]; then
        check_artifacts_exist_for_target_os $target_os
    fi

    ### Rust Tests

    if [ "$run_all_tests" == y -o "$run_cpp_rust_tests" == y ]; then
        # Only on Linux because `cargo` would look for libouinet_asio.so which is not
        # built for Windows (only dll).
        if [ "$target_os" == linux ]; then
            env+=(
                CXXFLAGS="-I$build_dir/boost/src/built_boost"
                LD_LIBRARY_PATH="$build_dir"
                LIBRARY_PATH="$build_dir"
                RUST_BACKTRACE=1
                RUST_LOG=ouinet_rs=debug
            )
            exe ${env[@]/#/-e } cargo test --manifest-path $ouinet_dir/rust/Cargo.toml -- --nocapture
        fi
    fi

    ### C++ Tests

    if [ "$run_all_tests" == y -o "$run_cpp_rust_tests" == y -o -n "${run_cpp_tests[*]}" ]; then
        if [ "$target_os" != android ]; then
            args=(
                --build-dir $build_dir
                ${run_cpp_tests[@]/#/--run-test }
                ${excluded_test_targets[@]/#/--exclude-test }
            )

            exe -w $ouinet_dir bash -c "./scripts/run_cpp_tests.sh ${args[*]}"
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
                "export OUINET_REPO_DIR=$ouinet_dir;"

                "$ouinet_dir/scripts/run_python_tests.sh;"
            )

            exe bash -c "${script[*]}"
        fi
    fi

    # Sign Windows artifacts
    if [ "$target_os" == windows ]; then
        if [ "$windows_sign_artifacts" = y ]; then
          opt_env=()
          for v in "${env[@]}"; do
              case $v in
                SIGN_CERT_*) opt_env+=(-e "$v");;
                *);;
              esac
          done
          exe "${opt_env[@]}" bash -c 'echo -n "$SIGN_CERT_BASE64" | base64 -d > /opt/sign/cert.pfx'
          artifacts=($(list_artifacts_for_target_os $target_os))
          for artifact in "${artifacts[@]}"; do
              if [[ $artifact == *.exe ]]; then
                  input_artifact="$sign_directory/in/$(basename $artifact)"
                  output_artifact="$sign_directory/out/$(basename $artifact)"
                  script=(
                      "rm -rf $sign_directory/{in,out};"
                      "mkdir -p $sign_directory/{in,out};"
                      "mv $artifact $input_artifact;"
                      "osslsigncode sign "
                          "-pkcs12 $sign_directory/cert.pfx"
                          '-pass "$SIGN_CERT_PASSWORD"'
                          '-n "Ouinet" -i "https://equalitie.org"'
                          "-in $input_artifact"
                          "-out $output_artifact;"
                      "mv $output_artifact $artifact;"
                      "rm -rf $sign_directory/{in,out}/*;"
                  )
                  echo "Signing $artifact"
                  exe "${opt_env[@]}" bash -c "${script[*]}"
              fi
          done
          exe bash -c "rm ${sign_directory}/cert.pfx"
        fi
    fi

    ### Download artifacts

    if [ -n "$artifact_dir" ]; then
        artifacts=($(list_artifacts_for_target_os $target_os))

        dst_dir=$artifact_dir/$target_os
        mkdir -p $dst_dir

        for artifact in "${artifacts[@]}"; do
            dock container cp $container_name:$artifact $dst_dir/$(basename $artifact)
        done
    fi
done
