#include <stdlib.h>
#include <string.h>

#include "redismodule.h"
#include "dep/rmr/rmr.h"
#include "dep/rmr/hiredis/async.h"
#include "dep/rmr/reply.h"
#include "dep/rmutil/util.h"
#include "dep/rmutil/strings.h"
#include "crc16_tags.h"
#include "crc12_tags.h"
#include "dep/rmr/redis_cluster.h"
#include "dep/rmr/redise.h"
#include "fnv32.h"
#include "dep/heap.h"
#include "search_cluster.h"
#include "config.h"
#include "dep/RediSearch/src/module.h"
#include <math.h>
#include "info_command.h"
#include "version.h"
#include "build-info/info.h"
#include <sys/param.h>
#include <pthread.h>
#include <aggregate/aggregate.h>


#define CLUSTERDOWN_ERR "Uninitialized cluster state, could not perform command"

/* A reducer that just chains the replies from a map request */
int chainReplyReducer(struct MRCtx *mc, int count, MRReply **replies) {

  RedisModuleCtx *ctx = MRCtx_GetRedisCtx(mc);

  RedisModule_ReplyWithArray(ctx, count);
  for (int i = 0; i < count; i++) {
    MR_ReplyWithMRReply(ctx, replies[i]);
  }
  // RedisModule_ReplySetArrayLength(ctx, x);
  return REDISMODULE_OK;
}

/* A reducer that just merges N arrays of strings by chaining them into one big array with no
 * duplicates */
int uniqueStringsReducer(struct MRCtx *mc, int count, MRReply **replies) {
  RedisModuleCtx *ctx = MRCtx_GetRedisCtx(mc);

  MRReply *err = NULL;

  TrieMap *dict = NewTrieMap();
  int nArrs = 0;
  // Add all the array elements into the dedup dict
  for (int i = 0; i < count; i++) {
    if (replies[i] && MRReply_Type(replies[i]) == MR_REPLY_ARRAY) {
      nArrs++;
      for (size_t j = 0; j < MRReply_Length(replies[i]); j++) {
        size_t sl = 0;
        char *s = MRReply_String(MRReply_ArrayElement(replies[i], j), &sl);
        if (s && sl) {
          TrieMap_Add(dict, s, sl, NULL, NULL);
        }
      }
    } else if (MRReply_Type(replies[i]) == MR_REPLY_ERROR && err == NULL) {
      err = replies[i];
    }
  }

  // if there are no values - either reply with an empty array or an error
  if (dict->cardinality == 0) {

    if (nArrs > 0) {
      // the arrays were empty - return an empty array
      RedisModule_ReplyWithArray(ctx, 0);
    } else {
      return RedisModule_ReplyWithError(ctx, err ? (const char *)err : "Could not perfrom query");
    }
    goto cleanup;
  }

  char *s;
  tm_len_t sl;
  void *p;
  // Iterate the dict and reply with all values
  TrieMapIterator *it = TrieMap_Iterate(dict, "", 0);
  RedisModule_ReplyWithArray(ctx, dict->cardinality);
  while (TrieMapIterator_Next(it, &s, &sl, &p)) {
    RedisModule_ReplyWithStringBuffer(ctx, s, sl);
  }

  TrieMapIterator_Free(it);

cleanup:
  TrieMap_Free(dict, NULL);

  return REDISMODULE_OK;
}
/* A reducer that just merges N arrays of the same length, selecting the first non NULL reply from
 * each */
int mergeArraysReducer(struct MRCtx *mc, int count, MRReply **replies) {

  RedisModuleCtx *ctx = MRCtx_GetRedisCtx(mc);

  int j = 0;
  int stillValid;
  do {
    // the number of still valid arrays in the response
    stillValid = 0;

    for (int i = 0; i < count; i++) {
      // if this is not an array - ignore it
      if (MRReply_Type(replies[i]) != MR_REPLY_ARRAY) continue;
      // if we've overshot the array length - ignore this one
      if (MRReply_Length(replies[i]) <= j) continue;
      // increase the number of valid replies
      stillValid++;

      // get the j element of array i
      MRReply *ele = MRReply_ArrayElement(replies[i], j);
      // if it's a valid response OR this is the last array we are scanning -
      // add this element to the merged array
      if (MRReply_Type(ele) != MR_REPLY_NIL || i + 1 == count) {
        // if this is the first reply - we need to crack open a new array reply
        if (j == 0) RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

        MR_ReplyWithMRReply(ctx, ele);
        j++;
        break;
      }
    }
  } while (stillValid > 0);

  // j 0 means we could not process a single reply element from any reply
  if (j == 0) {
    return RedisModule_ReplyWithError(ctx, "Could not process replies");
  }
  RedisModule_ReplySetArrayLength(ctx, j);

  return REDISMODULE_OK;
}

