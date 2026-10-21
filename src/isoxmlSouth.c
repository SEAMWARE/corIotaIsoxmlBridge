//
// FILE            isoxmlSouth.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
// Task data in and out - see header. What iotagent-isoxml answers (lib/bindings/HTTPBindings.js), its
// errors {"name", "message"} included.
//
// Threads: a POST is handled on the broker's thread for the port, its records handed to the broker there
// (sampleIn); publish() runs on a broker thread and only queues the task data; the sender thread POSTs it
// to the MICS, one at a time.
//
#include <pthread.h>                                  // pthread_*
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, calloc, free, getenv
#include <string.h>                                   // strcmp, strcasecmp, strstr, strdup, strlen, memcpy
#include <strings.h>                                  // strcasecmp, strncasecmp

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_I, COR_V
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BridgeHttpRequest, BRIDGE_*

#include "isoxmlXml.h"                                // isoxmlParse, isoxmlRender, isoxmlAppend
#include "micsHttp.h"                                 // MicsTarget, micsTargetInit, micsRequest
#include "isoxmlSouth.h"                              // Own interface



// -----------------------------------------------------------------------------
//
// Endpoint - a DataChannel's: an element's tag, or "*"
//
typedef struct Endpoint
{
  char*             endpoint;
  struct Endpoint*  next;
} Endpoint;



// -----------------------------------------------------------------------------
//
// Send - task data queued for the MICS
//
#define SENDS_MAX  256

typedef struct Send
{
  char*         xml;
  char          what[160];                            // for the log: <tag> <id>
  struct Send*  next;
} Send;



static const BridgeBroker*  brokerP      = NULL;
static char                 path[256]    = "/iot/isoxml";
static pthread_mutex_t      mutex        = PTHREAD_MUTEX_INITIALIZER;
static Endpoint*            endpointList = NULL;

static bool                 micsOn       = false;
static MicsTarget           mics;
static pthread_t            sender;
static pthread_cond_t       sendCond     = PTHREAD_COND_INITIALIZER;
static Send*                sendHead     = NULL;
static Send*                sendTail     = NULL;
static int                  sends        = 0;
static bool                 stopping     = false;



// -----------------------------------------------------------------------------
//
// errorText - iotagent-isoxml's error: {"name": "<NAME>", "message": "<message>"}
//
static void errorText(char* buf, int size, const char* name, const char* message)
{
  int at = snprintf(buf, size, "{\"name\":\"%s\",\"message\":\"", name);

  for (const char* s = message; (*s != 0) && (at < size - 8); s++)
  {
    if ((*s == '"') || (*s == '\\'))
      buf[at++] = '\\';
    buf[at++] = ((unsigned char) *s < 0x20) ? ' ' : *s;
  }
  snprintf(&buf[at], size - at, "\"}");
}

static int error(char** bodyP, int status, const char* name, const char* message)
{
  char body[1024];

  errorText(body, sizeof(body), name, message);
  *bodyP = strdup(body);
  return (*bodyP != NULL) ? status : 500;
}



// -----------------------------------------------------------------------------
//
// endpointOf - the DataChannel of an element's tag: its own, else "*"; into 'endpoint'. false: none
//
static bool endpointOf(const char* tag, char* endpoint, int size)
{
  bool any   = false;
  bool found = false;

  pthread_mutex_lock(&mutex);

  for (Endpoint* eP = endpointList; (eP != NULL) && (found == false); eP = eP->next)
  {
    if (strcmp(eP->endpoint, tag) == 0)
      found = true;
    else if (strcmp(eP->endpoint, "*") == 0)
      any = true;
  }

  pthread_mutex_unlock(&mutex);

  if ((found == false) && (any == false))
    return false;

  snprintf(endpoint, size, "%s", found ? tag : "*");
  return true;
}



