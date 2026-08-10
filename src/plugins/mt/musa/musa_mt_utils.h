/* SPDX-FileCopyrightText: Copyright (c) 2025-2026 Moore Threads / Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#ifndef __MUSA_MT_UTILS_H
#define __MUSA_MT_UTILS_H

#include <fcntl.h>
#include <unistd.h>
#include <nixl.h>
// muFile is API-identical to cuFile. The MUSA SDK ships a cuFile-compatible
// header named <cufile.h>; if only the native <musa_cufile.h> is present, the
// shim below remaps the names. Either way the call sites stay source-identical
// to the upstream GDS_MT / ROCm AIS_MT plugins.
#include "musafile_compat.h"

class musaMtFileHandle {
public:
    musaMtFileHandle(int fd);
    ~musaMtFileHandle();

    musaMtFileHandle(const musaMtFileHandle &) = delete;
    musaMtFileHandle &
    operator=(const musaMtFileHandle &) = delete;
    musaMtFileHandle(musaMtFileHandle &&) = delete;
    musaMtFileHandle &
    operator=(musaMtFileHandle &&) = delete;

    int fd{-1};
    CUfileHandle_t cu_fhandle{nullptr};
};

class musaMtMemBuf {
public:
    musaMtMemBuf(void *ptr, size_t sz, int flags = 0);
    ~musaMtMemBuf();

    musaMtMemBuf(const musaMtMemBuf &) = delete;
    musaMtMemBuf &
    operator=(const musaMtMemBuf &) = delete;
    musaMtMemBuf(musaMtMemBuf &&) = delete;
    musaMtMemBuf &
    operator=(musaMtMemBuf &&) = delete;

private:
    void *base_{nullptr};
    bool registered_{false};
};

class musaMtUtil {
public:
    musaMtUtil();
    ~musaMtUtil();
};
#endif
