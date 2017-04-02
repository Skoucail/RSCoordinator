#include "command.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

struct mrCommandConf {
  const char *command;
  MRCommandFlags flags;
  int keyPos;
};

struct mrCommandConf __commandConfig[] = {

    // document commands
    {"FT.SEARCH", MRCommand_Read | MRCommand_SingleKey, 1},
    {"FT.DEL", MRCommand_Write | MRCommand_SingleKey, 1},
    {"FT.ADD", MRCommand_Write | MRCommand_SingleKey, 1},
    {"FT.ADDHASH", MRCommand_Write | MRCommand_SingleKey, 1},

    // index commands
    {"FT.CREATE", MRCommand_Write | MRCommand_SingleKey, 1},
    {"FT.DROP", MRCommand_Write | MRCommand_SingleKey, 1},
    {"FT.OPTIMIZE", MRCommand_Write | MRCommand_SingleKey, 1},
    {"FT.INFO", MRCommand_Read | MRCommand_SingleKey, 1},

    // Suggest commands
    {"FT.SUGADD", MRCommand_Write | MRCommand_SingleKey, 1},
    {"FT.SUGGET", MRCommand_Read | MRCommand_SingleKey, 1},
    {"FT.SUGLEN", MRCommand_Read | MRCommand_SingleKey, 1},
    {"FT.SUGDEL", MRCommand_Write | MRCommand_SingleKey, 1},

    // Coordination commands - they are all read commands since they can be triggered from slaves
    {"DFT.ADD", MRCommand_Read | MRCommand_Coordination, -1},
    {"DFT.SEARCH", MRCommand_Read | MRCommand_Coordination, -1},
    {"DFT.XSEARCH", MRCommand_Read | MRCommand_Coordination, -1},
    {"DFT.CREATE", MRCommand_Read | MRCommand_Coordination, -1},
    {"DFT.CLUSTERINFO", MRCommand_Read | MRCommand_Coordination, -1},
    {"DFT.INFO", MRCommand_Read | MRCommand_Coordination, -1},
    {"DFT.ADDHASH", MRCommand_Read | MRCommand_Coordination, -1},
    {"DFT.DEL", MRCommand_Read | MRCommand_Coordination, -1},
    {"DFT.DROP", MRCommand_Read | MRCommand_Coordination, -1},
    {"DFT.CREATE", MRCommand_Read | MRCommand_Coordination, -1},

    // sentinel
    {NULL},
};

int _getCommandConfId(MRCommand *cmd) {
  cmd->id = -1;
  if (cmd->num == 0) {
    return 0;
  }

  for (int i = 0; __commandConfig[i].command != NULL; i++) {
    if (!strcasecmp(cmd->args[0], __commandConfig[i].command)) {
      printf("conf id for cmd %s: %d\n", cmd->args[0], i);
      cmd->id = i;
      return 1;
    }
  }
  return 0;
}

void MRCommand_Free(MRCommand *cmd) {
  for (int i = 0; i < cmd->num; i++) {
    free(cmd->args[i]);
  }

  free(cmd->args);
}

MRCommand MR_NewCommandArgv(int argc, char **argv) {
  MRCommand cmd = (MRCommand){.num = argc, .args = calloc(argc, sizeof(char **))};

  for (int i = 0; i < argc; i++) {

    cmd.args[i] = strdup(argv[i]);
  }
  _getCommandConfId(&cmd);
  return cmd;
}

/* Create a deep copy of a command by duplicating all strings */
MRCommand MRCommand_Copy(MRCommand *cmd) {
  MRCommand ret = *cmd;

  ret.args = calloc(cmd->num, sizeof(char *));
  for (int i = 0; i < cmd->num; i++) {
    ret.args[i] = strdup(cmd->args[i]);
  }
  return ret;
}

MRCommand MR_NewCommand(int argc, ...) {
  MRCommand cmd = (MRCommand){
      .num = argc, .args = calloc(argc, sizeof(char *)),
  };

  va_list ap;
  va_start(ap, argc);
  for (int i = 0; i < argc; i++) {
    cmd.args[i] = strdup(va_arg(ap, const char *));
  }
  va_end(ap);
  _getCommandConfId(&cmd);
  return cmd;
}

MRCommand MR_NewCommandFromRedisStrings(int argc, RedisModuleString **argv) {
  MRCommand cmd = (MRCommand){
      .num = argc, .args = calloc(argc, sizeof(char *)),
  };
  for (int i = 0; i < argc; i++) {
    cmd.args[i] = strdup(RedisModule_StringPtrLen(argv[i], NULL));
  }
  _getCommandConfId(&cmd);
  return cmd;
}

void MRCommand_AppendArgs(MRCommand *cmd, int num, ...) {
  if (num <= 0) return;
  int oldNum = cmd->num;
  cmd->num += num;

  cmd->args = realloc(cmd->args, cmd->num * sizeof(*cmd->args));
  va_list(ap);
  va_start(ap, num);
  for (int i = oldNum; i < cmd->num; i++) {
    cmd->args[i] = strdup(va_arg(ap, const char *));
  }
  va_end(ap);
}

void MRCommand_ReplaceArg(MRCommand *cmd, int index, const char *newArg) {
  if (index < 0 || index >= cmd->num) {
    return;
  }
  char *tmp = cmd->args[index];
  cmd->args[index] = strdup(newArg);
  free(tmp);

  // if we've replaced the first argument, we need to reconfigure the command
  if (index == 0) {
    _getCommandConfId(cmd);
  }
}

int MRCommand_GetShardingKey(MRCommand *cmd) {
  if (cmd->id < 0) {
    return 1;  // default
  }

  return __commandConfig[cmd->id].keyPos;
}

/* Return 1 if the command should not be sharded (i.e a coordination command or system command) */
int MRCommand_IsUnsharded(MRCommand *cmd) {
  if (cmd->id < 0) {
    return 0;  // default
  }
  return __commandConfig[cmd->id].keyPos <= 0;
}

void MRCommand_Print(MRCommand *cmd) {

  for (int i = 0; i < cmd->num; i++) {
    printf("%s ", cmd->args[i]);
  }
  printf("\n");
}