// -----------------------------------------------------------------------------
//
// serve - task data from the MICS:
//
//   POST <path>   Content-Type: application/xml   <ISO11783_TaskData> <CTR A="CTR1" .../> ... </ISO11783_TaskData>
//
// Each element under the root - an entity in iotagent-isoxml - a record on the DataChannel of its tag: its
// attributes columns (A, B, ...), its child elements arrays of objects under their tags, @deviceId its id
// (A), @element its tag. 200 when each was taken; one element, not taken: its error; none taken 400, some
// 202 - the errors in an array.
//
static int serve(const BridgeHttpRequest* reqP, char** bodyP, const char** contentTypeP)
{
  (void) contentTypeP;

  if (strcmp(reqP->path, path) != 0)
    return error(bodyP, 404, "NOT_FOUND", "task data is POSTed to the agent's resource");

  if (strcmp(reqP->method, "POST") != 0)
    return error(bodyP, 405, "METHOD_NOT_ALLOWED", "task data is POSTed");

  if (reqP->bodyTooBig)
    return error(bodyP, 413, "PAYLOAD_TOO_LARGE", "the task data is over 1 MB");

  const char* contentType = reqP->header(reqP, "Content-Type");

  if ((contentType != NULL) && (strncasecmp(contentType, "application/xml", 15) != 0) && (strncasecmp(contentType, "text/xml", 8) != 0))
    return error(bodyP, 400, "UNSUPPORTED_TYPE", "The request content didn't have the expected type [application/xml ]");

  CorAlloc ka;
  char     kaBuf[16 * 1024];
  char     why[256];

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 64 * 1024, NULL, "iotaisoxml task data");

  char*    text  = corAlloc(&ka, reqP->bodyLen + 1);
  CorNode* rootP = NULL;

  if (text != NULL)
  {
    memcpy(text, reqP->body, reqP->bodyLen);
    text[reqP->bodyLen] = 0;
    rootP = isoxmlParse(&ka, text, why, sizeof(why));
  }
  else
    snprintf(why, sizeof(why), "out of memory");

  if ((rootP != NULL) && (strcasecmp(rootP->name, "ISO11783_TaskData") != 0))
  {
    snprintf(why, sizeof(why), "the root element is %s, not ISO11783_TaskData", rootP->name);
    rootP = NULL;
  }

  if (rootP == NULL)
  {
    char message[400];

    snprintf(message, sizeof(message), "There was a syntax error in the ISOXML request: %s", why);
    corAllocBufferReset(&ka, false);
    COR_W("iotaisoxml: task data refused - %s", why);
    return error(bodyP, 400, "PARSE_ERROR", message);
  }

  //
  // Every element has its id (A) - checked before any is taken
  //
  char missing[512] = "";
  int  at           = 0;
  int  elements     = 0;

  for (CorNode* arrayP = rootP->value.head; arrayP != NULL; arrayP = arrayP->next)
  {
    if (arrayP->type != CorArray)
      continue;                                       // the root's own attributes: VersionMajor ...

    for (CorNode* elementP = arrayP->value.head; elementP != NULL; elementP = elementP->next)
    {
      CorNode* idP = corTreeLookup(elementP, "A");

      elements += 1;

      if ((idP == NULL) || (idP->value.s[0] == 0))
      {
        char tag[64];
        int  ix = 0;

        for (; (arrayP->name[ix] != 0) && (ix < (int) sizeof(tag) - 1); ix++)
          tag[ix] = ((arrayP->name[ix] >= 'A') && (arrayP->name[ix] <= 'Z')) ? arrayP->name[ix] + 32 : arrayP->name[ix];
        tag[ix] = 0;

        at += snprintf(&missing[at], sizeof(missing) - at, "%s\"Id for Entity: %s\"", (at == 0) ? "" : ",", tag);
        if (at >= (int) sizeof(missing))
          at = sizeof(missing) - 1;
      }
    }
  }

  if (missing[0] != 0)
  {
    char message[700];

    snprintf(message, sizeof(message), "Some of the mandatory params weren't found in the request: [%s]", missing);
    corAllocBufferReset(&ka, false);
    COR_W("iotaisoxml: task data refused - %s", message);
    return error(bodyP, 400, "MANDATORY_PARAMS_NOT_FOUND", message);
  }

  //
  // The records - an element no DataChannel takes an error of its own
  //
  char  errors[4096] = "[";
  int   errorsLen    = 1;
  int   failed       = 0;
  char  lastError[600];
  bool  full         = false;

  for (CorNode* arrayP = rootP->value.head; arrayP != NULL; arrayP = arrayP->next)
  {
    if (arrayP->type != CorArray)
      continue;

    for (CorNode* elementP = arrayP->value.head; elementP != NULL; elementP = elementP->next)
    {
      const char* id = corTreeLookup(elementP, "A")->value.s;
      char        endpoint[128];

      if (endpointOf(arrayP->name, endpoint, sizeof(endpoint)) == false)
      {
        char message[300];

        snprintf(message, sizeof(message), "Device not found with ID [%s]", id);
        errorText(lastError, sizeof(lastError), "DEVICE_NOT_FOUND", message);
        COR_V("iotaisoxml: %s %s - no DataChannel has it", arrayP->name, id);

        int n = strlen(lastError);

        if ((full == false) && (errorsLen + n + 3 < (int) sizeof(errors)))
        {
          if (failed > 0)
            errors[errorsLen++] = ',';
          memcpy(&errors[errorsLen], lastError, n);
          errorsLen += n;
        }
        else
          full = true;

        failed += 1;
        continue;
      }

      CorNode* recordP = corTreeClone(&ka, elementP);

      if (recordP == NULL)
        continue;

      recordP->name = NULL;
      corTreeChildAdd(recordP, corTreeString(&ka, "@deviceId", id));
      corTreeChildAdd(recordP, corTreeString(&ka, "@element", arrayP->name));

      int   size = corJsonFastRenderSize(recordP);
      char* json = malloc(size + 1);

      if (json != NULL)
      {
        corJsonFastRender(recordP, json);

        int r = brokerP->sampleIn("iotaisoxml", endpoint, json, 0);

        COR_V("iotaisoxml: record on %s (%d): %s", endpoint, r, json);
        free(json);
      }
    }
  }

  errors[errorsLen++] = ']';
  errors[errorsLen]   = 0;
  corAllocBufferReset(&ka, false);

  if (failed == 0)
  {
    *bodyP        = NULL;
    *contentTypeP = "text/plain";
    return 200;
  }

  if ((failed == 1) && (elements == 1))
  {
    *bodyP = strdup(lastError);
    return (*bodyP != NULL) ? 404 : 500;
  }

  *bodyP = strdup(errors);
  if (*bodyP == NULL)
    return 500;

  return (failed == elements) ? 400 : 202;
}



