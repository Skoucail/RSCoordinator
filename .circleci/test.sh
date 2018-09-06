#!/bin/bash
set -x
set -e

MODULE_OSS_SO=$BUILD_DIR/module-oss.so

if [ -n "$REDISEARCH_CI_SKIP_TESTS" ]; then
    exit 0
fi

ROOT=$PWD
cd $BUILD_DIR

cat > rmtest.config << EOF
[server]
module = $ROOT/$MODULE_OSS_SO
EOF

ctest -V
RLTest --env oss-cluster --env-reuse --tests-dir $ROOT/src/dep/RediSearch/src/pytest/ --clear-logs --shards-count 3 --module $ROOT/$MODULE_OSS_SO --module-args "PARTITIONS AUTO"
RLTest --env oss-cluster --env-reuse --tests-dir $ROOT/src/dep/RediSearch/src/pytest/ --clear-logs --shards-count 3 --module $ROOT/$MODULE_OSS_SO --module-args "PARTITIONS AUTO SAFEMODE"
