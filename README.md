# Media Study

这是一个媒体封装与传输协议的学习仓库，重点放在容器层和协议层的拆解、字节级定位和代码对照。

## 目录

### `Container`

容器层总纲与样本，关注 PS / TS / FLV 这类封装格式本身。

- `[container_layer_study.md](Container/container_layer_study.md)`：容器分层总纲
- `[ts/ts_study.md](Container/ts/ts_study.md)`：TS 结构与样本分析
- `[ts/ts-complete-reference.md](Container/ts/ts-complete-reference.md)`：TS 完整参考

### `MediaProtrocl`

媒体协议层资料，当前主要是 FLV / RTMP。

- `[flv/flv_study.md](MediaProtrocl/flv/flv_study.md)`：FLV 结构学习笔记
- `[flv/flv_code_study.md](MediaProtrocl/flv/flv_code_study.md)`：FLV 代码摘录
- `[rtmp/rtmp_study.md](MediaProtrocl/rtmp/rtmp_study.md)`：RTMP 模块学习笔记

## 推荐阅读顺序

1. 先读 `Container/container_layer_study.md`，建立容器层和编解码层的边界。
2. 再读 `Container/ts/ts_study.md`，看 TS 的实际字节结构和排障过程。
3. 然后读 `MediaProtrocl/flv/flv_study.md` 和 `MediaProtrocl/flv/flv_code_study.md`，对照 FLV 结构与代码。
4. 最后读 `MediaProtrocl/rtmp/rtmp_study.md`，把 RTMP、librtmp 和 FLV dump 路径串起来。

## 资料定位

- `Container`：容器格式学习。
- `MediaProtrocl`：媒体协议与推流封装学习。
- 样本文件保留在对应目录下，用于 WinHex 和代码对照。

## 备注

仓库里有一些大型工程目录和编译产物已经被 `.gitignore` 排除，不作为本仓库的学习主体。