int singleReplyReducer(struct MRCtx *mc, int count, MRReply **replies) {

  RedisModuleCtx *ctx = MRCtx_GetRedisCtx(mc);
  if (count == 0) {
    return RedisModule_ReplyWithNull(ctx);
  }

  MR_ReplyWithMRReply(ctx, replies[0]);

  return REDISMODULE_OK;
}
// a reducer that expects "OK" reply for all replies, and stops at the first error and returns it
int allOKReducer(struct MRCtx *mc, int count, MRReply **replies) {
  RedisModuleCtx *ctx = MRCtx_GetRedisCtx(mc);
  if (count == 0) {
    RedisModule_ReplyWithError(ctx, "Could not distribute comand");
    return REDISMODULE_OK;
  }
  for (int i = 0; i < count; i++) {
    if (MRReply_Type(replies[i]) == MR_REPLY_ERROR) {
      MR_ReplyWithMRReply(ctx, replies[i]);
      return REDISMODULE_OK;
    }
  }

  RedisModule_ReplyWithSimpleString(ctx, "OK");
  return REDISMODULE_OK;
}

typedef struct {
  char *id;
  double score;
  MRReply *fields;
  MRReply *payload;
  const char *sortKey;
  double sortKeyNum;
} searchResult;

typedef struct {
  char *queryString;
  long long offset;
  long long limit;
  int withScores;
  int withPayload;
  int withSortby;
  int sortAscending;
  int withSortingKeys;
  int noContent;

} searchRequestCtx;

void searchRequestCtx_Free(searchRequestCtx *r) {
  free(r->queryString);
  free(r);
}

searchRequestCtx *rscParseRequest(RedisModuleString **argv, int argc) {
  /* A search request must have at least 3 args */
  if (argc < 3) {
    return NULL;
  }

  searchRequestCtx *req = malloc(sizeof(searchRequestCtx));
  req->queryString = strdup(RedisModule_StringPtrLen(argv[2], NULL));
  req->limit = 10;
  req->offset = 0;
  // marks the user set WITHSCORES. internally it's always set
  req->withScores = RMUtil_ArgExists("WITHSCORES", argv, argc, 3) != 0;

  // Parse SORTBY ... ASC
  int sortByIndex = RMUtil_ArgIndex("SORTBY", argv, argc);
  req->withSortby = sortByIndex > 2;
  req->sortAscending = 1;
  if (req->withSortby && sortByIndex + 2 < argc) {
    if (RMUtil_StringEqualsCaseC(argv[sortByIndex + 2], "DESC")) {
      req->sortAscending = 0;
    }
  }

  req->withSortingKeys = RMUtil_ArgExists("WITHSORTKEYS", argv, argc, 3) != 0;
  // fprintf(stderr, "Sortby: %d, asc: %d withsort: %d\n", req->withSortby, req->sortAscending,
  //         req->withSortingKeys);

  // Detect "NOCONTENT"
  req->noContent = RMUtil_ArgExists("NOCONTENT", argv, argc, 3) != 0;

  // if RETURN exists - make sure we don't have RETURN 0
  if (!req->noContent && RMUtil_ArgExists("RETURN", argv, argc, 3)) {
    long long numReturns = -1;
    RMUtil_ParseArgsAfter("RETURN", argv, argc, "l", &numReturns);
    // RETURN 0 equals NOCONTENT
    if (numReturns <= 0) {
      req->noContent = 1;
    }
  }

  req->withPayload = RMUtil_ArgExists("WITHPAYLOADS", argv, argc, 3) != 0;

  // Parse LIMIT argument
  RMUtil_ParseArgsAfter("LIMIT", argv, argc, "ll", &req->offset, &req->limit);
  if (req->limit <= 0) req->limit = 10;
  if (req->offset <= 0) req->offset = 0;

  return req;
}

int cmp_results(const void *p1, const void *p2, const void *udata) {

  const searchResult *r1 = p1, *r2 = p2;

  const searchRequestCtx *req = udata;
  int cmp = 0;
  // Compary by sorting keys
  if (r1->sortKey && r2->sortKey && req->withSortby) {
    // Sort by numeric sorting keys
    if (r1->sortKeyNum != HUGE_VAL && r2->sortKeyNum != HUGE_VAL) {
      double diff = r2->sortKeyNum - r1->sortKeyNum;
      cmp = diff < 0 ? -1 : (diff > 0 ? 1 : 0);
    } else {
      // Sort by string sort keys
      cmp = strcmp(r2->sortKey, r1->sortKey);
    }
    // in case of a tie - compare ids
    if (!cmp) cmp = strcmp(r2->id, r1->id);
    return (req->sortAscending ? -cmp : cmp);
  }

  double s1 = r1->score, s2 = r2->score;

  return s1 < s2 ? 1 : (s1 > s2 ? -1 : strcmp(r2->id, r1->id));
}

