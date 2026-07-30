#!/bin/bash

set -e

image_name=$USER.ouinet.i2p.proxy
container_name=$image_name

function build_image (
    # https://i2p.net/en/docs/guides/installing-i2p-on-debian-and-ubuntu/

    # Forces rebuild
    #no_cache_arg="--no-cache"

    apt_dependencies=(
        apt-transport-https
        lsb-release
        curl
        gpg
        sudo
    )

    fingerprint="7840 E761 0F28 B904 7535 49D7 67EC E560 5BCF 1346"

    dockerfile=(
        "FROM debian:trixie-slim\n"

        "RUN apt update\n"
        "RUN apt install -y ${apt_dependencies[*]}\n"
        'RUN echo "deb [signed-by=/usr/share/keyrings/i2p-archive-keyring.gpg] https://deb.i2p.net/ $(lsb_release -sc) main" | tee /etc/apt/sources.list.d/i2p.list\n'
        "RUN curl -o i2p-archive-keyring.gpg https://i2p.net/i2p-archive-keyring.gpg\n"
        # Verify fingerprint
        "RUN FINGERPRINT=\"\$(gpg --keyid-format long --import --import-options show-only --with-fingerprint i2p-archive-keyring.gpg 2>&1 \\
                | grep 'Key fingerprint' \\
                | cut -d'=' -f2 \\
                | awk '{\$1=\$1;print}')\"; \\
            test \"\$FINGERPRINT\" = \"${fingerprint[*]}\"\n"
        "RUN cp i2p-archive-keyring.gpg /usr/share/keyrings\n"
        "RUN apt-get update\n"
        "RUN apt-get install -y i2p i2p-keyring\n"
        # i2p refuses to run under root
        "RUN useradd -m i2p\n"
        # Enable SAM client bridge
        "RUN sed -i 's/clientApp.1.startOnLoad=false/clientApp.1.startOnLoad=true/' /usr/share/i2p/clients.config\n"
    )

    echo -e "${dockerfile[@]}" | docker build $no_cache_arg -t $image_name -
)

build_image

# After this you should be able to access i2p consose in your browser at
# http://localhost:7657
docker run --network host -it --rm --name $container_name $image_name sudo -u i2p i2prouter console
