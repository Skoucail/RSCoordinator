#!/bin/bash

set -xe

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT=$(cd $HERE/.. && pwd)

[[ -n "$REDISEARCH_CI_SKIP_TESTS" ]] && exit 0

BUILD_DIR=${BUILD_DIR:-build}
BUILD_DIR=$(cd $BUILD_DIR; pwd)

cd $BUILD_DIR
ctest -V

cd $ROOT
MODULE=$BUILD_DIR/module-oss.so
test_args="--env oss-cluster --env-reuse --clear-logs --shards-count 3"
test_cmd="$ROOT/src/dep/RediSearch/tests/pytests/runtests.sh $MODULE $test_args"

export EXT_TEST_PATH=src/dep/RediSearch/tests/ctests/ext-example/libexample_extension.so

MODARGS="PARTITIONS AUTO" $test_cmd
MODARGS="PARTITIONS AUTO SAFEMODE" $test_cmd
