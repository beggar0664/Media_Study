# GB28181 Minimal Module

This directory contains a minimal GB28181 learning module.

Main study guide:

- [../gb28181_study.md](../gb28181_study.md)

## What it covers

- `REGISTER` text generation
- `INVITE + SDP` text generation
- `BYE` text generation
- `MESSAGE` Keepalive, Catalog, DeviceInfo and DeviceStatus XML generation
- Basic SIP response parsing
- Basic Digest challenge parsing and Authorization building
- RTP sending through `jrtplib`
- Minimal SIP UDP register client and mock server examples

## Files

- `gb28181_module.h`: public C interface
- `gb28181_module.cpp`: internal C++ implementation using `jrtplib`
- `gb28181_minimal_example.cpp`: console demo entry
- `gb28181_sip_register_client.cpp`: minimal SIP UDP/Digest demo
- `gb28181_sip_mock_server.cpp`: minimal SIP mock server for REGISTER 401/200 flow
- `CMakeLists.txt`: build entry

## Build

```powershell
cd E:\code\Media\MediaProtrocl\GB28181
cmake -S . -B build
cmake --build build
```

## Notes

This module is for study. It does not yet implement a full SIP state machine, full Catalog response list parsing, or full H.264/H.265 RTP packetization.

## Mock server usage

Run the mock server first:

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_sip_mock_server.exe
```

Then run the client:

```powershell
E:\code\Media\MediaProtrocl\GB28181\out\buildBin\gb28181_sip_register_client.exe
```

The flow is:

```text
REGISTER
401 Unauthorized
REGISTER + Authorization
200 OK
MESSAGE Keepalive
200 OK
MESSAGE Catalog
200 OK
Catalog Response MESSAGE
200 OK
MESSAGE DeviceInfo
200 OK
DeviceInfo Response MESSAGE
200 OK
MESSAGE DeviceStatus
200 OK
DeviceStatus Response MESSAGE
200 OK
INVITE + SDP
200 OK + SDP
ACK
BYE
200 OK
```
