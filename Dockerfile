FROM rust:slim-bookworm AS base
ENV LANG=C.UTF-8

RUN apt-get update && apt-get upgrade -y
RUN apt-get install -y \
      build-essential \
      git \
      libssl-dev \
      python3-twisted \
      zlib1g-dev
# quieten wget and unzip
RUN echo 'quiet = on' >> /etc/wgetrc
WORKDIR /usr/local/src

FROM base AS builder
# This version is a recommendation and this file has been tested to work for it,
# but you may attempt to build other versions by overriding this argument.
# Also see `OUINET_DOCKER_VERSION` below.
ARG OUINET_VERSION=v1.4.0
ARG CMAKE_VERSION=3.31.7-linux-x86_64
RUN git clone --recursive -b "$OUINET_VERSION" https://gitlab.com/equalitie/ouinet.git
WORKDIR /opt/ouinet
# The C.UTF-8 locale (which is always available in Debian)
# is needed to allow CMake to extract files in the Go language binary distribution
# with UTF-8-encoded Unicode names.
RUN /usr/local/src/ouinet/scripts/install-cmake.sh "$CMAKE_VERSION"
ENV PATH="/opt/cmake/cmake-$CMAKE_VERSION/bin:$PATH"
ARG OUINET_DEBUG=no
RUN \
if [ $OUINET_DEBUG = yes ]; then \
    cmake /usr/local/src/ouinet -DCMAKE_BUILD_TYPE=Debug && make -j $(nproc); \
else \
    cmake /usr/local/src/ouinet && make -j $(nproc); \
fi
RUN cp -r /usr/local/src/ouinet/repos/ repo-templates/
RUN \
if [ $OUINET_DEBUG != yes ]; then \
    strip injector client test/bt-* test/oui-* \
        && find . -name '*.so' -exec strip '{}' + \
        && find . -wholename '*/libexec/*' -executable -type f -exec strip '{}' + ; \
fi
# Setting this to a different version than `OUINET_VERSION` allows to
# use that version's Docker-specific files (e.g. wrapper scripts)
# without having to rebuild source.
# Maybe those Docker-specific files should go in a different repo.
ARG OUINET_DOCKER_VERSION=$OUINET_VERSION
RUN cd /usr/local/src/ouinet \
 && git fetch -t \
 && git checkout "$OUINET_DOCKER_VERSION"
# Populate the licenses directory (avoid version numbers in source paths).
RUN /usr/local/src/ouinet/scripts/add-licenses-dir.sh /usr/local/src/ouinet .

FROM debian:bookworm-slim
# To get the list of system library packages to install,
# enter the build directory and execute:
#
#     ldd injector client $(find . -name '*.so' | grep -v '\.libs') \
#         | sed -En 's#^.* => (/lib/.*|/usr/lib/.*) \(.*#\1#p' | sort -u \
#         | (while read l; do dpkg -S $l; done) | cut -f1 -d: | sort -u
#
ARG OUINET_DEBUG=no
# This will also be used by the wrapper script.
ENV OUINET_DEBUG=$OUINET_DEBUG
RUN apt-get update && apt-get install -y \
    libc6 \
    libgcc1 \
    libssl3 \
    libstdc++6 \
    zlib1g \
    \
    ca-certificates \
    $(echo $OUINET_DEBUG | sed -n 's/^yes$/gdb/p') \
    \
    lsb-release \
    netcat-openbsd \
    wget \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/ouinet
# Copy locally built libraries (all placed along binaries).
RUN mkdir /opt/ouinet/lib
COPY --from=builder /opt/ouinet/lib* /opt/ouinet/lib
# Update the dynamic linker cache after all non-system libraries have been copied.
# This also creates the appropriate symbolic links to those libraries.
RUN ldconfig
COPY --from=builder /opt/ouinet/injector /opt/ouinet/client ./
COPY --from=builder /opt/ouinet/repo-templates/ repo-templates/
RUN mkdir utils
COPY --from=builder \
 /opt/ouinet/test/bt-* /opt/ouinet/test/oui-* \
 /usr/local/src/ouinet/scripts/ping-swarm \
 utils/
# This ensures that we use the desired Docker-specific files.
RUN echo "$OUINET_DOCKER_VERSION"
COPY --from=builder /usr/local/src/ouinet/scripts/ouinet-wrapper.sh ouinet
COPY --from=builder /opt/ouinet/licenses/ licenses/
# This last step pulls in latest updates to Debian packages
# (only if something changed above)
# since the base image may not have been upgraded in a long while.
RUN apt-get update && apt-get upgrade -y \
 && rm -rf /var/lib/apt/lists/*
ENTRYPOINT ["/opt/ouinet/ouinet"]
