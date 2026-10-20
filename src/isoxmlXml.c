//
// FILE            isoxmlXml.c
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
// Task data XML as JSON and back - see header.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // realloc, strtol
#include <string.h>                                   // strncmp, strchr, strstr, strlen, memcpy

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "isoxmlXml.h"                                // Own interface



#define DEPTH_MAX  32



// -----------------------------------------------------------------------------
//
// isName - a character of an XML name (as ISOXML's: letters, digits, '_', '-', '.', ':')
//
static bool isName(char c)
{
  return ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-') || (c == '.') || (c == ':');
}

static bool isSpace(char c)
{
  return (c == ' ') || (c == '\t') || (c == '\n') || (c == '\r');
}



// -----------------------------------------------------------------------------
//
// unescape - an attribute value's references replaced, in place; false: one that is not known
//
static bool unescape(char* s)
{
  char* out = s;

  while (*s != 0)
  {
    if (*s != '&')
    {
      *out++ = *s++;
      continue;
    }

    char* semi = strchr(s, ';');

    if (semi == NULL)
      return false;

    int len = semi - s - 1;

    if      ((len == 2) && (strncmp(s + 1, "lt", 2) == 0))    *out++ = '<';
    else if ((len == 2) && (strncmp(s + 1, "gt", 2) == 0))    *out++ = '>';
    else if ((len == 3) && (strncmp(s + 1, "amp", 3) == 0))   *out++ = '&';
    else if ((len == 4) && (strncmp(s + 1, "quot", 4) == 0))  *out++ = '"';
    else if ((len == 4) && (strncmp(s + 1, "apos", 4) == 0))  *out++ = '\'';
    else if ((len >= 2) && (s[1] == '#'))
    {
      char* end;
      long  cp = (s[2] == 'x') ? strtol(s + 3, &end, 16) : strtol(s + 2, &end, 10);

      if ((end != semi) || (cp <= 0) || (cp > 0x10FFFF))
        return false;

      if (cp < 0x80)
        *out++ = cp;
      else if (cp < 0x800)
      {
        *out++ = 0xC0 | (cp >> 6);
        *out++ = 0x80 | (cp & 0x3F);
      }
      else if (cp < 0x10000)
      {
        *out++ = 0xE0 | (cp >> 12);
        *out++ = 0x80 | ((cp >> 6) & 0x3F);
        *out++ = 0x80 | (cp & 0x3F);
      }
      else
      {
        *out++ = 0xF0 | (cp >> 18);
        *out++ = 0x80 | ((cp >> 12) & 0x3F);
        *out++ = 0x80 | ((cp >> 6) & 0x3F);
        *out++ = 0x80 | (cp & 0x3F);
      }
    }
    else
      return false;

    s = semi + 1;
  }

  *out = 0;
  return true;
}



// -----------------------------------------------------------------------------
//
// childArray - the array of an element's children of a tag, made when it is not there
//
static CorNode* childArray(CorAlloc* kaP, CorNode* elementP, const char* tag)
{
  CorNode* arrayP = corTreeLookup(elementP, tag);

  if ((arrayP != NULL) && (arrayP->type == CorArray))
    return arrayP;

  if (arrayP != NULL)
    return NULL;                                      // an attribute of the same name

  if ((arrayP = corTreeArray(kaP, tag)) != NULL)
    corTreeChildAdd(elementP, arrayP);

  return arrayP;
}



