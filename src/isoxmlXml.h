#ifndef ISOXML_XML_H_
#define ISOXML_XML_H_

//
// FILE            isoxmlXml.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// ISO 11783-10 task data (TASKDATA.XML) as JSON, and JSON as task data - the XML ISOXML is: elements and
// their attributes, no text content. An element is an object: its attributes members of their own names
// (A, B, C ...), its child elements arrays under their tags (PAN: [ {...}, ... ]).
//
// The parser takes what ISOXML files have - the XML declaration, comments, a DOCTYPE, the five named
// entities and character references; text between elements is skipped. No namespaces, no DTD.
//
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// isoxmlParse - an XML document (modified in place) as its root element: an object named by its tag, in
// kaP; NULL: why in 'why'
//
extern CorNode* isoxmlParse(CorAlloc* kaP, char* text, char* why, int whyLen);



// -----------------------------------------------------------------------------
//
// isoxmlRender - an element as XML: <tag> with the object's scalar members as attributes (a boolean 1 or
// 0), its arrays of objects as child elements - and an object member as one child element (NGSI-LD
// compacts an array of one to its item); into a malloc'd buffer appended to (*bufP, *lenP, *sizeP).
// false: a member that is none of these (null, an array of scalars), or out of memory - why in 'why'
//
extern bool isoxmlRender(const char* tag, CorNode* objectP, char** bufP, int* lenP, int* sizeP, char* why, int whyLen);



// -----------------------------------------------------------------------------
//
// isoxmlAppend - text appended to the buffer isoxmlRender writes; false: out of memory
//
extern bool isoxmlAppend(char** bufP, int* lenP, int* sizeP, const char* text, int textLen);

#endif  // ISOXML_XML_H_
