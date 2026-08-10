/* SPDX-FileCopyrightText: Copyright (c) 2025-2026 Moore Threads / Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include <nixl.h>
#include <nixl_types.h>
#include <backend/backend_engine.h>
#include <thread>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <exception>
#include <cstring>
#include <variant>
#include <future>
#include <atomic>
#include "common/nixl_log.h"
#include "musa_mt_backend.h"
#include "musa_mt_utils.h"
#include "musafile_compat.h"
#include "file/file_utils.h"
#include <taskflow/taskflow.hpp>
#include <unordered_map>
#include <unordered_set>

namespace {
const size_t default_thread_count = std::max(1u, std::thread::hardware_concurrency() / 2);

struct FileSegData {
    std::shared_ptr<musaMtFileHandle> handle;

    FileSegData(std::shared_ptr<musaMtFileHandle> h) : handle(std::move(h)) {}
};

struct MemSegData {
    std::unique_ptr<musaMtMemBuf> buf;

    MemSegData(void *addr, size_t size, int flags)
        : buf(std::make_unique<musaMtMemBuf>(addr, size, flags)) {}
};

struct MusaMtTransferRequestH {
    MusaMtTransferRequestH(void *a,
                           size_t s,
                           size_t offset,
                           CUfileHandle_t handle,
                           CUfileOpcode_t operation,
                           int device_id)
        : addr{a},
          size{s},
          file_offset{offset},
          fh{handle},
          op{operation},
          dev_id{device_id} {}

    void *addr;
    size_t size;
    size_t file_offset;
    CUfileHandle_t fh;
    CUfileOpcode_t op;
    int dev_id;
};

class nixlMusaMtMetadata : public nixlBackendMD {
public:
    explicit nixlMusaMtMetadata(std::shared_ptr<musaMtFileHandle> file_handle)
        : nixlBackendMD(true),
          data_(FileSegData{std::move(file_handle)}) {}

    explicit nixlMusaMtMetadata(void *addr, size_t size, int flags)
        : nixlBackendMD(true),
          data_(MemSegData{addr, size, flags}) {}

    ~nixlMusaMtMetadata() = default;

    nixlMusaMtMetadata(const nixlMusaMtMetadata &) = delete;
    nixlMusaMtMetadata &
    operator=(const nixlMusaMtMetadata &) = delete;

    nixlMusaMtMetadata(nixlMusaMtMetadata &&) = default;
    nixlMusaMtMetadata &
    operator=(nixlMusaMtMetadata &&) = default;

    std::variant<FileSegData, MemSegData> data_;
};

class nixlMusaMtBackendReqH : public nixlBackendReqH {
public:
    ~nixlMusaMtBackendReqH();

    std::vector<MusaMtTransferRequestH> request_list;
    tf::Taskflow taskflow;
    std::future<void> running_transfer;
    std::atomic<nixl_status_t> overall_status;
};

size_t
getThreadCount(const nixlBackendInitParams *init_params) {
    size_t thread_count = default_thread_count;

    nixl_b_params_t *custom_params = init_params->customParams;
    if (custom_params) {
        if (custom_params->count("thread_count") > 0) {
            try {
                size_t tcount = std::stoul((*custom_params)["thread_count"]);
                if (tcount != 0) {
                    thread_count = tcount;
                }
            }
            catch (const std::exception &e) {
                throw std::runtime_error("MUSA_MT: invalid thread_count parameter: " +
                                         std::string(e.what()));
            }
        }
    }
    return thread_count;
}

void
runMuFileOp(MusaMtTransferRequestH *req, std::atomic<nixl_status_t> *overall_status) {
    if (req->dev_id >= 0) {
        const cudaError_t dev_err = cudaSetDevice(req->dev_id);
        if (dev_err != cudaSuccess) {
            NIXL_ERROR << "MUSA_MT: cudaSetDevice failed: " << cudaGetErrorString(dev_err);
            overall_status->store(NIXL_ERR_BACKEND);
            return;
        }
    }

    ssize_t nbytes = 0;
    if (req->op == CUFILE_READ) {
        nbytes = cuFileRead(req->fh, req->addr, req->size, req->file_offset, 0);
        if (nbytes < 0) {
            NIXL_ERROR << "MUSA_MT: cuFileRead failed: " << strerror(errno);
            overall_status->store(NIXL_ERR_BACKEND);
            return;
        }
    } else if (req->op == CUFILE_WRITE) {
        nbytes = cuFileWrite(req->fh, req->addr, req->size, req->file_offset, 0);
        if (nbytes < 0) {
            NIXL_ERROR << "MUSA_MT: cuFileWrite failed: " << strerror(errno);
            overall_status->store(NIXL_ERR_BACKEND);
            return;
        }
    } else {
        overall_status->store(NIXL_ERR_INVALID_PARAM);
        return;
    }

    if ((size_t)nbytes != req->size) {
        NIXL_ERROR << "MUSA_MT: error: short "
                   << ((req->op == CUFILE_READ) ? "read: " : "write: ") << nbytes << " out of "
                   << req->size << " bytes - address=" << req->addr;
        overall_status->store(NIXL_ERR_BACKEND);
        return;
    }
}

nixl_status_t
extractTransferParams(
    const nixlMetaDesc &mem_desc,
    const nixlMetaDesc &file_desc,
    const std::unordered_map<int, std::weak_ptr<musaMtFileHandle>> &file_map,
    void *&base_addr,
    size_t &total_size,
    size_t &base_offset,
    CUfileHandle_t &mu_fhandle) {
    base_addr = (void *)mem_desc.addr;
    total_size = mem_desc.len;
    base_offset = (size_t)file_desc.addr;

    auto it = file_map.find(file_desc.devId);
    if (it == file_map.end()) {
        NIXL_ERROR << "MUSA_MT: error: file metadata not found";
        return NIXL_ERR_NOT_FOUND;
    }

    auto handle = it->second.lock();
    NIXL_ASSERT(handle);
    mu_fhandle = handle->cu_fhandle;
    return NIXL_SUCCESS;
}
} // namespace

nixlMusaMtBackendReqH::~nixlMusaMtBackendReqH() {
    if (running_transfer.valid()) {
        running_transfer.wait();
    }
}

nixlMusaMtEngine::nixlMusaMtEngine(const nixlBackendInitParams *init_params)
    : FileMtEngineBase<nixlMusaMtEngine>(init_params),
      musa_mt_utils_(),
      thread_count_(getThreadCount(init_params)),
      executor_(std::make_unique<tf::Executor>(thread_count_)) {
    NIXL_DEBUG << "MUSA_MT: thread count=" << thread_count_;
}

nixl_status_t
nixlMusaMtEngine::registerMem(const nixlBlobDesc &mem,
                              const nixl_mem_t &nixl_mem,
                              nixlBackendMD *&out) {
    switch (nixl_mem) {
    case FILE_SEG: {
        auto it = musa_mt_file_map_.find(mem.devId);
        std::shared_ptr<musaMtFileHandle> handle;
        if (it != musa_mt_file_map_.end()) {
            handle = it->second.lock();
            if (handle) {
                out = new nixlMusaMtMetadata(handle);
                return NIXL_SUCCESS;
            }
            musa_mt_file_map_.erase(it);
        }

        try {
            handle = std::make_shared<musaMtFileHandle>(mem.devId);
        }
        catch (const std::exception &e) {
            NIXL_ERROR << "MUSA_MT: failed to create file handle: " << e.what();
            return NIXL_ERR_BACKEND;
        }
        musa_mt_file_map_[mem.devId] = handle;
        out = new nixlMusaMtMetadata(handle);
        return NIXL_SUCCESS;
    }

    case VRAM_SEG: {
        const cudaError_t error_id = cudaSetDevice(mem.devId);
        if (error_id != cudaSuccess) {
            NIXL_ERROR << "MUSA_MT: error: cudaSetDevice returned "
                       << cudaGetErrorString(error_id) << " for device ID " << mem.devId;
            return NIXL_ERR_BACKEND;
        }
        [[fallthrough]];
    }

    case DRAM_SEG: {
        try {
            out = new nixlMusaMtMetadata((void *)mem.addr, mem.len, 0);
            return NIXL_SUCCESS;
        }
        catch (const std::exception &e) {
            NIXL_ERROR << "MUSA_MT: failed to create memory buffer: " << e.what();
            return NIXL_ERR_BACKEND;
        }
    }

    default:
        return NIXL_ERR_BACKEND;
    }
}

nixl_status_t
nixlMusaMtEngine::deregisterMem(nixlBackendMD *meta) {
    std::unique_ptr<nixlMusaMtMetadata> md((nixlMusaMtMetadata *)meta);

    if (auto *file_data = std::get_if<FileSegData>(&md->data_)) {
        if (file_data->handle) {
            int key = file_data->handle->fd;
            md.reset();

            auto it = musa_mt_file_map_.find(key);
            if (it != musa_mt_file_map_.end() && it->second.expired()) {
                musa_mt_file_map_.erase(it);
            }
        }
    }

    return NIXL_SUCCESS;
}

nixl_status_t
nixlMusaMtEngine::prepXfer(const nixl_xfer_op_t &operation,
                           const nixl_meta_dlist_t &local,
                           const nixl_meta_dlist_t &remote,
                           const std::string &remote_agent,
                           nixlBackendReqH *&handle,
                           const nixl_opt_b_args_t *opt_args) const {
    auto musa_mt_handle = std::make_unique<nixlMusaMtBackendReqH>();
    size_t buf_cnt = local.descCount();
    size_t file_cnt = remote.descCount();

    if ((buf_cnt != file_cnt) || ((operation != NIXL_READ) && (operation != NIXL_WRITE))) {
        NIXL_ERROR << "MUSA_MT: error: incorrect count or operation selection";
        return NIXL_ERR_INVALID_PARAM;
    }

    if ((remote.getType() != FILE_SEG) && (local.getType() != FILE_SEG)) {
        NIXL_ERROR << "MUSA_MT: error: backend only supports I/O between memory "
                      "(DRAM/VRAM_SEG) and "
                      "files (FILE_SEG)";
        return NIXL_ERR_INVALID_PARAM;
    }

    musa_mt_handle->request_list.clear();
    bool is_local_file = (local.getType() == FILE_SEG);
    for (size_t i = 0; i < buf_cnt; i++) {
        void *base_addr;
        size_t total_size;
        size_t base_offset;
        CUfileHandle_t mu_fhandle;

        nixl_status_t param_status;
        if (is_local_file) {
            param_status = extractTransferParams(remote[i],
                                                 local[i],
                                                 musa_mt_file_map_,
                                                 base_addr,
                                                 total_size,
                                                 base_offset,
                                                 mu_fhandle);
        } else {
            param_status = extractTransferParams(local[i],
                                                 remote[i],
                                                 musa_mt_file_map_,
                                                 base_addr,
                                                 total_size,
                                                 base_offset,
                                                 mu_fhandle);
        }

        if (param_status != NIXL_SUCCESS) {
            return param_status;
        }

        const int dev_id = is_local_file ? remote[i].devId : local[i].devId;

        musa_mt_handle->request_list.emplace_back(
            base_addr,
            total_size,
            base_offset,
            mu_fhandle,
            (operation == NIXL_READ) ? CUFILE_READ : CUFILE_WRITE,
            dev_id);
    }

    if (musa_mt_handle->request_list.empty()) {
        return NIXL_ERR_INVALID_PARAM;
    }
    musa_mt_handle->taskflow.emplace(
        [reqs = &musa_mt_handle->request_list,
         overall_status = &musa_mt_handle->overall_status]() {
            for (MusaMtTransferRequestH &req : *reqs) {
                if (overall_status->load() != NIXL_SUCCESS) {
                    return;
                }
                runMuFileOp(&req, overall_status);
            }
        });

    handle = musa_mt_handle.release();
    return NIXL_SUCCESS;
}

nixl_status_t
nixlMusaMtEngine::postXfer(const nixl_xfer_op_t &operation,
                           const nixl_meta_dlist_t &local,
                           const nixl_meta_dlist_t &remote,
                           const std::string &remote_agent,
                           nixlBackendReqH *&handle,
                           const nixl_opt_b_args_t *opt_args) const {
    nixlMusaMtBackendReqH *musa_mt_handle = (nixlMusaMtBackendReqH *)handle;

    musa_mt_handle->overall_status.store(NIXL_SUCCESS);
    musa_mt_handle->running_transfer = executor_->run(musa_mt_handle->taskflow);
    return NIXL_IN_PROG;
}

nixl_status_t
nixlMusaMtEngine::checkXfer(nixlBackendReqH *handle) const {
    nixlMusaMtBackendReqH *musa_mt_handle = (nixlMusaMtBackendReqH *)handle;
    if (musa_mt_handle->running_transfer.wait_for(nixlTime::seconds(0)) !=
        std::future_status::ready) {
        return NIXL_IN_PROG;
    }
    musa_mt_handle->running_transfer.get();

    std::unordered_set<int> devices;
    for (const MusaMtTransferRequestH &req : musa_mt_handle->request_list) {
        if (req.dev_id >= 0) {
            devices.insert(req.dev_id);
        }
    }
    for (int dev_id : devices) {
        const cudaError_t dev_err = cudaSetDevice(dev_id);
        if (dev_err != cudaSuccess) {
            NIXL_ERROR << "MUSA_MT: cudaSetDevice failed during sync: "
                       << cudaGetErrorString(dev_err);
            return NIXL_ERR_BACKEND;
        }
    }

    return musa_mt_handle->overall_status.load();
}

nixl_status_t
nixlMusaMtEngine::releaseReqH(nixlBackendReqH *handle) const {
    std::unique_ptr<nixlMusaMtBackendReqH> musa_mt_handle(
        (nixlMusaMtBackendReqH *)handle);
    return NIXL_SUCCESS;
}

nixl_status_t
nixlMusaMtEngine::queryMem(const nixl_reg_dlist_t &descs,
                           std::vector<nixl_query_resp_t> &resp) const {
    std::vector<nixl_blob_t> metadata(descs.descCount());
    for (int i = 0; i < descs.descCount(); ++i) {
        metadata[i] = descs[i].metaInfo;
    }

    return nixl::queryFileInfoList(metadata, resp);
}