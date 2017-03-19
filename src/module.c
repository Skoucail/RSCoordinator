#include <stdlib.h>
#include <string.h>

#include "redismodule.h"
#include "rmr.h"
#include "hiredis/async.h"
#include "reply.h"
#include "fnv.h"

/* A reducer that just chains the replies from a map request */
int chainReplyReducer(struct MRCtx *mc, int count, MRReply **replies) {

  RedisModuleCtx *ctx = MRCtx_GetPrivdata(mc);

  RedisModule_ReplyWithArray(ctx, count);
  for (int i = 0; i < count; i++) {
    MR_ReplyWithMRReply(ctx, replies[i]);
  }
  return REDISMODULE_OK;
}

/* A reducer that sums up numeric replies from a request */
int sumReducer(struct MRCtx *mc, int count, MRReply **replies) {

  RedisModuleCtx *ctx = MRCtx_GetPrivdata(mc);
  long long sum = 0;
  for (int i = 0; i < count; i++) {
    long long n = 0;
    if (MRReply_ToInteger(replies[i], &n)) {
      sum += n;
    }
  }

  return RedisModule_ReplyWithLongLong(ctx, sum);
}

int SumAggCmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  if (argc < 2) {
    return RedisModule_WrongArity(ctx);
  }
  RedisModule_AutoMemory(ctx);

  MRCommand cmds[argc - 1];
  for (int i = 0; i < argc - 1; i++) {
    cmds[i] = MR_NewCommand(2, "GET", RedisModule_StringPtrLen(argv[i + 1], NULL));
  }

  MR_Map(MR_CreateCtx(ctx), sumReducer, cmds, argc - 1);

  // MR_Fanout(MR_CreateCtx(ctx), sumReducer, cmd);

  return REDISMODULE_OK;
}

int TestCmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  if (argc < 2) {
    return RedisModule_WrongArity(ctx);
  }
  RedisModule_AutoMemory(ctx);

  struct MRCtx *mc = MR_CreateCtx(ctx);
  MR_Fanout(mc, chainReplyReducer, MR_NewCommandFromRedisStrings(argc - 1, &argv[1]));

  return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx) {

  MRTopologyProvider tp = NewStaticTopologyProvider(4096, 2, "localhost:6375", "localhost:6376",
                                                    "localhost:6377", "localhost:6378");
  MRCluster *cl = MR_NewCluster(tp, CRC16ShardFunc);
  MR_Init(cl);

  if (RedisModule_Init(ctx, "rmr", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  // register index type

  if (RedisModule_CreateCommand(ctx, "rmr.test", TestCmd, "readonly", 1, 1, 1) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  if (RedisModule_CreateCommand(ctx, "rmr.sum", SumAggCmd, "readonly", 1, 1, 1) == REDISMODULE_ERR)
    return REDISMODULE_ERR;

  return REDISMODULE_OK;
}