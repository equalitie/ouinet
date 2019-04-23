FROM debian:stretch AS builder
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
ENV LANG=C.UTF-8
