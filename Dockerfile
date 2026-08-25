FROM archlinux:latest

RUN pacman --noconfirm -Syu
RUN pacman --noconfirm -S --needed git base-devel
RUN pacman --noconfirm -S --needed mingw-w64-toolchain cmake ninja zstd

RUN useradd -m builder \
    && echo 'builder ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/builder

RUN echo 'MAKEFLAGS="-j8"' >> /etc/makepkg.conf

USER builder
WORKDIR /home/builder

RUN git clone https://aur.archlinux.org/yay-bin.git \
    && cd yay-bin \
    && makepkg -si --noconfirm

RUN yay -S --noconfirm --needed mingw-w64-cmake


ENV OPENSSL_ROOT_DIR=/usr/x86_64-w64-mingw32/

ENTRYPOINT [ "/usr/bin/env", "bash" ]
