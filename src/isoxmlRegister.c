//
// FILE            isoxmlRegister.c
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
// The iotaisoxml bridge - the SEAM: what the broker calls, and the one symbol it looks for (bridgeRegister).
// The contract is corBridge's BridgeDriver.h (broker -> plugin) and BridgeBroker.h (plugin -> broker).
//
// The IoT Agent for ISOXML (ISO 11783-10 task data): the MICS's TASKDATA.XML, each element a DataChannel
// record - a DataChannel's endpoint is an element's tag (CTR, TSK ...) or * - and with "channelDirection":
// "both" a written attribute is task data POSTed to the MICS (isoxmlSouth.c). A Channel or an AssetChannel
// is refused - this bridge carries DataChannels only.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // fopen, fread, fseek, ftell, snprintf
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // strcmp

#include "corLog/corLog.h"                            // COR_E, COR_W
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeChannelKind, BridgeDirection
#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "isoxmlSouth.h"                              // isoxmlSouth*, isoxmlEndpoint*, isoxmlPublish, isoxmlVersion



static const BridgeBroker* hostP = NULL;



// -----------------------------------------------------------------------------
//
// configLoad - the whole --bridgeConfig file, NUL-terminated; malloc'd. NULL: not readable
//
static char* configLoad(const char* path)
{
  FILE* fP = fopen(path, "r");
  if (fP == NULL)
    return NULL;

  char* text = NULL;
  long  size = (fseek(fP, 0, SEEK_END) == 0) ? ftell(fP) : -1;

  if ((size >= 0) && (fseek(fP, 0, SEEK_SET) == 0) && ((text = malloc(size + 1)) != NULL))
  {
    if (fread(text, 1, size, fP) == (size_t) size)
      text[size] = 0;
    else
    {
      free(text);
      text = NULL;
    }
  }

  fclose(fP);
  return text;
}



// -----------------------------------------------------------------------------
//
// iotaisoxmlInit - the southbound port and the MICS, from the --bridgeConfig file's member of this plugin:
//
//   { "iotaisoxml": { "southbound": { "port": 7896, "path": "/iot/isoxml" },
//                     "mics": { "url": "https://mics.farm.example/taskdata", "authorizationEnv": "MICS_AUTHORIZATION" },
//                     "ngsild": { "dataChannels": { "TSK": { ... } } } } }
//
// No "southbound": iotagent-isoxml's 7896 and /iot/isoxml. No "mics": no write-back.
//
static int iotaisoxmlInit(const char* configFile, const BridgeBroker* brokerP)
{
  CorAlloc  ka;
  CorJson   cj;
  char      buf[16 * 1024];
  char*     text   = (configFile != NULL) ? configLoad(configFile) : NULL;
  CorNode*  southP = NULL;
  CorNode*  micsP  = NULL;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "iotaisoxml config");
  corJsonCreate(&cj, &ka);

  if (text != NULL)
  {
    CorNode* rootP   = corJsonParse(&cj, text);       // valid JSON: the broker has parsed the same file
    CorNode* bridgeP = (rootP != NULL) ? corTreeLookup(rootP, "iotaisoxml") : NULL;

    southP = (bridgeP != NULL) ? corTreeLookup(bridgeP, "southbound") : NULL;
    micsP  = (bridgeP != NULL) ? corTreeLookup(bridgeP, "mics")       : NULL;
  }

  hostP = brokerP;

  int r = isoxmlSouthOpen(southP, micsP, brokerP);

  corAllocBufferReset(&ka, false);                    // nothing parsed here is kept: copies were made
  free(text);
  return r;
}



// -----------------------------------------------------------------------------
//
// iotaisoxmlClose - the broker has stopped serving the port: no handler runs
//
static void iotaisoxmlClose(void)
{
  isoxmlSouthClose();
}



// -----------------------------------------------------------------------------
//
// The DataChannels - each an element's tag, or *. A broker of ABI 15 says which channel type
// an endpoint is (channelTypedAdd); an older one hands every channel over alike (channelAddInfo), and each
// is taken as a DataChannel.
//
static int iotaisoxmlChannelAddInfo(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  (void) kind;
  (void) direction;                                   // in: the task data; both: also to the MICS

  if ((info != NULL) && (strcmp(info, "[]") != 0))
  {
    COR_W("iotaisoxml: DataChannel '%s' refused - an ISOXML DataChannel has no channelInfo", endpoint);
    return BRIDGE_BAD_INPUT;
  }

  int r = isoxmlEndpointAdd(endpoint);

  if (r == BRIDGE_BAD_INPUT)
    COR_W("iotaisoxml: DataChannel '%s' refused - its endpoint is an element's tag (CTR, TSK ...) or *", endpoint);

  return r;
}

#if BRIDGE_ABI_VERSION >= 15
static int iotaisoxmlChannelTypedAdd(const char* endpoint, BridgeChannelType type, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  if (type == BridgeChannelTypeDataChannel)
    return iotaisoxmlChannelAddInfo(endpoint, kind, direction, info);

  const char* reason = "the iotaisoxml bridge carries DataChannels only - a task data element is a record";

  if ((hostP != NULL) && (hostP->abiVersion >= 12) && (hostP->channelStatusIn != NULL))
    hostP->channelStatusIn("iotaisoxml", endpoint, BridgeChannelDormant, reason);

  COR_W("iotaisoxml: '%s' refused - %s", endpoint, reason);
  return BRIDGE_UNSUPPORTED;
}
#endif

static int iotaisoxmlChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  return iotaisoxmlChannelAddInfo(endpoint, kind, direction, NULL);
}

static int iotaisoxmlChannelDel(const char* endpoint)
{
  return isoxmlEndpointDel(endpoint);
}

static int iotaisoxmlPublish(const char* endpoint, const char* json)
{
  return isoxmlPublish(endpoint, json);
}



// -----------------------------------------------------------------------------
//
// iotaisoxmlVersionInfo - one line for GET /version
//
static const char* iotaisoxmlVersionInfo(void)
{
  static char info[128];

  snprintf(info, sizeof(info), "iotaisoxml 0.1.0 (%s)", isoxmlVersion());
  return info;
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol the broker looks for, and the one this .so exports
//
// A slot is filled only when the broker's abiVersion has it; abiVersion is then overwritten with this
// plugin's. Which channel type an endpoint is, ABI 15: before it, every channel is taken as a DataChannel.
// The southbound port needs ABI 16 - init() refuses an older broker.
//
__attribute__((visibility("default"))) void bridgeRegister(BridgeDriver* driverP)
{
  if (driverP->abiVersion < 1)
    driverP->abiVersion = 1;                          // a host from before the handshake

  driverP->alias       = "iotaisoxml";                // --bridges iotaisoxml, and its member of --bridgeConfig
  driverP->version     = "0.1.0";
  driverP->args        = NULL;
  driverP->init        = iotaisoxmlInit;
  driverP->close       = iotaisoxmlClose;
  driverP->versionInfo = iotaisoxmlVersionInfo;
  driverP->channelAdd  = iotaisoxmlChannelAdd;
  driverP->channelDel  = iotaisoxmlChannelDel;
  driverP->publish     = iotaisoxmlPublish;

  if (driverP->abiVersion >= 9)
    driverP->channelAddInfo = iotaisoxmlChannelAddInfo;

#if BRIDGE_ABI_VERSION >= 15
  if (driverP->abiVersion >= 15)
    driverP->channelTypedAdd = iotaisoxmlChannelTypedAdd;
#endif

  driverP->abiVersion  = BRIDGE_ABI_VERSION;
}
