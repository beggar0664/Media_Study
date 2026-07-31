# live555 minimal RTSP examples

This directory contains isolated live555 console examples for RTSP study.

The code uses the existing local dependencies:

- Live555: `E:\code\QT_PRJ\VMS_TOOL\3rdParty\Live555`
- OpenSSL: `E:\code\QT_PRJ\VMS_TOOL\3rdParty\OpenSSL\1.1.1l`

## Targets

- `rtsp_push_server`: serve an H.264 Annex-B elementary stream as an RTSP URL.
- `rtsp_pull_client`: run OPTIONS/DESCRIBE/SETUP/PLAY and print received RTP payload info.

## Build

```powershell
cmd /c '"E:\tool\Visual Studio\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 && cmake -S E:\code\Media\MediaProtrocl\RTSP\live555_minimal -B E:\code\Media\MediaProtrocl\RTSP\live555_minimal\build_nmake -G "NMake Makefiles" && cmake --build E:\code\Media\MediaProtrocl\RTSP\live555_minimal\build_nmake'
```

## Run

The server needs an H.264 Annex-B elementary stream file:

```powershell
E:\code\Media\MediaProtrocl\RTSP\live555_minimal\out\buildBin\rtsp_push_server.exe E:\path\to\test.h264 test
```

Connect with the client:

```powershell
E:\code\Media\MediaProtrocl\RTSP\live555_minimal\out\buildBin\rtsp_pull_client.exe rtsp://127.0.0.1:8554/test
```

Wireshark display filter:

```text
rtsp || rtp || udp
```
