#!/bin/bash

# Settings
BIN_PATH="/home/ariel/redis/redis/src"
CLUSTER_HOST=127.0.0.1
PORT=30000
TIMEOUT=2000
NODES=5
REPLICAS=0
PROTECTED_MODE=no
ADDITIONAL_OPTIONS=" --loadmodule /home/ariel/redis/RedisJSON/target/debug/librejson.so --loadmodule /home/ariel/redis/RSCoordinator/build/module-oss.so"

# You may want to put the above config parameters into config.sh in order to
# override the defaults without modifying this script.

if [ -a config.sh ]
then
    source "config.sh"
fi

# Computed vars
ENDPORT=$((PORT+NODES))

if [ "$1" == "start" ]
then
    while [ $((PORT < ENDPORT)) != "0" ]; do
        PORT=$((PORT+1))
        echo "Starting $PORT"
        $BIN_PATH/redis-server --port $PORT  --protected-mode $PROTECTED_MODE --cluster-enabled yes --cluster-config-file nodes-${PORT}.conf --cluster-node-timeout $TIMEOUT --appendonly no --appendfilename appendonly-${PORT}.aof --save "" --dbfilename dump-${PORT}.rdb --logfile ${PORT}.log --daemonize yes ${ADDITIONAL_OPTIONS}
        # /home/ariel/redis/redis/src /redis-server --port 30001 --protected-mode no --cluster-enabled yes --cluster-config-file nodes-30001.conf --cluster-node-timeout 2000 --appendonly no --appendfilename appendonly-30001.aof --save "" --dbfilename dump-30001.rdb --logfile 30001.log --daemonize yes ${ADDITIONAL_OPTIONS}
    done
    exit 0
fi

if [ "$1" == "create" ]
then
    HOSTS=""
    while [ $((PORT < ENDPORT)) != "0" ]; do
        PORT=$((PORT+1))
        HOSTS="$HOSTS $CLUSTER_HOST:$PORT"
    done
    OPT_ARG=""
    if [ "$2" == "-f" ]; then
        OPT_ARG="--cluster-yes"
    fi
    $BIN_PATH/redis-cli --cluster create $HOSTS --cluster-replicas $REPLICAS $OPT_ARG
    exit 0
fi

if [ "$1" == "stop" ]
then
    while [ $((PORT < ENDPORT)) != "0" ]; do
        PORT=$((PORT+1))
        echo "Stopping $PORT"
        $BIN_PATH/redis-cli -p $PORT shutdown nosave
    done
    exit 0
fi