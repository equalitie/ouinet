
util_container_os=$1

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

case "$util_container_os" in
    linux) ;;
    windows) ;;
    *) error "Invalid util_container_os: \"$util_container_os\""
        ;;
esac

function choose_docker_container_name (
    echo $USER.ouinet.build
)

function choose_docker_image_name (
    echo $USER.ouinet.build
)

function dock (
    if [ -n "$host" ]; then
        opt_h="--host ssh://$host"
    fi
    docker $opt_h "$@"
)

# Shorthand for `docker -H ssh://$host exec ... $container_name ...`
function exe (
    opt_w=
    opt_i=
    opt_t=
    opt_e=()
    while [[ "$#" -gt 0 ]]; do
        case $1 in
            -w) case "$util_container_os" in
                    linux)
                        opt_w="-w=$2"
                        ;;
                    windows)
                        if [[ "$2" == /* ]]; then
                            opt_w="-w c:/msys64$2"
                        else
                            opt_w="-w $2"
                        fi
                        ;;
                esac
                shift
                ;;
            -i) opt_i="-i" ;;
            -t) opt_t="-t" ;;
            -it) opt_i="-i"; opt_t="-t" ;;
            -e) opt_e+=("$2"); shift ;;
            *) break ;;
        esac
        shift
    done

    opts=($opt_w $opt_i $opt_t ${opt_e[@]/#/'-e '} $container_name)

    case "$util_container_os" in
        linux)
            dock exec ${opts[@]} "$@"
            ;;
        windows)
            dock exec ${opts[@]} bash -c "$*"
            ;;
    esac

)

function docker_rsync (
    exclude=()

    while [[ "$#" -gt 0 ]]; do
        case $1 in
            -e) exclude+=($2); shift ;;
            *) break;;
        esac
        shift
    done

    host_src_dir=$1
    container_dst_dir=$2

    if [ -n "$host" ]; then
        opt_h="--host ssh://$host"
    fi

    rsync -e "docker $opt_h exec -i" \
        -av --no-links --delete \
        ${exclude[@]/#/--exclude=} \
        $host_src_dir/ $container_name:$container_dst_dir
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

function list_all_test_targets (
    cmake_build_dir=$1
    # The output of the cmake command below is in the following format:
    #
    #     The following are some of the valid targets for this Makefile:
    #     ... all (the default if no target is provided)
    #     ... clean
    #     ... depend
    #     ... edit_cache
    #     ... install
    #     ... test_cache
    #
    # e.t.c.
    exe cmake --build $cmake_build_dir --target help | grep '^\.\.\. test' | sed 's/^\.\.\. \(.*\)/\1/g'
)