// -----------------------------------------------------------------------------
//
// senderThread - the queued task data POSTed to the MICS, one at a time
//
static void* senderThread(void* unused)
{
  (void) unused;

  while (true)
  {
    pthread_mutex_lock(&mutex);

    while ((sendHead == NULL) && (stopping == false))
      pthread_cond_wait(&sendCond, &mutex);

    if ((sendHead == NULL) && (stopping == true))
    {
      pthread_mutex_unlock(&mutex);
      return NULL;
    }

    Send* sP = sendHead;

    sendHead = sP->next;
    if (sendHead == NULL)
      sendTail = NULL;
    sends -= 1;

    pthread_mutex_unlock(&mutex);

    char* answer = NULL;
    int   status = micsRequest(&mics, "POST", (mics.basePath[0] == 0) ? "/" : "", sP->xml, &answer, 10000);

    if (status == 0)
      COR_W("iotaisoxml: the task data of %s not sent to the MICS - %s", sP->what, micsHttpError());
    else if ((status < 200) || (status > 299))
      COR_W("iotaisoxml: the MICS refused the task data of %s: %d %.200s", sP->what, status, (answer != NULL) ? answer : "");
    else
      COR_V("iotaisoxml: the task data of %s sent to the MICS (%d)", sP->what, status);

    free(answer);
    free(sP->xml);
    free(sP);
  }
}



