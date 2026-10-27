# The Playdate SDK's Linux tools (pdc) are built against glibc 2.38; this
# runs them on an older host. The SDK itself is mounted at /sdk.
FROM docker.io/library/ubuntu:24.04
RUN apt-get update && apt-get install -y libpng16-16t64 zlib1g && rm -rf /var/lib/apt/lists/*