searchResult *newResult(searchResult *cached, MRReply *arr, int j, int scoreOffset,
                        int payloadOffset, int fieldsOffset, int sortKeyOffset) {
  searchResult *res = cached ? cached : malloc(sizeof(searchResult));
  res->sortKey = NULL;
  res->sortKeyNum = HUGE_VAL;
  res->id = MRReply_String(MRReply_ArrayElement(arr, j), NULL);
  // if the id contains curly braces, get rid of them now
  if (res->id) {
    char *brace = strchr(res->id, '{');
    if (brace && strchr(brace, '}')) {
      *brace = '\0';
    }
  } else {  // this usually means an invalid result
    return res;
  }
  // parse socre
  MRReply_ToDouble(MRReply_ArrayElement(arr, j + scoreOffset), &res->score);
  // get fields
  res->fields = fieldsOffset > 0 ? MRReply_ArrayElement(arr, j + fieldsOffset) : NULL;
  // get payloads
  res->payload = payloadOffset > 0 ? MRReply_ArrayElement(arr, j + payloadOffset) : NULL;

  res->sortKey =
      sortKeyOffset > 0 ? MRReply_String(MRReply_ArrayElement(arr, j + sortKeyOffset), NULL) : NULL;
  if (res->sortKey) {
    if (res->sortKey[0] == '#') {
      char *eptr;
      double d = strtod(res->sortKey + 1, &eptr);
      if (eptr != res->sortKey + 1 && *eptr == 0) {
        res->sortKeyNum = d;
      }
    }
    // fprintf(stderr, "Sort key string '%s', num '%f\n", res->sortKey, res->sortKeyNum);
  }
  return res;
}

int searchResultReducer(struct MRCtx *mc, int count, MRReply **replies) {
  RedisModuleCtx *ctx = MRCtx_GetRedisCtx(mc);
  searchRequestCtx *req = MRCtx_GetPrivdata(mc);

  // got no replies - this means timeout
  if (count == 0 || req->limit < 0) {
    return RedisModule_ReplyWithError(ctx, "Could not send query to cluster");
  }

  long long total = 0;
  MRReply *lastError = NULL;
  size_t num = req->offset + req->limit;
  heap_t *pq = malloc(heap_sizeof(num));
  heap_init(pq, cmp_results, req, num);
  searchResult *cached = NULL;
  for (int i = 0; i < count; i++) {
    MRReply *arr = replies[i];
    if (!arr) continue;
    if (MRReply_Type(arr) == MR_REPLY_ERROR) {
      lastError = arr;
      continue;
    }
    if (MRReply_Type(arr) == MR_REPLY_ARRAY && MRReply_Length(arr) > 0) {
      // first element is always the total count
      total += MRReply_Integer(MRReply_ArrayElement(arr, 0));
      size_t len = MRReply_Length(arr);

      int step = 3;  // 1 for key, 1 for score, 1 for fields
      int scoreOffset = 1, fieldsOffset = 2, payloadOffset = -1, sortKeyOffset = -1;
      if (req->withPayload) {  // save an extra step for payloads
        step++;
        payloadOffset = 2;
        fieldsOffset = 3;
      }
      if (req->withSortby || req->withSortingKeys) {
        step++;
        sortKeyOffset = fieldsOffset++;
      }
      // nocontent - one less field, and the offset is -1 to avoid parsing it
      if (req->noContent) {
        step--;
        fieldsOffset = -1;
      }
      // fprintf(stderr, "Step %d, scoreOffset %d, fieldsOffset %d, sortKeyOffset %d\n", step,
      //         scoreOffset, fieldsOffset, sortKeyOffset);
      for (int j = 1; j < len; j += step) {
        searchResult *res =
            newResult(cached, arr, j, scoreOffset, payloadOffset, fieldsOffset, sortKeyOffset);
        if (!res || !res->id) {
          // invalid result - usually means something is off with the response, and we should just
          // quit this response
          cached = res;
          break;
        } else {
          cached = NULL;
        }
        // fprintf(stderr, "Response %d result %d Reply docId %s score: %f sortkey %f\n", i, j,
        //         res->id, res->score, res->sortKeyNum);

        if (heap_count(pq) < heap_size(pq)) {
          // printf("Offering result score %f\n", res->score);
          heap_offerx(pq, res);

        } else {
          searchResult *smallest = heap_peek(pq);
          int c = cmp_results(res, smallest, req);
          if (c < 0) {
            smallest = heap_poll(pq);
            heap_offerx(pq, res);
            cached = smallest;
          } else {
            // If the result is lower than the last result in the heap - we can stop now
            cached = res;
            break;
          }
        }
      }
    }
  }
  if (cached) free(cached);
  // If we didn't get any results and we got an error - return it.
  // If some shards returned results and some errors - we prefer to show the results we got an not
  // return an error. This might change in the future
  if (total == 0 && lastError != NULL) {
    MR_ReplyWithMRReply(ctx, lastError);
    searchRequestCtx_Free(req);

    return REDISMODULE_OK;
  }

  // Reverse the top N results
  size_t qlen = heap_count(pq);

  // // If we didn't get enough results - we return nothing
  // if (qlen <= req->offset) {
  //   qlen = 0;
  // }

  size_t pos = qlen;
  searchResult *results[qlen];
  while (pos) {
    results[--pos] = heap_poll(pq);
  }
  heap_free(pq);

  RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
  int len = 1;
  RedisModule_ReplyWithLongLong(ctx, total);
  for (pos = req->offset; pos < qlen && pos < num; pos++) {
    searchResult *res = results[pos];
    RedisModule_ReplyWithStringBuffer(ctx, res->id, strlen(res->id));
    len++;
    if (req->withScores) {
      RedisModule_ReplyWithDouble(ctx, res->score);
      len++;
    }
    if (req->withPayload) {

      MR_ReplyWithMRReply(ctx, res->payload);
      len++;
    }
    if (req->withSortingKeys && req->withSortby) {
      len++;
      if (res->sortKey) {
        RedisModule_ReplyWithStringBuffer(ctx, res->sortKey, strlen(res->sortKey));
      } else {
        RedisModule_ReplyWithNull(ctx);
      }
    }
    if (!req->noContent) {
      MR_ReplyWithMRReply(ctx, res->fields);
      len++;
    }
  }
  RedisModule_ReplySetArrayLength(ctx, len);
  for (pos = 0; pos < qlen; pos++) {
    free(results[pos]);
  }

  searchRequestCtx_Free(req);

  return REDISMODULE_OK;
}

