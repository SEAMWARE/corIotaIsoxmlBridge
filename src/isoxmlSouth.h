#ifndef ISOXML_SOUTH_H_
#define ISOXML_SOUTH_H_

//
// FILE            isoxmlSouth.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Task data in and out: the MICS POSTs TASKDATA.XML to the southbound port the broker serves for the plugin
// (BridgeBroker.httpListen, ABI 16) - each element under ISO11783_TaskData a DataChannel record - and a
// written attribute is task data POSTed to the MICS, on a thread of its own.
//
#include <stdbool.h>                                  // bool

#include "corTree/CorNode.h"                          // CorNode
#include "corBridge/BridgeBroker.h"                   // BridgeBroker



// -----------------------------------------------------------------------------
//
// isoxmlSouthOpen - the southbound port and path ("southbound" config member, NULL: 7896 /iot/isoxml), the
// MICS ("mics" config member, NULL: no write-back); BRIDGE_OK or BRIDGE_ERR
//
extern int isoxmlSouthOpen(CorNode* southP, CorNode* micsP, const BridgeBroker* brokerP);



// -----------------------------------------------------------------------------
//
// isoxmlSouthClose - the sender thread stopped, the DataChannels freed
//
extern void isoxmlSouthClose(void);



// -----------------------------------------------------------------------------
//
// isoxmlEndpointAdd / Del - a DataChannel's endpoint: an element's tag (CTR, TSK ...), or * - every element
// no DataChannel of its tag has
//
extern int isoxmlEndpointAdd(const char* endpoint);
extern int isoxmlEndpointDel(const char* endpoint);



// -----------------------------------------------------------------------------
//
// isoxmlPublish - a write-back fragment as task data to the MICS
//
extern int isoxmlPublish(const char* endpoint, const char* json);



// -----------------------------------------------------------------------------
//
// isoxmlVersion - the TLS library linked, for GET /version
//
extern const char* isoxmlVersion(void);

#endif  // ISOXML_SOUTH_H_
