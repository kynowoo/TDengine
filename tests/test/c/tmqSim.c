/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "taos.h"
#include "taoserror.h"
#include "tlog.h"

#define GREEN     "\033[1;32m"
#define NC        "\033[0m"
#define min(a, b) (((a) < (b)) ? (a) : (b))

#define MAX_SQL_STR_LEN         (1024 * 1024)
#define MAX_ROW_STR_LEN         (16 * 1024)
#define MAX_CONSUMER_THREAD_CNT (16)

typedef struct {
  TdThread thread;
  int32_t  consumerId;

  int32_t ifCheckData;
  int64_t expectMsgCnt;

  int64_t consumeMsgCnt;
  int32_t checkresult;

  char topicString[1024];
  char keyString[1024];

  int32_t numOfTopic;
  char    topics[32][64];

  int32_t numOfKey;
  char    key[32][64];
  char    value[32][64];

  tmq_t*      tmq;
  tmq_list_t* topicList;

} SThreadInfo;

typedef struct {
  // input from argvs
  char        dbName[32];
  int32_t     showMsgFlag;
  int32_t     consumeDelay;  // unit s
  int32_t     numOfThread;
  SThreadInfo stThreads[MAX_CONSUMER_THREAD_CNT];
} SConfInfo;

static SConfInfo g_stConfInfo;
TdFilePtr        g_fp = NULL;

// char* g_pRowValue = NULL;
// TdFilePtr g_fp = NULL;

static void printHelp() {
  char indent[10] = "        ";
  printf("Used to test the tmq feature with sim cases\n");

  printf("%s%s\n", indent, "-c");
  printf("%s%s%s%s\n", indent, indent, "Configuration directory, default is ", configDir);
  printf("%s%s\n", indent, "-d");
  printf("%s%s%s\n", indent, indent, "The name of the database for cosumer, no default ");
  printf("%s%s\n", indent, "-g");
  printf("%s%s%s%d\n", indent, indent, "showMsgFlag, default is ", g_stConfInfo.showMsgFlag);
  printf("%s%s\n", indent, "-y");
  printf("%s%s%s%d\n", indent, indent, "consume delay, default is s", g_stConfInfo.consumeDelay);
  exit(EXIT_SUCCESS);
}

void initLogFile() {
  // FILE *fp = fopen(g_stConfInfo.resultFileName, "a");
  TdFilePtr pFile = taosOpenFile("./tmqlog.txt", TD_FILE_CREATE | TD_FILE_WRITE | TD_FILE_APPEND | TD_FILE_STREAM);
  if (NULL == pFile) {
    fprintf(stderr, "Failed to open %s for save result\n", "./tmqlog.txt");
    exit - 1;
  };
  g_fp = pFile;

  time_t    tTime = taosGetTimestampSec();
  struct tm tm = *taosLocalTime(&tTime, NULL);

  taosFprintfFile(pFile, "###################################################################\n");
  taosFprintfFile(pFile, "# configDir:           %s\n", configDir);
  taosFprintfFile(pFile, "# dbName:              %s\n", g_stConfInfo.dbName);
  taosFprintfFile(pFile, "# showMsgFlag:         %d\n", g_stConfInfo.showMsgFlag);
  taosFprintfFile(pFile, "# consumeDelay:        %d\n", g_stConfInfo.consumeDelay);

  for (int32_t i = 0; i < g_stConfInfo.numOfThread; i++) {
    taosFprintfFile(pFile, "# consumer %d info:\n", g_stConfInfo.stThreads[i].consumerId);
    taosFprintfFile(pFile, "  Topics: ");
    for (int i = 0; i < g_stConfInfo.stThreads[i].numOfTopic; i++) {
      taosFprintfFile(pFile, "%s, ", g_stConfInfo.stThreads[i].topics[i]);
    }
    taosFprintfFile(pFile, "\n");
    taosFprintfFile(pFile, "  Key: ");
    for (int i = 0; i < g_stConfInfo.stThreads[i].numOfKey; i++) {
      taosFprintfFile(pFile, "%s:%s, ", g_stConfInfo.stThreads[i].key[i], g_stConfInfo.stThreads[i].value[i]);
    }
    taosFprintfFile(pFile, "\n");
  }

  taosFprintfFile(pFile, "# Test time:                %d-%02d-%02d %02d:%02d:%02d\n", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  taosFprintfFile(pFile, "###################################################################\n");
}

void parseArgument(int32_t argc, char* argv[]) {
  memset(&g_stConfInfo, 0, sizeof(SConfInfo));
  g_stConfInfo.showMsgFlag = 0;
  g_stConfInfo.consumeDelay = 5;

  for (int32_t i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      printHelp();
      exit(0);
    } else if (strcmp(argv[i], "-d") == 0) {
      strcpy(g_stConfInfo.dbName, argv[++i]);
    } else if (strcmp(argv[i], "-c") == 0) {
      strcpy(configDir, argv[++i]);
    } else if (strcmp(argv[i], "-g") == 0) {
      g_stConfInfo.showMsgFlag = atol(argv[++i]);
    } else if (strcmp(argv[i], "-y") == 0) {
      g_stConfInfo.consumeDelay = atol(argv[++i]);
    } else {
      printf("%s unknow para: %s %s", GREEN, argv[++i], NC);
      exit(-1);
    }
  }

#if 1
  pPrint("%s configDir:%s %s", GREEN, configDir, NC);
  pPrint("%s dbName:%s %s", GREEN, g_stConfInfo.dbName, NC);
  pPrint("%s consumeDelay:%d %s", GREEN, g_stConfInfo.consumeDelay, NC);
  pPrint("%s showMsgFlag:%d %s", GREEN, g_stConfInfo.showMsgFlag, NC);
#endif
}

