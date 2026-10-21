# corIotaIsoxmlBridge

**Prototype - not released.** The FIWARE IoT Agent for ISOXML (ISO 11783-10, agricultural task data) in the
coraine NGSI-LD broker: the task data a **MICS** (Mobile Implement Control System) POSTs - `TASKDATA.XML` -
each element a **DataChannel record**, and a written attribute as task data POSTed back to the MICS -
`iotaisoxml.so`, a bridge plugin loaded with `--bridges iotaisoxml`. Task data iotagent-isoxml takes it takes
the same way, with the same answers.

## Task data in

    POST /iot/isoxml   Content-Type: application/xml

    <ISO11783_TaskData>
      <FRM A="FRM3" B="Animal Farm" F="City2" I="CTR2"/>
      <CTR A="CTR2" B="Napoleon the Pig"/>
      <TSK A="TSK11" B="mass-based product allocation">
        <PAN A="PDT2" C="20000" E="DET3"><ASP A="2019-11-12T08:00:00" D="4"/></PAN>
      </TSK>
    </ISO11783_TaskData>

On the southbound port - served by the broker for the plugin (bridge ABI 16, `httpListen`), plain HTTP, no
NGSI-LD and no authentication. Each element under `ISO11783_TaskData` - an entity in iotagent-isoxml - is a
record on the DataChannel of its **tag**: a DataChannel's endpoint is a tag (`CTR`, `FRM`, `TSK`, `PDT` ...),
or `*` for every element no DataChannel of its tag has.

- **The record**: the element's attributes as columns of their ISO 11783-10 names (`A`, `B`, `C` ...), its
  child elements as an array of objects under their tag (`PAN`: `[ { "A": "PDT2", ..., "ASP": [ ... ] } ]`),
  `@deviceId` its id (`A`), `@element` its tag. A value is a string, as in the XML - the attribute's `as`
  makes it a number. An array of one child element is that element's object in the entity: NGSI-LD compacts
  an array of one to its item.
- Which attribute is which is the DataChannel's mapping - the column names are the schema's letters
  (`CTR`: B the last name, C the first name; `TSK`: B the designator, G the status ...).
- **The answers** are iotagent-isoxml's: 200, no body, when every element was taken. An element no DataChannel
  takes is `DEVICE_NOT_FOUND`: alone, that error (404); none taken 400, some 202 - the errors in an array
  `[{"name", "message"}]`. An element without its id - `400 MANDATORY_PARAMS_NOT_FOUND`, nothing taken; XML
  that is not task data - `400 PARSE_ERROR`; a Content-Type that is not XML - `400 UNSUPPORTED_TYPE`. A body
  over 1 MB 413, a method other than POST 405, another path 404.
- The XML taken: the declaration, comments, a DOCTYPE without an internal subset, CDATA (skipped), the five
  named entities and character references. Text inside elements is skipped - ISOXML has none.

## Task data out - write-back to the MICS

With `"channelDirection": "both"` (coraine's DataChannel write-back, `"idEncoding": "percent"`) and a `mics`
configured, a client's write to a mapped attribute is task data POSTed to `mics.url`, `application/xml`:

    <?xml version="1.0" encoding="UTF-8"?>
    <ISO11783_TaskData VersionMajor="4" VersionMinor="3" DataTransferOrigin="1"><TSK A="TSK11" G="4"/></ISO11783_TaskData>

- **The element**: the DataChannel's tag - on `*`, the fragment's `@element` (an id template with
  `{@element}`); its id (`A`) the fragment's `@deviceId` (an id template with `{@deviceId}`); its attributes
  and child elements the columns written. A number is its text, a boolean 1 or 0, an array of objects - or
  one object - child elements.
- Refused, nothing stored: a value that is neither (null, an array of strings), no tag or no id - 400; no
  `mics` configured - 422; 256 writes waiting for the MICS - 503.
- The write is queued and the client answered at once; a thread of the plugin's POSTs the task data, one at a
  time. A MICS that refuses it, or is not reached, is logged: the write was stored.

iotagent-isoxml's `send` command (a task and the entities it names, compiled into one TASKDATA) is not built:
a write is one element.

## Configuration

```json
{
  "iotaisoxml": {
    "southbound": { "port": 7896, "path": "/iot/isoxml" },
    "mics": { "url": "https://mics.farm.example/taskdata", "authorizationEnv": "MICS_AUTHORIZATION" },
    "ngsild": {
      "defaultEntity": false,
      "dataChannels": {
        "CTR": { "entities": [ { "type": "Person", "id": "urn:ngsi-ld:Person:{@deviceId}",
                                 "attributes": { "familyName": { "column": "B" }, "givenName": { "column": "C" } } } ] },
        "TSK": { "channelDirection": "both", "idEncoding": "percent",
                 "entities": [ { "type": "Task", "id": "urn:ngsi-ld:Task:{@deviceId}",
                                 "attributes": { "name": { "column": "B" }, "status": { "column": "G" },
                                                 "allocations": { "column": "PAN" } } } ] }
      }
    }
  }
}
```

| member | |
|---|---|
| `southbound.port` | the southbound port - 7896, iotagent-isoxml's, when not said |
| `southbound.path` | the resource task data is POSTed to - `/iot/isoxml`, iotagent-isoxml's |
| `mics.url` | where written attributes go: `http[s]://<host>[:<port>][/<path>]`. No `mics`: no write-back |
| `mics.authorizationEnv` | the environment variable that holds the `Authorization` header's value - never in the file |
| `mics.caFile`, `mics.insecure` | https: the CAs the MICS's certificate is verified against (default: the system's); `true`: not verified |

A DataChannel of this bridge has no channelInfo. A Channel or an AssetChannel configured for this bridge is
refused (dormant): it carries DataChannels only.

## Not built

The provisioning API (a DataChannel comes from the configuration file), iotagent-isoxml's named attributes
(its Smart Data Models mapping - here the DataChannel's), the `send` command, the binary TimeLog files
(`TLG*.BIN`) a TASKDATA refers to, ISOXML zipped, MQTT.

## Build and test

    make && make test          # libssl-dev; the tests run python3 and curl

`test/funcTests/micsMock.py` plays the MICS: the task data POSTed to it logged, one refused.
