# Build environment for memtest86plus + our board overlay.
# Used from macOS (or any non-Linux host) where cross-gcc is painful.
#
#   docker build -t a1990-memtest-build docker/
#   docker run --rm -v "$PWD":/work -w /work a1990-memtest-build make iso
#
# Host produces dist/a1990-memtest.iso owned by the container user; the
# top-level Makefile wrapper re-chowns back to the host user.

FROM --platform=linux/amd64 debian:bookworm-slim

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        gcc \
        binutils \
        make \
        git \
        python3 \
        python3-yaml \
        grub-common \
        grub-pc-bin \
        grub-efi-amd64-bin \
        xorriso \
        mtools \
        dosfstools \
        patch \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Treat bind-mounted repo as "safe" even when UID mismatches.
RUN git config --system --add safe.directory '*'

WORKDIR /work