// -----------------------------------------------------------------------------
//
// isoxmlParse - elemV: the open elements, nameV their tags (an item of an array has no name of its own)
//
CorNode* isoxmlParse(CorAlloc* kaP, char* text, char* why, int whyLen)
{
  CorNode*    elemV[DEPTH_MAX];
  const char* nameV[DEPTH_MAX];
  int         depth = 0;
  CorNode*    rootP = NULL;
  char*       p     = text;

  while (*p != 0)
  {
    if (*p != '<')
    {
      if ((isSpace(*p) == false) && (depth == 0))
      {
        snprintf(why, whyLen, "text outside the root element");
        return NULL;
      }
      ++p;                                            // text in an element: ISOXML has none, skipped
      continue;
    }

    if (strncmp(p, "<?", 2) == 0)
    {
      char* end = strstr(p, "?>");

      if (end == NULL)
      {
        snprintf(why, whyLen, "a processing instruction that does not end");
        return NULL;
      }
      p = end + 2;
      continue;
    }

    if (strncmp(p, "<!--", 4) == 0)
    {
      char* end = strstr(p, "-->");

      if (end == NULL)
      {
        snprintf(why, whyLen, "a comment that does not end");
        return NULL;
      }
      p = end + 3;
      continue;
    }

    if (strncmp(p, "<![CDATA[", 9) == 0)
    {
      char* end = strstr(p, "]]>");

      if (end == NULL)
      {
        snprintf(why, whyLen, "a CDATA section that does not end");
        return NULL;
      }
      p = end + 3;
      continue;
    }

    if (strncmp(p, "<!", 2) == 0)                     // DOCTYPE - an internal subset is not taken
    {
      char* end = strchr(p, '>');

      if ((end == NULL) || (memchr(p, '[', end - p) != NULL))
      {
        snprintf(why, whyLen, "a DOCTYPE that does not end, or has an internal subset");
        return NULL;
      }
      p = end + 1;
      continue;
    }

    //
    // An end tag
    //
    if (p[1] == '/')
    {
      char* name = p + 2;
      char* end  = name;

      while (isName(*end))
        ++end;

      char* close = end;

      while (isSpace(*close))
        ++close;

      if ((*close != '>') || (depth == 0) || ((int) strlen(nameV[depth - 1]) != end - name) || (strncmp(nameV[depth - 1], name, end - name) != 0))
      {
        snprintf(why, whyLen, "an end tag that does not match its start tag");
        return NULL;
      }

      depth -= 1;
      p      = close + 1;
      continue;
    }

    //
    // A start tag: its name - terminated at once, the character after it looked at first - its
    // attributes, then '>' or '/>'
    //
    char* name = p + 1;
    char* end  = name;

    while (isName(*end))
      ++end;

    if ((end == name) || ((*end != '>') && (*end != '/') && (isSpace(*end) == false)))
    {
      snprintf(why, whyLen, "a '<' that starts no element");
      return NULL;
    }

    if ((rootP != NULL) && (depth == 0))
    {
      snprintf(why, whyLen, "a second root element");
      return NULL;
    }

    if (depth == DEPTH_MAX)
    {
      snprintf(why, whyLen, "elements nested deeper than %d", DEPTH_MAX);
      return NULL;
    }

    bool selfClosing = false;
    bool closed      = false;

    if (*end == '>')
      closed = true;
    else if (*end == '/')
    {
      if (end[1] != '>')
      {
        snprintf(why, whyLen, "a '/' in a start tag that is not '/>'");
        return NULL;
      }
      selfClosing = true;
      closed      = true;
      p           = end + 2;
    }

    if ((closed == true) && (selfClosing == false))
      p = end + 1;
    else if (closed == false)
      p = end + 1;

    *end = 0;

    CorNode* elementP = corTreeObject(kaP, (depth == 0) ? name : NULL);

    if (elementP == NULL)
    {
      snprintf(why, whyLen, "out of memory");
      return NULL;
    }

    while (closed == false)
    {
      while (isSpace(*p))
        ++p;

      if (*p == '>')
      {
        ++p;
        break;
      }

      if ((p[0] == '/') && (p[1] == '>'))
      {
        p += 2;
        selfClosing = true;
        break;
      }

      char* aName = p;

      while (isName(*p))
        ++p;

      char* aEnd = p;

      while (isSpace(*p))
        ++p;

      if ((aEnd == aName) || (*p != '='))
      {
        snprintf(why, whyLen, "element %s: an attribute that is not name=\"value\"", name);
        return NULL;
      }
      ++p;
      while (isSpace(*p))
        ++p;

      char quote = *p;

      if ((quote != '"') && (quote != '\''))
      {
        snprintf(why, whyLen, "element %s: an attribute value without quotes", name);
        return NULL;
      }

      char* value = ++p;
      char* vEnd  = strchr(value, quote);

      if (vEnd == NULL)
      {
        snprintf(why, whyLen, "element %s: an attribute value that does not end", name);
        return NULL;
      }

      *aEnd = 0;                                      // the '=' or a space after the name: passed already
      *vEnd = 0;
      p     = vEnd + 1;

      if ((strchr(value, '<') != NULL) || (unescape(value) == false))
      {
        snprintf(why, whyLen, "element %s, attribute %s: a '<' or a reference that is not known", name, aName);
        return NULL;
      }

      if (corTreeLookup(elementP, aName) != NULL)
      {
        snprintf(why, whyLen, "element %s: attribute %s twice", name, aName);
        return NULL;
      }

      corTreeChildAdd(elementP, corTreeString(kaP, aName, value));
    }

    if (depth == 0)
      rootP = elementP;
    else
    {
      CorNode* arrayP = childArray(kaP, elemV[depth - 1], name);

      if (arrayP == NULL)
      {
        snprintf(why, whyLen, "element %s: a child element and an attribute both named %s", nameV[depth - 1], name);
        return NULL;
      }

      corTreeChildAdd(arrayP, elementP);
    }

    if (selfClosing == false)
    {
      elemV[depth] = elementP;
      nameV[depth] = name;
      depth       += 1;
    }
  }

  if (depth != 0)
  {
    snprintf(why, whyLen, "element %s does not end", nameV[depth - 1]);
    return NULL;
  }

  if (rootP == NULL)
  {
    snprintf(why, whyLen, "no root element");
    return NULL;
  }

  return rootP;
}



// -----------------------------------------------------------------------------
//
// isoxmlAppend -
//
bool isoxmlAppend(char** bufP, int* lenP, int* sizeP, const char* text, int textLen)
{
  if (*lenP + textLen + 1 > *sizeP)
  {
    int   size = (*sizeP == 0) ? 1024 : *sizeP;
    char* newP;

    while (*lenP + textLen + 1 > size)
      size *= 2;

    if ((newP = realloc(*bufP, size)) == NULL)
      return false;

    *bufP  = newP;
    *sizeP = size;
  }

  memcpy(&(*bufP)[*lenP], text, textLen);
  *lenP += textLen;
  (*bufP)[*lenP] = 0;
  return true;
}



