/* SPDX-FileCopyrightText: Copyright (c) 2025-2026 Moore Threads / Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include "common/nixl_log.h"
#include "musa_mt_utils.h"

namespace {
bool
musaMtCompatModeAllowed() {
    const char *v = std::getenv("MUFILE_ALLOW_COMPAT_MODE");
    if (v == nullptr || v[0] == '\0') {
        return false;
    }
    return std::strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0;
}
} // namespace

musaMtUtil::musaMtUtil() {
    const CUfileError_t status = cuFileDriverOpen();
    if (status.err != CU_FILE_SUCCESS) {
        throw std::runtime_error(
            "MUSA_MT: error initializing MUSA Infinity Storage (muFile) driver: error=" +
            std::to_string(status.err));
    }
}

musaMtUtil::~musaMtUtil() {
    (void)cuFileDriverClose();
}

musaMtMemBuf::musaMtMemBuf(void *ptr, size_t sz, int flags) : base_(ptr) {
    const CUfileError_t status = cuFileBufRegister(ptr, sz, flags);
    if (status.err != CU_FILE_SUCCESS) {
        if (musaMtCompatModeAllowed()) {
            NIXL_WARN << "MUSA_MT: buffer registration failed - compat mode: err="
                      << status.err;
            return;
        }
        throw std::runtime_error(
            "MUSA_MT: cuFileBufRegister failed (err=" + std::to_string(status.err) +
            "); set MUFILE_ALLOW_COMPAT_MODE=true to allow fallback");
    }
    registered_ = true;
}

musaMtMemBuf::~musaMtMemBuf() {
    if (registered_) {
        const CUfileError_t status = cuFileBufDeregister(base_);
        if (status.err != CU_FILE_SUCCESS) {
            NIXL_WARN << "MUSA_MT: warning: deregistering buffer: error=" << status.err
                      << " ptr=" << base_;
        }
    }
}

musaMtFileHandle::musaMtFileHandle(int file_fd) : fd(file_fd) {
    CUfileDescr_t descr = {};
    descr.handle.fd = fd;
    descr.type = CU_FILE_HANDLE_TYPE_OPAQUE_FD;

    const CUfileError_t status = cuFileHandleRegister(&cu_fhandle, &descr);
    if (status.err != CU_FILE_SUCCESS) {
        throw std::runtime_error("MUSA_MT: file register error: error=" +
                                 std::to_string(status.err) + ", fd=" + std::to_string(fd));
    }
}

musaMtFileHandle::~musaMtFileHandle() {
    (void)cuFileHandleDeregister(cu_fhandle);
}
