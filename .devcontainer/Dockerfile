FROM docker.io/library/fedora:40

##install prereqs
RUN sudo dnf install -y \
    bash which git make cmake gcc mingw32-gcc mingw64-gcc\
    arm-none-eabi-gcc-cs  \
    arm-none-eabi-newlib \
    gcc-aarch64-linux-gnu \
    gcc-arm-linux-gnu \
    libusb-compat-0.1-devel libusb1-devel \
    python3 \
    dfu-util \
    wget gcc-c++ python perl-Pod-Simple gperf patch autoconf automake \ 
    makedepend bison flex ncurses-devel gmp-devel mpfr-devel libmpc-devel \
    gettext-devel texinfo rsync readline-devel

# installl libopencm3
RUN cd /usr/lib && \
    git clone https://github.com/libopencm3/libopencm3.git && \
    cd libopencm3 && \
    make

# installl stlink
RUN cd /usr && \
    git clone https://github.com/stlink-org/stlink.git && \
    cd stlink && \
    make

# Install Bebbo's amiga-gcc
RUN cd /tmp && \
    git clone https://github.com/bebbo/amiga-gcc amiga-gcc-13.2 && \
    export PREFIX=/opt/amiga13 && \
    mkdir -p $PREFIX && \
    cd amiga-gcc-13.2 && \
    make branch branch=amiga13.2 && \
    make update && \
    make all -j20 PREFIX=$PREFIX && \
    rm -rf /tmp/amiga-gcc-13.2

ENV PATH="$PATH:/opt/amiga13/bin"