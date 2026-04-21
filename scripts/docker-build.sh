#!/usr/bin/env bash
# Run the build inside a container so macOS/Windows hosts don't need
# cross-gcc, grub-mkrescue, xorriso, mtools locally.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="a1990-memtest-build"
target="${1:-iso}"

if ! command -v docker >/dev/null; then
    echo "error: docker not found in PATH"
    exit 1
fi

# Build image if missing or Dockerfile is newer than image.
need_build=0
if ! docker image inspect "$image" >/dev/null 2>&1; then
    need_build=1
else
    img_ts=$(docker image inspect -f '{{.Created}}' "$image" | xargs -I{} date -j -f '%Y-%m-%dT%H:%M:%S' '{}' +%s 2>/dev/null || echo 0)
    df_ts=$(stat -f %m "$here/docker/Dockerfile" 2>/dev/null || stat -c %Y "$here/docker/Dockerfile")
    [[ "$df_ts" -gt "$img_ts" ]] && need_build=1
fi

if [[ "$need_build" -eq 1 ]]; then
    echo "==> docker build $image"
    docker build --platform linux/amd64 -t "$image" "$here/docker"
fi

echo "==> docker run: make $target"
docker run --rm \
    --platform linux/amd64 \
    -v "$here":/work \
    -w /work \
    -e HOST_UID="$(id -u)" \
    -e HOST_GID="$(id -g)" \
    "$image" \
    bash -lc "make $target && chown -R \$HOST_UID:\$HOST_GID dist memtest86plus/build memtest86plus/system/board src/board_table.c 2>/dev/null || true"
