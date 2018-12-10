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
RUN wget -q "https://downloads.sourceforge.net/project/boost/boost/1.67.0/boost_1_67_0.tar.bz2" \
 && tar -xf boost_1_67_0.tar.bz2 \
 && cd boost_1_67_0 \
 && ./bootstrap.sh \
 && ./b2 -j `nproc` -d+0 --link=shared \
         --with-system \
         --with-program_options \
         --with-test \
         --with-coroutine \
         --with-filesystem \
         --with-date_time \
         --with-regex \
         --with-iostreams \
         --prefix=/usr/local install
# This version is a recommendation and this file has been tested to work for it,
# but you may attempt to build other versions by overriding this argument.
ARG OUINET_VERSION=v0.0.22
RUN git clone --recursive -b "$OUINET_VERSION" https://github.com/equalitie/ouinet.git
WORKDIR /opt/ouinet
RUN cmake /usr/local/src/ouinet \
 && make
ARG OUINET_DEBUG=no
RUN \
if [ $OUINET_DEBUG != yes ]; then \
    strip injector client test/test-* \
        && find . -name '*.so' -exec strip '{}' + \
        && find . -wholename '*/libexec/*' -executable -type f -exec strip '{}' + ; \
fi

FROM debian:stretch
# To get the list of system library packages to install,
# enter the build directory and execute:
#
#     ldd injector client test/test-* $(find . -name '*.so' | grep -v '\.libs') \
#         | sed -En 's#^.* => (/lib/.*|/usr/lib/.*) \(.*#\1#p' | sort -u \
#         | (while read l; do dpkg -S $l; done) | cut -f1 -d: | sort -u
#
ARG OUINET_DEBUG=no
# This will also be used by the wrapper script.
ENV OUINET_DEBUG=$OUINET_DEBUG
RUN apt-get update && apt-get install -y \
    libc6 \
    libgcc1 \
    libssl1.1 \
    libstdc++6 \
    zlib1g \
    \
    ca-certificates \
    $(echo $OUINET_DEBUG | sed -n 's/^yes$/gdb/p') \
 && rm -rf /var/lib/apt/lists/*
# Manually install Boost libraries.
COPY --from=builder /usr/local/lib/libboost_* /usr/local/lib/
WORKDIR /opt/ouinet
# To get the list of locally built libraries to copy,
# enter the build directory and execute:
#
#     ldd injector client test/test-* $(find . -name '*.so' | grep -v '\.libs') \
#         | sed -En "s#^.* => ($PWD/.*) \(.*#\1#p" | sort -u \
#         | sed "s#$PWD#/opt/ouinet#"
#
COPY --from=builder \
     /opt/ouinet/gcrypt/src/gcrypt/src/.libs/libgcrypt.so.20 \
     /opt/ouinet/gpg_error/out/lib/libgpg-error.so.0 \
     /opt/ouinet/modules/asio-ipfs/ipfs_bindings/libipfs_bindings.so \
     \
     /usr/local/lib/
# Update the dynamic linker cache after all non-system libraries have been copied.
RUN ldconfig
# GNUnet support has been temporarily removed.
#COPY --from=builder /opt/ouinet/modules/gnunet-channels/gnunet-bin/share/gnunet/ modules/gnunet-channels/gnunet-bin/share/gnunet/
#COPY --from=builder /opt/ouinet/modules/gnunet-channels/gnunet-bin/lib/ modules/gnunet-channels/gnunet-bin/lib/
COPY --from=builder /opt/ouinet/injector /opt/ouinet/client ./
COPY --from=builder /usr/local/src/ouinet/scripts/ouinet-wrapper.sh ouinet
COPY --from=builder /opt/ouinet/test/test-* test/
COPY --from=builder /usr/local/src/ouinet/repos/ repo-templates/
ENTRYPOINT ["/opt/ouinet/ouinet"]