/* ft.ADD {index} ... */
int SingleShardCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  if (argc < 2) {
    return RedisModule_WrongArity(ctx);
  }
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  RedisModule_AutoMemory(ctx);

  MRCommand cmd = MR_NewCommandFromRedisStrings(argc, argv);
  /* Replace our own FT command with _FT. command */
  MRCommand_SetPrefix(&cmd, "_FT");
  int partPos = MRCommand_GetPartitioningKey(&cmd);

  /* Rewrite the sharding key based on the partitioning key */
  if (partPos > 0) {
    SearchCluster_RewriteCommand(GetSearchCluster(), &cmd, partPos);
  }

  /* Rewrite the partitioning key as well */

  if (MRCommand_GetFlags(&cmd) & MRCommand_MultiKey) {
    if (partPos > 0) {
      SearchCluster_RewriteCommandArg(GetSearchCluster(), &cmd, partPos, partPos);
    }
  }
  // MRCommand_Print(&cmd);
  MR_MapSingle(MR_CreateCtx(ctx, NULL), singleReplyReducer, cmd);

  return REDISMODULE_OK;
}

/* FT.MGET {idx} {key} ... */
int MGetCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  if (argc < 3) {
    return RedisModule_WrongArity(ctx);
  }
  // Check that the cluster state is valid
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  RedisModule_AutoMemory(ctx);

  MRCommand cmd = MR_NewCommandFromRedisStrings(argc, argv);
  /* Replace our own FT command with _FT. command */
  MRCommand_SetPrefix(&cmd, "_FT");
  for (int i = 2; i < argc; i++) {
    SearchCluster_RewriteCommandArg(GetSearchCluster(), &cmd, i, i);
  }

  MRCommandGenerator cg = SearchCluster_MultiplexCommand(GetSearchCluster(), &cmd);
  struct MRCtx *mrctx = MR_CreateCtx(ctx, NULL);
  MR_SetCoordinationStrategy(mrctx, MRCluster_MastersOnly | MRCluster_FlatCoordination);
  MR_Map(mrctx, mergeArraysReducer, cg);
  cg.Free(cg.ctx);
  return REDISMODULE_OK;
}

int MastersFanoutCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc < 2) {
    return RedisModule_WrongArity(ctx);
  }
  // Check that the cluster state is valid
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  RedisModule_AutoMemory(ctx);

  MRCommand cmd = MR_NewCommandFromRedisStrings(argc, argv);
  /* Replace our own FT command with _FT. command */
  MRCommand_SetPrefix(&cmd, "_FT");

  MRCommandGenerator cg = SearchCluster_MultiplexCommand(GetSearchCluster(), &cmd);
  struct MRCtx *mrctx = MR_CreateCtx(ctx, NULL);
  MR_SetCoordinationStrategy(mrctx, MRCluster_MastersOnly | MRCluster_FlatCoordination);
  MR_Map(mrctx, allOKReducer, cg);
  cg.Free(cg.ctx);
  return REDISMODULE_OK;
}

int FanoutCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  if (argc < 2) {
    return RedisModule_WrongArity(ctx);
  }

  RedisModule_AutoMemory(ctx);

  MRCommand cmd = MR_NewCommandFromRedisStrings(argc, argv);
  /* Replace our own FT command with _FT. command */
  MRCommand_SetPrefix(&cmd, "_FT");

  MRCommandGenerator cg = SearchCluster_MultiplexCommand(GetSearchCluster(), &cmd);
  MR_Map(MR_CreateCtx(ctx, NULL), allOKReducer, cg);
  cg.Free(cg.ctx);
  return REDISMODULE_OK;
}

int AggregateRequest_BuildDistributedPlan(AggregateRequest *req, RedisSearchCtx *sctx,
                                          RedisModuleString **argv, int argc, char **err);

struct distAggCtx {
  RedisModuleBlockedClient *bc;
  RedisModuleString **argv;
  int argc;
};

void _DistAggregateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc,
                           struct ConcurrentCmdCtx *ccx) {
  RedisSearchCtx sctx = {.redisCtx = ctx};
  AggregateRequest req_s = {NULL};
  char *err;
  if (AggregateRequest_BuildDistributedPlan(&req_s, &sctx, argv, argc, &err) != REDISMODULE_OK) {
    RedisModule_Log(ctx, "warning", "Error building dist plan: %s", err);
    RedisModule_ReplyWithError(ctx, err ? err : "Error building plan");
    ERR_FREE(err);
    goto done;
  }
  AggregateRequest_Run(&req_s, ctx);
done:
  AggregateRequest_Free(&req_s);
}

static int DIST_AGG_THREADPOOL = -1;

int DistAggregateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  if (argc < 3) {
    return RedisModule_WrongArity(ctx);
  }
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  return ConcurrentSearch_HandleRedisCommandEx(DIST_AGG_THREADPOOL, CMDCTX_NO_GIL,
                                               _DistAggregateCommand, ctx, argv, argc);
}

int TagValsCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  if (argc < 3) {
    return RedisModule_WrongArity(ctx);
  }
  // Check that the cluster state is valid
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  RedisModule_AutoMemory(ctx);

  MRCommand cmd = MR_NewCommandFromRedisStrings(argc, argv);
  /* Replace our own FT command with _FT. command */
  MRCommand_SetPrefix(&cmd, "_FT");

  MRCommandGenerator cg = SearchCluster_MultiplexCommand(GetSearchCluster(), &cmd);
  MR_Map(MR_CreateCtx(ctx, NULL), uniqueStringsReducer, cg);
  cg.Free(cg.ctx);
  return REDISMODULE_OK;
}

int BroadcastCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  if (argc < 2) {
    return RedisModule_WrongArity(ctx);
  }
  // Check that the cluster state is valid
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  RedisModule_AutoMemory(ctx);

  MRCommand cmd = MR_NewCommandFromRedisStrings(argc - 1, &argv[1]);
  struct MRCtx *mctx = MR_CreateCtx(ctx, NULL);
  MR_SetCoordinationStrategy(mctx, MRCluster_FlatCoordination);

  if (cmd.num > 1 && MRCommand_GetShardingKey(&cmd) >= 0) {
    MRCommandGenerator cg = SearchCluster_MultiplexCommand(GetSearchCluster(), &cmd);
    MR_Map(mctx, chainReplyReducer, cg);
    cg.Free(cg.ctx);
  } else {
    MR_Fanout(mctx, chainReplyReducer, cmd);
  }
  return REDISMODULE_OK;
}

int InfoCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 2) {
    // FT.INFO {index}
    return RedisModule_WrongArity(ctx);
  }
  // Check that the cluster state is valid
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  RedisModule_AutoMemory(ctx);
  MRCommand cmd = MR_NewCommandFromRedisStrings(argc, argv);
  MRCommand_SetPrefix(&cmd, "_FT");

  struct MRCtx *mctx = MR_CreateCtx(ctx, NULL);
  MRCommandGenerator cg = SearchCluster_MultiplexCommand(GetSearchCluster(), &cmd);
  MR_SetCoordinationStrategy(mctx, MRCluster_FlatCoordination);
  MR_Map(mctx, InfoReplyReducer, cg);
  cg.Free(cg.ctx);
  return REDISMODULE_OK;
}

int LocalSearchCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  // MR_UpdateTopology(ctx);
  if (argc < 3) {
    return RedisModule_WrongArity(ctx);
  }
  // Check that the cluster state is valid
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  RedisModule_AutoMemory(ctx);

  searchRequestCtx *req = rscParseRequest(argv, argc);
  if (!req) {
    return RedisModule_ReplyWithError(ctx, "Invalid search request");
  }

  MRCommand cmd = MR_NewCommandFromRedisStrings(argc, argv);
  if (!req->withScores) {
    MRCommand_AppendArgs(&cmd, 1, "WITHSCORES");
  }
  if (!req->withSortingKeys && req->withSortby) {
    MRCommand_AppendArgs(&cmd, 1, "WITHSORTKEYS");
  }

  // replace the LIMIT {offset} {limit} with LIMIT 0 {limit}, because we need all top N to merge
  int limitIndex = RMUtil_ArgExists("LIMIT", argv, argc, 3);
  if (limitIndex && req->limit > 0 && limitIndex < argc - 2) {
    MRCommand_ReplaceArg(&cmd, limitIndex + 1, "0");
  }

  /* Replace our own DFT command with FT. command */
  MRCommand_ReplaceArg(&cmd, 0, "_FT.SEARCH");
  MRCommandGenerator cg = SearchCluster_MultiplexCommand(GetSearchCluster(), &cmd);
  struct MRCtx *mrctx = MR_CreateCtx(ctx, req);
  // we prefer the next level to be local - we will only approach nodes on our own shard
  // we also ask only masters to serve the request, to avoid duplications by random
  MR_SetCoordinationStrategy(mrctx, MRCluster_LocalCoordination | MRCluster_MastersOnly);

  MR_Map(mrctx, searchResultReducer, cg);
  cg.Free(cg.ctx);
  return REDISMODULE_OK;
}

int FlatSearchCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  // MR_UpdateTopology(ctx);
  if (argc < 3) {
    return RedisModule_WrongArity(ctx);
  }
  // Check that the cluster state is valid
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  RedisModule_AutoMemory(ctx);

  searchRequestCtx *req = rscParseRequest(argv, argc);
  if (!req) {
    return RedisModule_ReplyWithError(ctx, "Invalid search request");
  }

  MRCommand cmd = MR_NewCommandFromRedisStrings(argc, argv);
  if (!req->withScores) {
    MRCommand_AppendArgs(&cmd, 1, "WITHSCORES");
  }

  if (!req->withSortingKeys && req->withSortby) {
    MRCommand_AppendArgs(&cmd, 1, "WITHSORTKEYS");
    // req->withSortingKeys = 1;
  }

  // replace the LIMIT {offset} {limit} with LIMIT 0 {limit}, because we need all top N to merge
  int limitIndex = RMUtil_ArgExists("LIMIT", argv, argc, 3);
  if (limitIndex && req->limit > 0 && limitIndex < argc - 2) {
    MRCommand_ReplaceArg(&cmd, limitIndex + 1, "0");
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", req->limit + req->offset);
    MRCommand_ReplaceArg(&cmd, limitIndex + 2, buf);
  }

  // Tag the InKeys arguments
  int inKeysPos = RMUtil_ArgIndex("INKEYS", argv, argc);

  if (inKeysPos > 2) {
    long long numFilteredIds = 0;
    // Get the number of INKEYS args
    RMUtil_ParseArgsAfter("INKEYS", &argv[inKeysPos], argc - inKeysPos, "l", &numFilteredIds);
    // If we won't overflow - tag each key
    if (numFilteredIds > 0 && numFilteredIds + inKeysPos + 1 < argc) {
      inKeysPos += 2;  // the start of the actual keys
      for (int x = inKeysPos; x < inKeysPos + numFilteredIds && x < argc; x++) {
        SearchCluster_RewriteCommandArg(GetSearchCluster(), &cmd, x, x);
      }
    }
  }
  // MRCommand_Print(&cmd);

  /* Replace our own FT command with _FT. command */
  MRCommand_ReplaceArg(&cmd, 0, "_FT.SEARCH");
  MRCommandGenerator cg = SearchCluster_MultiplexCommand(GetSearchCluster(), &cmd);
  struct MRCtx *mrctx = MR_CreateCtx(ctx, req);
  // we prefer the next level to be local - we will only approach nodes on our own shard
  // we also ask only masters to serve the request, to avoid duplications by random
  MR_SetCoordinationStrategy(mrctx, MRCluster_FlatCoordination);

  MR_Map(mrctx, searchResultReducer, cg);
  cg.Free(cg.ctx);
  return REDISMODULE_OK;
}

int SearchCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  if (argc < 3) {
    return RedisModule_WrongArity(ctx);
  }
  // Check that the cluster state is valid
  if (!SearchCluster_Ready(GetSearchCluster())) {
    return RedisModule_ReplyWithError(ctx, CLUSTERDOWN_ERR);
  }
  RedisModule_AutoMemory(ctx);
  // MR_UpdateTopology(ctx);

  // If this a one-node cluster, we revert to a simple, flat one level coordination
  // if (MR_NumHosts() < 2) {
  //   return LocalSearchCommandHandler(ctx, argv, argc);
  // }

  MRCommand cmd = MR_NewCommandFromRedisStrings(argc, argv);
  MRCommand_ReplaceArg(&cmd, 0, "FT.LSEARCH");

  searchRequestCtx *req = rscParseRequest(argv, argc);
  if (!req) {
    return RedisModule_ReplyWithError(ctx, "Invalid search request");
  }
  // Internally we must have WITHSCORES set, even if the usr didn't set it
  if (!req->withScores) {
    MRCommand_AppendArgs(&cmd, 1, "WITHSCORES");
  }
  // MRCommand_Print(&cmd);

  struct MRCtx *mrctx = MR_CreateCtx(ctx, req);
  MR_SetCoordinationStrategy(mrctx, MRCluster_RemoteCoordination | MRCluster_MastersOnly);
  MR_Fanout(mrctx, searchResultReducer, cmd);

  return REDIS_OK;
}

int ClusterInfoCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  RedisModule_AutoMemory(ctx);

  int n = 0;
  RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

  RedisModule_ReplyWithSimpleString(ctx, "num_partitions");
  n++;
  RedisModule_ReplyWithLongLong(ctx, GetSearchCluster()->size);
  n++;
  RedisModule_ReplyWithSimpleString(ctx, "cluster_type");
  n++;
  RedisModule_ReplyWithSimpleString(
      ctx, clusterConfig.type == ClusterType_RedisLabs ? "redislabs" : "redis_oss");
  n++;

  // Report hash func
  MRClusterTopology *topo = MR_GetCurrentTopology();
  RedisModule_ReplyWithSimpleString(ctx, "hash_func");
  n++;
  if (topo) {
    RedisModule_ReplyWithSimpleString(
        ctx, topo->hashFunc == MRHashFunc_CRC12
                 ? MRHASHFUNC_CRC12_STR
                 : (topo->hashFunc == MRHashFunc_CRC16 ? MRHASHFUNC_CRC16_STR : "n/a"));
  } else {
    RedisModule_ReplyWithSimpleString(ctx, "n/a");
  }
  n++;

  // Report topology
  RedisModule_ReplyWithSimpleString(ctx, "num_slots");
  n++;
  RedisModule_ReplyWithLongLong(ctx, topo ? (long long)topo->numSlots : 0);
  n++;

  RedisModule_ReplyWithSimpleString(ctx, "slots");
  n++;

  if (!topo) {
    RedisModule_ReplyWithNull(ctx);
    n++;

  } else {

    for (int i = 0; i < topo->numShards; i++) {
      MRClusterShard *sh = &topo->shards[i];
      RedisModule_ReplyWithArray(ctx, 2 + sh->numNodes);
      n++;
      RedisModule_ReplyWithLongLong(ctx, sh->startSlot);
      RedisModule_ReplyWithLongLong(ctx, sh->endSlot);
      for (int j = 0; j < sh->numNodes; j++) {
        MRClusterNode *node = &sh->nodes[j];
        RedisModule_ReplyWithArray(ctx, 4);
        RedisModule_ReplyWithSimpleString(ctx, node->id);
        RedisModule_ReplyWithSimpleString(ctx, node->endpoint.host);
        RedisModule_ReplyWithLongLong(ctx, node->endpoint.port);
        RedisModule_ReplyWithString(
            ctx, RedisModule_CreateStringPrintf(ctx, "%s%s",
                                                node->flags & MRNode_Master ? "master " : "slave ",
                                                node->flags & MRNode_Self ? "self" : ""));
      }
    }
  }

  RedisModule_ReplySetArrayLength(ctx, n);
  return REDISMODULE_OK;
}

