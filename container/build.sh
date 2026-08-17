#!/bin/sh
set -eu

engine=${CONTAINER_ENGINE:-podman}
container_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)

if ! command -v "$engine" >/dev/null 2>&1; then
    echo "container/build.sh: $engine is not available" >&2
    exit 1
fi

exec "$engine" build \
    --pull=missing \
    --tag cma \
    --file "$container_dir/Containerfile" \
    "$container_dir"