// -----------------------------------------------------------------------------
//
// attrAppend - ' name="value"', the value escaped
//
static bool attrAppend(char** bufP, int* lenP, int* sizeP, const char* name, const char* value)
{
  if ((isoxmlAppend(bufP, lenP, sizeP, " ", 1) == false) || (isoxmlAppend(bufP, lenP, sizeP, name, strlen(name)) == false) || (isoxmlAppend(bufP, lenP, sizeP, "=\"", 2) == false))
    return false;

  for (const char* s = value; *s != 0; s++)
  {
    const char* esc = NULL;

    switch (*s)
    {
    case '<':   esc = "&lt;";    break;
    case '>':   esc = "&gt;";    break;
    case '&':   esc = "&amp;";   break;
    case '"':   esc = "&quot;";  break;
    case '\n':  esc = "&#10;";   break;
    case '\r':  esc = "&#13;";   break;
    case '\t':  esc = "&#9;";    break;
    }

    if (isoxmlAppend(bufP, lenP, sizeP, (esc != NULL) ? esc : s, (esc != NULL) ? (int) strlen(esc) : 1) == false)
      return false;
  }

  return isoxmlAppend(bufP, lenP, sizeP, "\"", 1);
}



// -----------------------------------------------------------------------------
//
// nameOk - an XML name ISOXML uses
//
static bool nameOk(const char* name)
{
  if ((name == NULL) || (name[0] == 0) || ((name[0] >= '0') && (name[0] <= '9')) || (name[0] == '-') || (name[0] == '.'))
    return false;

  for (const char* s = name; *s != 0; s++)
  {
    if (isName(*s) == false)
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// isoxmlRender -
//
bool isoxmlRender(const char* tag, CorNode* objectP, char** bufP, int* lenP, int* sizeP, char* why, int whyLen)
{
  if (nameOk(tag) == false)
  {
    snprintf(why, whyLen, "'%s' is no element name", tag);
    return false;
  }

  if ((isoxmlAppend(bufP, lenP, sizeP, "<", 1) == false) || (isoxmlAppend(bufP, lenP, sizeP, tag, strlen(tag)) == false))
    goto oom;

  bool children = false;

  for (CorNode* memberP = objectP->value.head; memberP != NULL; memberP = memberP->next)
  {
    char        num[64];
    const char* value = NULL;

    if (memberP->name[0] == '@')
      continue;

    switch (memberP->type)
    {
    case CorString:   value = memberP->value.s;                                      break;
    case CorInt:      snprintf(num, sizeof(num), "%lld", memberP->value.i);  value = num;  break;
    case CorFloat:    snprintf(num, sizeof(num), "%.15g", memberP->value.f);  value = num;  break;
    case CorBoolean:  value = memberP->value.b ? "1" : "0";                           break;   // ISOXML's booleans
    case CorArray:    children = true;                                                continue;
    case CorObject:   children = true;                                                continue;   // one child element: NGSI-LD compacts an array of one
    default:
      snprintf(why, whyLen, "%s.%s: null is no attribute", tag, memberP->name);
      return false;
    }

    if (nameOk(memberP->name) == false)
    {
      snprintf(why, whyLen, "%s: '%s' is no attribute name", tag, memberP->name);
      return false;
    }

    if (attrAppend(bufP, lenP, sizeP, memberP->name, value) == false)
      goto oom;
  }

  if (children == false)
    return (isoxmlAppend(bufP, lenP, sizeP, "/>", 2) == true) ? true : (snprintf(why, whyLen, "out of memory"), false);

  if (isoxmlAppend(bufP, lenP, sizeP, ">", 1) == false)
    goto oom;

  for (CorNode* memberP = objectP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if (((memberP->type != CorArray) && (memberP->type != CorObject)) || (memberP->name[0] == '@'))
      continue;

    if (memberP->type == CorObject)
    {
      if (isoxmlRender(memberP->name, memberP, bufP, lenP, sizeP, why, whyLen) == false)
        return false;
      continue;
    }

    for (CorNode* itemP = memberP->value.head; itemP != NULL; itemP = itemP->next)
    {
      if (itemP->type != CorObject)
      {
        snprintf(why, whyLen, "%s.%s: a child element is an object", tag, memberP->name);
        return false;
      }

      if (isoxmlRender(memberP->name, itemP, bufP, lenP, sizeP, why, whyLen) == false)
        return false;
    }
  }

  if ((isoxmlAppend(bufP, lenP, sizeP, "</", 2) == false) || (isoxmlAppend(bufP, lenP, sizeP, tag, strlen(tag)) == false) || (isoxmlAppend(bufP, lenP, sizeP, ">", 1) == false))
    goto oom;

  return true;

oom:
  snprintf(why, whyLen, "out of memory");
  return false;
}
