FROM debian:stretch AS builder
# To get the list of build dependency packages from the Vagrantfile, run:
#
#     sed '/# Install toolchain/,/^$/!d' Vagrantfile \
#         | sed -En 's/^\s+(\S+)\s*\\?$/\1/p' | sort
#
RUN apt-get update && apt-get install -y \
    autoconf \
    automake \
    autopoint \
    build-essential \
    cmake \
    gettext \
    git \
    libboost-coroutine-dev \
    libboost-date-time-dev \
    libboost-dev \
    libboost-filesystem-dev \
    libboost-program-options-dev \
    libboost-regex-dev \
    libboost-system-dev \
    libboost-test-dev \
    libboost-thread-dev \
    libgcrypt-dev \
    libidn11-dev \
    libssl-dev \
    libtool \
    libunistring-dev \
    pkg-config \
    rsync \
    texinfo \
    wget \
    zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /usr/local/src
RUN git clone --recursive https://github.com/equalitie/ouinet.git
WORKDIR /opt/ouinet
RUN cmake /usr/local/src/ouinet \
 && make
RUN strip injector client test/test-* \
 && find . -name '*.so' | xargs strip \
 && find . -wholename '*/libexec/*' -executable -type f | xargs strip

FROM debian:stretch
# To get the list of library packages, enter the build directory and execute:
#
#     ldd injector client $(find . -name '*.so' | grep -v '\.libs') \
#         | sed -En 's#^.* => (/lib/.*|/usr/lib/.*) \(.*#\1#p' | sort -u \
#         | (while read l; do dpkg -S $l; done) | cut -f1 -d: | sort -u
#
RUN apt-get update && apt-get install -y \
    libboost-atomic1.62.0 \
    libboost-chrono1.62.0 \
    libboost-context1.62.0 \
    libboost-coroutine1.62.0 \
    libboost-date-time1.62.0 \
    libboost-filesystem1.62.0 \
    libboost-program-options1.62.0 \
    libboost-regex1.62.0 \
    libboost-system1.62.0 \
    libboost-test1.62.0 \
    libboost-thread1.62.0 \
    libboost-timer1.62.0 \
    libc6 \
    libgcc1 \
    libgcrypt20 \
    libgpg-error0 \
    libicu57 \
    libltdl7 \
    libssl1.1 \
    libstdc++6 \
    libunistring0 \
    zlib1g \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/ouinet
COPY --from=builder /opt/ouinet/modules/ipfs-cache/ipfs_bindings/ipfs_bindings.so modules/ipfs-cache/ipfs_bindings/
COPY --from=builder /opt/ouinet/modules/ipfs-cache/libipfs-cache.so modules/ipfs-cache/
COPY --from=builder /opt/ouinet/modules/gnunet-channels/gnunet-bin/share/gnunet/ modules/gnunet-channels/gnunet-bin/share/gnunet/
COPY --from=builder /opt/ouinet/modules/gnunet-channels/gnunet-bin/lib/ modules/gnunet-channels/gnunet-bin/lib/
COPY --from=builder /opt/ouinet/injector /opt/ouinet/client /usr/local/src/ouinet/scripts/ouinet-docker.sh ./
COPY --from=builder /opt/ouinet/test/test-* test/
COPY --from=builder /usr/local/src/ouinet/repos/ repos/
CMD ["./ouinet-docker.sh", "injector"]
