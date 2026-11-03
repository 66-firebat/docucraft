# ncnn (optional, for OCR)

The core links prebuilt ncnn 20261020 libraries from this directory when they
are present. They are not in the repository. Download them from
https://github.com/Tencent/ncnn/releases/tag/20261020 and unpack so that each
target has `include/ncnn/net.h` and `lib/libncnn.a` (or `lib/libncnn.so.1`):

| Directory | Archive |
|---|---|
| `linux-x86_64/` | `ncnn-20261020-ubuntu-2204-shared.zip` |
| `arm64-v8a/` | the `arm64-v8a` folder of `ncnn-20261020-android.zip` |
| `mingw-x86_64/` | no official archive; build ncnn with MinGW |

Without them the core builds without OCR. ncnn is BSD 3-Clause licensed
(`LICENSE.txt`); its source is at https://github.com/Tencent/ncnn/tree/20261020.