void splitStr(char** arr, char* str, const char* del) {
  char* s = strtok(str, del);
  while (s != NULL) {
    *arr++ = s;
    s = strtok(NULL, del);
  }
}

void ltrim(char* str) {
  if (str == NULL || *str == '\0') {
    return;
  }
  int   len = 0;
  char* p = str;
  while (*p != '\0' && isspace(*p)) {
    ++p;
    ++len;
  }
  memmove(str, p, strlen(str) - len + 1);
  // return str;
}

static int  running = 1;
static void msg_process(TAOS_RES* msg, int32_t msgIndex, int32_t threadLable) {
  char buf[1024];

  // printf("topic: %s\n", tmq_get_topic_name(msg));
  // printf("vg:%d\n", tmq_get_vgroup_id(msg));
  taosFprintfFile(g_fp, "msg index:%d, threadLable: %d\n", msgIndex, threadLable);
  taosFprintfFile(g_fp, "topic: %s, vgroupId: %d\n", tmq_get_topic_name(msg), tmq_get_vgroup_id(msg));

  while (1) {
    TAOS_ROW row = taos_fetch_row(msg);
    if (row == NULL) break;
    TAOS_FIELD* fields = taos_fetch_fields(msg);
    int32_t     numOfFields = taos_field_count(msg);
    // taos_print_row(buf, row, fields, numOfFields);
    // printf("%s\n", buf);
    // taosFprintfFile(g_fp, "%s\n",  buf);
  }
}

int queryDB(TAOS* taos, char* command) {
  TAOS_RES* pRes = taos_query(taos, command);
  int       code = taos_errno(pRes);
  // if ((code != 0) && (code != TSDB_CODE_RPC_AUTH_REQUIRED)) {
  if (code != 0) {
    pError("failed to reason:%s, sql: %s", tstrerror(code), command);
    taos_free_result(pRes);
    return -1;
  }
  taos_free_result(pRes);
  return 0;
}

void build_consumer(SThreadInfo* pInfo) {
  char sqlStr[1024] = {0};

  TAOS* pConn = taos_connect(NULL, "root", "taosdata", NULL, 0);
  assert(pConn != NULL);

  sprintf(sqlStr, "use %s", g_stConfInfo.dbName);
  TAOS_RES* pRes = taos_query(pConn, sqlStr);
  if (taos_errno(pRes) != 0) {
    printf("error in use db, reason:%s\n", taos_errstr(pRes));
    taos_free_result(pRes);
    exit(-1);
  }
  taos_free_result(pRes);

  tmq_conf_t* conf = tmq_conf_new();
  // tmq_conf_set(conf, "group.id", "tg2");
  for (int32_t i = 0; i < pInfo->numOfKey; i++) {
    tmq_conf_set(conf, pInfo->key[i], pInfo->value[i]);
  }
  pInfo->tmq = tmq_consumer_new(conf, NULL, 0);
  return;
}