// -----------------------------------------------------------------------------
//
// micsOpen - "mics": { "url", "authorizationEnv", "caFile", "insecure" }
//
static int micsOpen(CorNode* micsP)
{
  CorNode*    urlP      = corTreeLookup(micsP, "url");
  CorNode*    authEnvP  = corTreeLookup(micsP, "authorizationEnv");
  CorNode*    caFileP   = corTreeLookup(micsP, "caFile");
  CorNode*    insecureP = corTreeLookup(micsP, "insecure");
  const char* auth      = NULL;

  if ((urlP == NULL) || (urlP->type != CorString))
  {
    COR_E("iotaisoxml: mics.url - where the task data is POSTed, http[s]://<host>[:<port>][/<path>]");
    return BRIDGE_ERR;
  }

  if (authEnvP != NULL)
  {
    if ((authEnvP->type != CorString) || ((auth = getenv(authEnvP->value.s)) == NULL))
    {
      COR_E("iotaisoxml: mics.authorizationEnv names an environment variable that is not set");
      return BRIDGE_ERR;
    }
  }

  const char* why = micsTargetInit(&mics, urlP->value.s, auth,
                                   ((caFileP != NULL) && (caFileP->type == CorString)) ? caFileP->value.s : NULL,
                                   (insecureP != NULL) && (insecureP->type == CorBoolean) && (insecureP->value.b == true));

  if (why != NULL)
  {
    COR_E("iotaisoxml: %s", why);
    return BRIDGE_ERR;
  }

  stopping = false;
  if (pthread_create(&sender, NULL, senderThread, NULL) != 0)
  {
    micsTargetRelease(&mics);
    return BRIDGE_ERR;
  }

  micsOn = true;
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// isoxmlSouthOpen -
//
int isoxmlSouthOpen(CorNode* southP, CorNode* micsP, const BridgeBroker* _brokerP)
{
  long long port = 7896;

  brokerP = _brokerP;

  if (southP != NULL)
  {
    CorNode* portP = corTreeLookup(southP, "port");
    CorNode* pathP = corTreeLookup(southP, "path");

    if (portP != NULL)
    {
      if ((portP->type != CorInt) || (portP->value.i < 1) || (portP->value.i > 65535))
      {
        COR_E("iotaisoxml: southbound port - a port number, 1-65535");
        return BRIDGE_ERR;
      }
      port = portP->value.i;
    }

    if (pathP != NULL)
    {
      if ((pathP->type != CorString) || (pathP->value.s[0] != '/') || (strlen(pathP->value.s) >= sizeof(path)))
      {
        COR_E("iotaisoxml: southbound path - the resource task data is POSTed to, /iot/isoxml");
        return BRIDGE_ERR;
      }
      snprintf(path, sizeof(path), "%s", pathP->value.s);
    }
  }

  if ((micsP != NULL) && (micsOpen(micsP) != BRIDGE_OK))
    return BRIDGE_ERR;

  if ((brokerP->abiVersion < 16) || (brokerP->httpListen == NULL))
  {
    COR_E("iotaisoxml: the southbound port %lld needs a broker that serves it (bridge ABI 16, httpListen)", port);
    return BRIDGE_ERR;
  }

  if (brokerP->httpListen("iotaisoxml", (unsigned short) port, serve) != BRIDGE_OK)
  {
    COR_E("iotaisoxml: cannot listen on the southbound port %lld", port);
    return BRIDGE_ERR;
  }

  COR_I("iotaisoxml: task data on port %lld, %s%s", port, path, micsOn ? "; written attributes to the MICS" : "");
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// isoxmlSouthClose - what is still queued is dropped
//
void isoxmlSouthClose(void)
{
  if (micsOn)
  {
    pthread_mutex_lock(&mutex);
    stopping = true;

    while (sendHead != NULL)
    {
      Send* sP = sendHead;

      sendHead = sP->next;
      free(sP->xml);
      free(sP);
    }
    sendTail = NULL;
    sends    = 0;

    pthread_cond_signal(&sendCond);
    pthread_mutex_unlock(&mutex);

    pthread_join(sender, NULL);
    micsTargetRelease(&mics);
    micsOn = false;
  }

  pthread_mutex_lock(&mutex);

  while (endpointList != NULL)
  {
    Endpoint* eP = endpointList;

    endpointList = eP->next;
    free(eP->endpoint);
    free(eP);
  }

  pthread_mutex_unlock(&mutex);
  brokerP = NULL;
}



// -----------------------------------------------------------------------------
//
// isoxmlEndpointAdd - a tag: letters and digits, as ISOXML's three-letter codes
//
int isoxmlEndpointAdd(const char* endpoint)
{
  bool ok = (endpoint[0] != 0) && (strlen(endpoint) < 64);

  if (strcmp(endpoint, "*") != 0)
  {
    for (const char* s = endpoint; (*s != 0) && ok; s++)
      ok = ((*s >= 'A') && (*s <= 'Z')) || ((*s >= 'a') && (*s <= 'z')) || ((*s >= '0') && (*s <= '9')) || (*s == '_');
  }

  if (ok == false)
    return BRIDGE_BAD_INPUT;

  Endpoint* eP = calloc(1, sizeof(Endpoint));

  if ((eP == NULL) || ((eP->endpoint = strdup(endpoint)) == NULL))
  {
    free(eP);
    return BRIDGE_ERR;
  }

  pthread_mutex_lock(&mutex);
  eP->next     = endpointList;
  endpointList = eP;
  pthread_mutex_unlock(&mutex);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// isoxmlEndpointDel -
//
int isoxmlEndpointDel(const char* endpoint)
{
  Endpoint* foundP = NULL;

  pthread_mutex_lock(&mutex);

  for (Endpoint** ePP = &endpointList; *ePP != NULL; ePP = &(*ePP)->next)
  {
    if (strcmp((*ePP)->endpoint, endpoint) == 0)
    {
      foundP = *ePP;
      *ePP   = foundP->next;
      break;
    }
  }

  pthread_mutex_unlock(&mutex);

  if (foundP == NULL)
    return BRIDGE_NOT_FOUND;

  free(foundP->endpoint);
  free(foundP);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// isoxmlPublish - the fragment as the element it is a part of, in task data of its own, queued for the MICS:
//
//   <?xml version="1.0" encoding="UTF-8"?>
//   <ISO11783_TaskData VersionMajor="4" VersionMinor="3" DataTransferOrigin="1"><TSK A="TSK11" G="4"/></ISO11783_TaskData>
//
// The element: its tag the endpoint's, else (on "*") the fragment's @element; its id (A) the fragment's
// @deviceId (the id template's); its attributes and child elements the columns written.
//
int isoxmlPublish(const char* endpoint, const char* json)
{
  if (micsOn == false)
  {
    COR_W("iotaisoxml: a write on '%s' refused - no MICS configured (mics.url)", endpoint);
    return BRIDGE_UNSUPPORTED;
  }

  CorAlloc ka;
  CorJson  cj;
  char     kaBuf[8 * 1024];
  char*    copy = strdup(json);
  int      r    = BRIDGE_OK;
  char*    xml  = NULL;
  int      len  = 0;
  int      size = 0;
  char     tag[64];
  char     id[128] = "";
  char     why[256];

  if (copy == NULL)
    return BRIDGE_ERR;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 32 * 1024, NULL, "iotaisoxml write");
  corJsonCreate(&cj, &ka);

  CorNode* fragmentP = corJsonParse(&cj, copy);
  CorNode* elementP  = corTreeObject(&ka, NULL);

  if ((fragmentP == NULL) || (fragmentP->type != CorObject) || (elementP == NULL))
    r = BRIDGE_BAD_INPUT;
  else
  {
    CorNode* idP      = corTreeLookup(fragmentP, "@deviceId");
    CorNode* elementTagP = corTreeLookup(fragmentP, "@element");

    if (strcmp(endpoint, "*") != 0)
      snprintf(tag, sizeof(tag), "%s", endpoint);
    else if ((elementTagP != NULL) && (elementTagP->type == CorString))
      snprintf(tag, sizeof(tag), "%s", elementTagP->value.s);
    else
      tag[0] = 0;

    if ((idP != NULL) && (idP->type == CorString))
      snprintf(id, sizeof(id), "%s", idP->value.s);

    if ((tag[0] == 0) || (id[0] == 0))
    {
      COR_W("iotaisoxml: a write on '%s' refused - which element: the entity's id template has no %s", endpoint, (id[0] == 0) ? "{@deviceId}" : "{@element}");
      r = BRIDGE_BAD_INPUT;
    }
    else
    {
      corTreeChildAdd(elementP, corTreeString(&ka, "A", id));

      for (CorNode* memberP = fragmentP->value.head; memberP != NULL; )
      {
        CorNode* nextP = memberP->next;

        if ((memberP->name[0] != '@') && (strcmp(memberP->name, "A") != 0))
          corTreeChildAdd(elementP, memberP);

        memberP = nextP;
      }

      const char* head = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ISO11783_TaskData VersionMajor=\"4\" VersionMinor=\"3\" DataTransferOrigin=\"1\">";
      const char* tail = "</ISO11783_TaskData>\n";

      if ((isoxmlAppend(&xml, &len, &size, head, strlen(head)) == false) ||
          (isoxmlRender(tag, elementP, &xml, &len, &size, why, sizeof(why)) == false) ||
          (isoxmlAppend(&xml, &len, &size, tail, strlen(tail)) == false))
      {
        COR_W("iotaisoxml: a write of %s %s refused - %s", tag, id, (xml != NULL) ? why : "out of memory");
        r = BRIDGE_BAD_INPUT;
      }
    }
  }

  if (r == BRIDGE_OK)
  {
    Send* sP = calloc(1, sizeof(Send));

    pthread_mutex_lock(&mutex);

    if ((sP == NULL) || (sends >= SENDS_MAX))
    {
      COR_W("iotaisoxml: a write of %s %s refused - %d writes wait for the MICS", tag, id, SENDS_MAX);
      free(sP);
      r = BRIDGE_ERR;
    }
    else
    {
      sP->xml = xml;
      xml     = NULL;
      snprintf(sP->what, sizeof(sP->what), "%s %s", tag, id);

      if (sendTail == NULL)
        sendHead = sP;
      else
        sendTail->next = sP;
      sendTail = sP;
      sends   += 1;
      pthread_cond_signal(&sendCond);
    }

    pthread_mutex_unlock(&mutex);
  }

  free(xml);
  corAllocBufferReset(&ka, false);
  free(copy);
  return r;
}



// -----------------------------------------------------------------------------
//
// isoxmlVersion -
//
const char* isoxmlVersion(void)
{
  return micsHttpVersion();
}