// A special command for redis cluster OSS, that refreshes the cluster state
int RefreshClusterCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  RedisModule_AutoMemory(ctx);
  MRClusterTopology *topo = RedisCluster_GetTopology(ctx);

  SearchCluster_EnsureSize(ctx, GetSearchCluster(), topo);

  MR_UpdateTopology(topo);
  RedisModule_ReplyWithSimpleString(ctx, "OK");

  return REDISMODULE_OK;
}

int SetClusterCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  RedisModule_AutoMemory(ctx);
  MRClusterTopology *topo = RedisEnterprise_ParseTopology(ctx, argv, argc);
  // this means a parsing error, the parser already sent the explicit error to the client
  if (!topo) {
    return REDISMODULE_ERR;
  }

  SearchCluster_EnsureSize(ctx, GetSearchCluster(), topo);
  // If the cluster hash func or cluster slots has changed, set the new value
  switch (topo->hashFunc) {
    case MRHashFunc_CRC12:
      PartitionCtx_SetSlotTable(&GetSearchCluster()->part, crc12_slot_table,
                                MIN(4096, topo->numSlots));
      break;
    case MRHashFunc_CRC16:
      PartitionCtx_SetSlotTable(&GetSearchCluster()->part, crc16_slot_table,
                                MIN(16384, topo->numSlots));
      break;
    case MRHashFunc_None:
    default:
      // do nothing
      break;
  }

  // send the topology to the cluster
  if (MR_UpdateTopology(topo) != REDISMODULE_OK) {
    // failed update
    MRClusterTopology_Free(topo);
    return RedisModule_ReplyWithError(ctx, "Error updating the topology");
  }

  RedisModule_ReplyWithSimpleString(ctx, "OK");

  return REDISMODULE_OK;
}

/* Perform basic configurations and init all threads and global structures */
int initSearchCluster(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  /* Init cluster configs */
  if (argc > 0) {
    if (ParseConfig(&clusterConfig, ctx, argv, argc) == REDISMODULE_ERR) {
      // printf("Could not parse module config\n");
      RedisModule_Log(ctx, "warning", "Could not parse module config");
      return REDISMODULE_ERR;
    }
  } else {
    RedisModule_Log(ctx, "warning", "No module config, reverting to default settings");
    clusterConfig = DEFAULT_CLUSTER_CONFIG;
  }

  RedisModule_Log(ctx, "notice",
                  "Cluster configuration: %d partitions, type: %d, coordinator timeout: %dms",
                  clusterConfig.numPartitions, clusterConfig.type, clusterConfig.timeoutMS);

  /* Configure cluster injections */
  ShardFunc sf;

  MRClusterTopology *initialTopology = NULL;

  const char **slotTable = NULL;
  size_t tableSize = 0;

  switch (clusterConfig.type) {
    case ClusterType_RedisLabs:
      sf = CRC12ShardFunc;
      slotTable = crc12_slot_table;
      tableSize = 4096;

      break;
    case ClusterType_RedisOSS:
    default:
      // init the redis topology updater loop
      if (InitRedisTopologyUpdater() == REDIS_ERR) {
        RedisModule_Log(ctx, "warning", "Could not init redis cluster topology updater. Aborting");
        return REDISMODULE_ERR;
      }
      sf = CRC16ShardFunc;
      slotTable = crc16_slot_table;
      tableSize = 16384;
  }

  MRCluster *cl = MR_NewCluster(initialTopology, sf, 2);
  MR_Init(cl, clusterConfig.timeoutMS);
  InitGlobalSearchCluster(clusterConfig.numPartitions, slotTable, tableSize);

  return REDISMODULE_OK;
}

/** A dummy command handler, for commands that are disabled when running the module in OSS
 * clusters
 * when it is not an internal OSS build. */
int DisabledCommandHandler(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return RedisModule_ReplyWithError(ctx, "Module Disabled in Open Source Redis");
}

/** A wrapper function that safely checks whether we are running in OSS cluster when registering
 * commands.
 * If we are, and the module was not compiled for oss clusters, this wrapper will return a pointer
 * to a dummy function disabling the actual handler.
 *
 * If we are running in RLEC or in a special OSS build - we simply return the original command.
 *
 * All coordinator handlers must be wrapped in this decorator.
 */