void build_topic_list(SThreadInfo* pInfo) {
  pInfo->topicList = tmq_list_new();
  // tmq_list_append(topic_list, "test_stb_topic_1");
  for (int32_t i = 0; i < pInfo->numOfTopic; i++) {
    tmq_list_append(pInfo->topicList, pInfo->topics[i]);
  }
  return;
}

int32_t saveConsumeResult(SThreadInfo* pInfo) {
  char sqlStr[1024] = {0};

  TAOS* pConn = taos_connect(NULL, "root", "taosdata", NULL, 0);
  assert(pConn != NULL);

  // schema: ts timestamp, consumerid int, consummsgcnt bigint, checkresult int
  sprintf(sqlStr, "insert into %s.consumeresult values (now, %d, %" PRId64 ", %d)", g_stConfInfo.dbName,
          pInfo->consumerId, pInfo->consumeMsgCnt, pInfo->checkresult);

  TAOS_RES* pRes = taos_query(pConn, sqlStr);
  if (taos_errno(pRes) != 0) {
    printf("error in save consumeinfo, reason:%s\n", taos_errstr(pRes));
    taos_free_result(pRes);
    exit(-1);
  }

  taos_free_result(pRes);

  return 0;
}

void loop_consume(SThreadInfo* pInfo) {
  tmq_resp_err_t err;

  int64_t totalMsgs = 0;
  // int64_t totalRows = 0;

  while (running) {
    TAOS_RES* tmqMsg = tmq_consumer_poll(pInfo->tmq, g_stConfInfo.consumeDelay * 1000);
    if (tmqMsg) {
      if (0 != g_stConfInfo.showMsgFlag) {
        msg_process(tmqMsg, totalMsgs, 0);
      }

      taos_free_result(tmqMsg);

      totalMsgs++;

      if (totalMsgs >= pInfo->expectMsgCnt) {
        break;
      }
    } else {
      break;
    }
  }

  err = tmq_consumer_close(pInfo->tmq);
  if (err) {
    printf("tmq_consumer_close() fail, reason: %s\n", tmq_err2str(err));
    exit(-1);
  }

  pInfo->consumeMsgCnt = totalMsgs;
}

void* consumeThreadFunc(void* param) {
  int32_t totalMsgs = 0;

  SThreadInfo* pInfo = (SThreadInfo*)param;

  build_consumer(pInfo);
  build_topic_list(pInfo);
  if ((NULL == pInfo->tmq) || (NULL == pInfo->topicList)) {
    return NULL;
  }

  tmq_resp_err_t err = tmq_subscribe(pInfo->tmq, pInfo->topicList);
  if (err) {
    printf("tmq_subscribe() fail, reason: %s\n", tmq_err2str(err));
    exit(-1);
  }

  loop_consume(pInfo);

  err = tmq_unsubscribe(pInfo->tmq);
  if (err) {
    printf("tmq_unsubscribe() fail, reason: %s\n", tmq_err2str(err));
    pInfo->consumeMsgCnt = -1;
    return NULL;
  }

  // save consume result into consumeresult table
  saveConsumeResult(pInfo);

  return NULL;
}

void parseConsumeInfo() {
  char*      token;
  const char delim[2] = ",";
  const char ch = ':';

  for (int32_t i = 0; i < g_stConfInfo.numOfThread; i++) {
    token = strtok(g_stConfInfo.stThreads[i].topicString, delim);
    while (token != NULL) {
      // printf("%s\n", token );
      strcpy(g_stConfInfo.stThreads[i].topics[g_stConfInfo.stThreads[i].numOfTopic], token);
      ltrim(g_stConfInfo.stThreads[i].topics[g_stConfInfo.stThreads[i].numOfTopic]);
      // printf("%s\n", g_stConfInfo.topics[g_stConfInfo.numOfTopic]);
      g_stConfInfo.stThreads[i].numOfTopic++;

      token = strtok(NULL, delim);
    }

    token = strtok(g_stConfInfo.stThreads[i].keyString, delim);
    while (token != NULL) {
      // printf("%s\n", token );
      {
        char* pstr = token;
        ltrim(pstr);
        char* ret = strchr(pstr, ch);
        memcpy(g_stConfInfo.stThreads[i].key[g_stConfInfo.stThreads[i].numOfKey], pstr, ret - pstr);
        strcpy(g_stConfInfo.stThreads[i].value[g_stConfInfo.stThreads[i].numOfKey], ret + 1);
        // printf("key: %s, value: %s\n", g_stConfInfo.key[g_stConfInfo.numOfKey],
        // g_stConfInfo.value[g_stConfInfo.numOfKey]);
        g_stConfInfo.stThreads[i].numOfKey++;
      }

      token = strtok(NULL, delim);
    }
  }
}

