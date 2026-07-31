# GB28181 Minimal Module Study

This page is the module-level entry for the minimal GB28181 code.

Main study guide:

- [../gb28181_study.md](../gb28181_study.md)

Module files:

- `gb28181_module.h`
- `gb28181_module.cpp`
- `gb28181_minimal_example.cpp`

Build:

```powershell
cd E:\code\Media\MediaProtrocl\GB28181
cmake -S . -B build
cmake --build build
```

Current module goals:

- learn SIP message structure
- learn SDP media negotiation fields
- learn RTP payload sending with `jrtplib`
- keep the code small enough to inspect field by field

