#!/bin/sh
# Host-side tests for the shared SVD code. Needs only a C compiler.
set -e
cd "$(dirname "$0")"
${CC:-gcc} -std=c99 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -g \
    -I../common test_svd.c ../common/svd_format.c ../common/svd_render.c -o test_svd
./test_svd