static RedisModuleCmdFunc SafeCmd(RedisModuleCmdFunc f) {
  if (RSBuildType_g == RSBuildType_Enterprise && clusterConfig.type != ClusterType_RedisLabs) {
    /* If we are running inside OSS cluster and not built for oss, we return the dummy handler */
    return DisabledCommandHandler;
  }

  /* Valid - we return the original function */
  return f;
}

/**
 * Because our indexes are in the form of IDX{something}, a single real index might
 * appear as multiple indexes, using more memory and essentially disabling rate
 * limiting.
 *
 * This works as a hook after every new index is created, to strip the '{' from the
 * cursor name, and use the real name as the entry.
 */
static void addIndexCursor(const IndexSpec *sp) {
  char *s = strdup(sp->name);
  char *end = strchr(s, '{');
  if (end) {
    *end = '\0';
    CursorList_AddSpec(&RSCursors, s, RSCURSORS_DEFAULT_CAPACITY);
  }
  free(s);
}

#define RM_TRY(expr)                                                  \
  if (expr == REDISMODULE_ERR) {                                      \
    RedisModule_Log(ctx, "warning", "Could not run " __STRING(expr)); \
    return REDISMODULE_ERR;                                           \
  }

int __attribute__((visibility("default")))
RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  IndexSpec_OnCreate = addIndexCursor;
  /**

  FT.AGGREGATE gh * LOAD 1 @type GROUPBY 1 @type REDUCE COUNT 0 AS num REDUCE SUM 1 @date SORTBY 2
  @num DESC MAX 10

   */

  printf("RSValue size: %lu\n", sizeof(RSValue));

  if (RedisModule_Init(ctx, "ft", RSCOORDINATOR_VERSION, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  // Init RediSearch internal search
  if (RediSearch_InitModuleInternal(ctx, argv, argc) == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning", "Could not init search library...");
    return REDISMODULE_ERR;
  }

  // Init the configuration and global cluster structs
  if (initSearchCluster(ctx, argv, argc) == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning", "Could not init MR search cluster");
    return REDISMODULE_ERR;
  }

  // Init the aggregation thread pool
  DIST_AGG_THREADPOOL = ConcurrentSearch_CreatePool(CONCURRENT_SEARCH_POOL_SIZE);

  /*********************************************************
   * Single-shard simple commands
   **********************************************************/
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.ADD", SafeCmd(SingleShardCommandHandler), "readonly", 0,
                                   0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.DEL", SafeCmd(SingleShardCommandHandler), "readonly", 0,
                                   0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.GET", SafeCmd(SingleShardCommandHandler), "readonly", 0,
                                   0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.TAGVALS", SafeCmd(TagValsCommandHandler), "readonly", 0,
                                   0, -1));
  RM_TRY(
      RedisModule_CreateCommand(ctx, "FT.MGET", SafeCmd(MGetCommandHandler), "readonly", 0, 0, -1));

  RM_TRY(RedisModule_CreateCommand(ctx, "FT.ADDHASH", SafeCmd(SingleShardCommandHandler),
                                   "readonly", 0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.EXPLAIN", SafeCmd(SingleShardCommandHandler),
                                   "readonly", 0, 0, -1));

  RM_TRY(RedisModule_CreateCommand(ctx, "FT.SUGADD", SafeCmd(SingleShardCommandHandler), "readonly",
                                   0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.SUGGET", SafeCmd(SingleShardCommandHandler), "readonly",
                                   0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.SUGDEL", SafeCmd(SingleShardCommandHandler), "readonly",
                                   0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.SUGLEN", SafeCmd(SingleShardCommandHandler), "readonly",
                                   0, 0, -1));

  /*********************************************************
   * Multi shard, fanout commands
   **********************************************************/
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.AGGREGATE", SafeCmd(DistAggregateCommand), "readonly",
                                   0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.CREATE", SafeCmd(MastersFanoutCommandHandler),
                                   "readonly", 0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.ALTER", SafeCmd(MastersFanoutCommandHandler),
                                   "readonly", 0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.DROP", SafeCmd(MastersFanoutCommandHandler), "readonly",
                                   0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.BROADCAST", SafeCmd(BroadcastCommand), "readonly", 0, 0,
                                   -1));
  RM_TRY(
      RedisModule_CreateCommand(ctx, "FT.INFO", SafeCmd(InfoCommandHandler), "readonly", 0, 0, -1));

  /*********************************************************
   * Complex coordination search commands
   **********************************************************/
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.LSEARCH", SafeCmd(LocalSearchCommandHandler),
                                   "readonly", 0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.FSEARCH", SafeCmd(FlatSearchCommandHandler), "readonly",
                                   0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.SEARCH", SafeCmd(FlatSearchCommandHandler), "readonly",
                                   0, 0, -1));

  /*********************************************************
   * RS Cluster specific commands
   **********************************************************/
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.CLUSTERSET", SafeCmd(SetClusterCommand), "readonly", 0,
                                   0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.CLUSTERREFRESH", SafeCmd(RefreshClusterCommand),
                                   "readonly", 0, 0, -1));
  RM_TRY(RedisModule_CreateCommand(ctx, "FT.CLUSTERINFO", SafeCmd(ClusterInfoCommand), "readonly",
                                   0, 0, -1));

  return REDISMODULE_OK;
}
