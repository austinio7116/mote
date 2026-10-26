# Cross toolchain for the TrimUI Brick / Brick Hammer (Allwinner A133P, aarch64)
# on the stock TrimUI OS. A slim cut of LoveRetro/tg5040-toolchain (the one
# NextUI builds with): its GCC 8.3 aarch64 compiler plus TrimUI's own SDK
# sysroot, which carries the device's glibc and SDL2 headers/libs. Building
# against that sysroot is what keeps the binary loadable by the stock OS.
FROM docker.io/library/ubuntu:24.04
RUN apt-get update && apt-get install -y make cmake wget xz-utils ca-certificates python3 qemu-user \
    && rm -rf /var/lib/apt/lists/*
ENV CROSS_TRIPLE=aarch64-nextui-linux-gnu
ENV CROSS_ROOT=/opt/aarch64-nextui-linux-gnu
ENV SYSROOT=${CROSS_ROOT}/${CROSS_TRIPLE}/libc
RUN mkdir -p ${CROSS_ROOT} && wget -qO - https://github.com/LoveRetro/gcc-arm-8.3-aarch64-tg5040/releases/download/v8.3.0-20260204-190602-d37268f5/gcc-8.3.0-aarch64-nextui-linux-gnu-x86_64-host.tar.xz \
    | tar -xJ -C ${CROSS_ROOT} --strip-components=2
RUN mkdir -p ${SYSROOT} && wget -qO - https://github.com/trimui/toolchain_sdk_smartpro/releases/download/20231018/SDK_usr_tg5040_a133p.tgz \
    | tar -xzC ${SYSROOT}
ENV PATH=${CROSS_ROOT}/bin:${PATH} \
    CC=${CROSS_ROOT}/bin/${CROSS_TRIPLE}-gcc \
    CXX=${CROSS_ROOT}/bin/${CROSS_TRIPLE}-g++ \
    AR=${CROSS_ROOT}/bin/${CROSS_TRIPLE}-ar \
    STRIP=${CROSS_ROOT}/bin/${CROSS_TRIPLE}-strip