int32_t getConsumeInfo() {
  char sqlStr[1024] = {0};

  TAOS* pConn = taos_connect(NULL, "root", "taosdata", NULL, 0);
  assert(pConn != NULL);

  sprintf(sqlStr, "select * from %s.consumeinfo", g_stConfInfo.dbName);
  TAOS_RES* pRes = taos_query(pConn, sqlStr);
  if (taos_errno(pRes) != 0) {
    printf("error in get consumeinfo, reason:%s\n", taos_errstr(pRes));
    taos_free_result(pRes);
    exit(-1);
  }

  TAOS_ROW    row = NULL;
  int         num_fields = taos_num_fields(pRes);
  TAOS_FIELD* fields = taos_fetch_fields(pRes);

  // schema: ts timestamp, consumerid int, topiclist binary(1024), keylist binary(1024), expectmsgcnt bigint,
  // ifcheckdata int

  int32_t numOfThread = 0;
  while ((row = taos_fetch_row(pRes))) {
    int32_t* lengths = taos_fetch_lengths(pRes);

    for (int i = 0; i < num_fields; ++i) {
      if (row[i] == NULL || 0 == i) {
        continue;
      }

      if ((1 == i) && (fields[i].type == TSDB_DATA_TYPE_INT)) {
        g_stConfInfo.stThreads[numOfThread].consumerId = *((int32_t*)row[i]);
      } else if ((2 == i) && (fields[i].type == TSDB_DATA_TYPE_BINARY)) {
        memcpy(g_stConfInfo.stThreads[numOfThread].topicString, row[i], lengths[i]);
      } else if ((3 == i) && (fields[i].type == TSDB_DATA_TYPE_BINARY)) {
        memcpy(g_stConfInfo.stThreads[numOfThread].keyString, row[i], lengths[i]);
      } else if ((4 == i) && (fields[i].type == TSDB_DATA_TYPE_BIGINT)) {
        g_stConfInfo.stThreads[numOfThread].expectMsgCnt = *((int64_t*)row[i]);
      } else if ((5 == i) && (fields[i].type == TSDB_DATA_TYPE_INT)) {
        g_stConfInfo.stThreads[numOfThread].ifCheckData = *((int32_t*)row[i]);
      }
    }
    numOfThread++;
  }
  g_stConfInfo.numOfThread = numOfThread;

  taos_free_result(pRes);

  parseConsumeInfo();

  return 0;
}

int main(int32_t argc, char* argv[]) {
  parseArgument(argc, argv);
  getConsumeInfo();
  initLogFile();

  TdThreadAttr thattr;
  taosThreadAttrInit(&thattr);
  taosThreadAttrSetDetachState(&thattr, PTHREAD_CREATE_JOINABLE);

  // pthread_create one thread to consume
  for (int32_t i = 0; i < g_stConfInfo.numOfThread; ++i) {
    taosThreadCreate(&(g_stConfInfo.stThreads[i].thread), &thattr, consumeThreadFunc,
                     (void*)(&(g_stConfInfo.stThreads[i])));
  }

  for (int32_t i = 0; i < g_stConfInfo.numOfThread; i++) {
    taosThreadJoin(g_stConfInfo.stThreads[i].thread, NULL);
  }

  // printf("consumer: %d, cosumer1: %d\n", totalMsgs, pInfo->consumeMsgCnt);

  taosFprintfFile(g_fp, "\n");
  taosCloseFile(&g_fp);

  return 0;
}

