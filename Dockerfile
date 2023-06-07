ARG fedora_version=latest
FROM fedora:${fedora_version} as builder
# docker build -t stg .
# Many of these dependencies are for libabigail
RUN dnf install -y \
    autoconf \
    automake \
    cpio \
    elfutils-devel \
    gcc-c++ \
    git \
    lbzip2 \
    libbpf-devel \
    libicu-devel \
    libtool \
    libxml2-devel \
    python3-koji \
    python3-mock \
    python3-pyxdg \
    shared-mime-info \
    six \
    wget
RUN git clone git://sourceware.org/git/libabigail.git /code && \
    cd /code && \
    git fetch && \
    git checkout libabigail-2.0 && \
    mkdir -p build && \
    autoreconf -i && \
    cd build && \
    ../configure --prefix=/opt/libabigail --disable-shared && \
    make -j $(nproc) all && \
    make install
ENV PATH=/opt/libabigail/bin:$PATH
WORKDIR /src
COPY . /src
RUN make
