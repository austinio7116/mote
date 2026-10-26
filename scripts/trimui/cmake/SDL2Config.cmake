# TrimUI's SDK sysroot ships SDL2 2.26.1 without a CMake package config; this
# stands in for one. Links the device's own libSDL2 (it carries the TrimUI
# display backend, so a vendored SDL2 would not draw on the stock OS).
set(SDL2_INCLUDE_DIRS "$ENV{SYSROOT}/usr/include/SDL2" "$ENV{SYSROOT}/usr/include")
set(SDL2_LIBRARIES "$ENV{SYSROOT}/usr/lib/libSDL2.so")
set(SDL2_FOUND TRUE)
