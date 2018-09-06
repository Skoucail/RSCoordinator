#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "dep/rmutil/util.h"
#include "dep/rmutil/strings.h"
#include "dep/rmr/endpoint.h"
#include "dep/rmr/hiredis/hiredis.h"

#define CONFIG_SETTER(name) \
  static int name(RSConfig *config, RedisModuleString **argv, size_t argc, size_t *offset)

#define CONFIG_GETTER(name) static sds name(const RSConfig *config)
#define CONFIG_FROM_RSCONFIG(c) ((SearchClusterConfig *)(c)->chainedConfig)

static SearchClusterConfig* getOrCreateRealConfig(RSConfig *config){
  if(!CONFIG_FROM_RSCONFIG(config)){
    config->chainedConfig = &clusterConfig;
  }
  return CONFIG_FROM_RSCONFIG(config);
}

// PARTITIONS
CONFIG_SETTER(setNumPartitions) {
  if (*offset == argc) {
    return REDISMODULE_ERR;
  }
  SearchClusterConfig *realConfig = getOrCreateRealConfig(config);
  RedisModuleString *s = argv[(*offset)++];
  const char *sstr = RedisModule_StringPtrLen(s, NULL);
  if (!strcasecmp(sstr, "AUTO")) {
    realConfig->numPartitions = 0;
  } else {
    long long ll = 0;
    if (RedisModule_StringToLongLong(s, &ll) != REDISMODULE_OK || ll < 0) {
      return REDISMODULE_ERR;
    }
    realConfig->numPartitions = ll;
  }
  return REDISMODULE_OK;
}

CONFIG_GETTER(getNumPartitions) {
  SearchClusterConfig *realConfig = getOrCreateRealConfig((RSConfig *)config);
  sds ss = sdsempty();
  return sdscatprintf(ss, "%lld", realConfig->numPartitions);
}

// TIMEOUT
CONFIG_SETTER(setTimeout) {
  if (*offset == argc) {
    return REDISMODULE_ERR;
  }
  SearchClusterConfig *realConfig = getOrCreateRealConfig(config);
  RedisModuleString *s = argv[(*offset)++];
  long long ll;
  if (RedisModule_StringToLongLong(s, &ll) != REDISMODULE_OK || ll < 0) {
    return REDISMODULE_ERR;
  }
  if (ll > 0) {
    realConfig->timeoutMS = ll;
  }
  return REDISMODULE_OK;
}

CONFIG_GETTER(getTimeout) {
  SearchClusterConfig *realConfig = getOrCreateRealConfig((RSConfig *)config);
  sds ss = sdsempty();
  return sdscatprintf(ss, "%lld", realConfig->timeoutMS);
}

static RSConfigOptions clusterOptions_g = {
    .vars =
        {
            {.name = "PARTITIONS",
             .helpText = "Number of RediSearch partitions to use",
             .setValue = setNumPartitions,
             .getValue = getNumPartitions,
             .flags = RSCONFIGVAR_F_IMMUTABLE},
            {.name = "TIMEOUT",
             .helpText = "Cluster synchronization timeout",
             .setValue = setTimeout,
             .getValue = getTimeout},
            {.name = NULL}
            // fin
        }
    // fin
};

SearchClusterConfig clusterConfig = {0};

/* Detect the cluster type, by trying to see if we are running inside RLEC.
 * If we cannot determine, we return OSS type anyway
 */
MRClusterType DetectClusterType() {
  RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);

  RedisModuleCallReply *r = RedisModule_Call(ctx, "INFO", "c", "SERVER");
  MRClusterType ret = ClusterType_RedisOSS;

  if (r && RedisModule_CallReplyType(r) == REDISMODULE_REPLY_STRING) {
    size_t len;
    // INFO SERVER should contain the term rlec_version in it if we are inside an RLEC shard

    const char *str = RedisModule_CallReplyStringPtr(r, &len);
    if (str) {

      if (memmem(str, len, "rlec_version", strlen("rlec_version")) != NULL) {
        ret = ClusterType_RedisLabs;
      }
    }
    RedisModule_FreeCallReply(r);
  }
  // RedisModule_ThreadSafeContextUnlock(ctx);
  RedisModule_FreeThreadSafeContext(ctx);
  return ret;
}

RSConfigOptions *GetClusterConfigOptions(void) {
  return &clusterOptions_g;
}
