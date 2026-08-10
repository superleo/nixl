/* SPDX-FileCopyrightText: Copyright (c) 2025-2026 Moore Threads / Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#ifndef __MUSA_MT_BACKEND_H
#define __MUSA_MT_BACKEND_H

#include <nixl.h>
#include <nixl_types.h>
#include <backend/backend_engine.h>
#include <memory>
#include <string>
#include <unordered_map>
#include "file_mt_engine_base.hpp"
#include "musa_mt_utils.h"
#include "taskflow/core/executor.hpp"

class nixlMusaMtEngine : public FileMtEngineBase<nixlMusaMtEngine> {
public:
    nixlMusaMtEngine(const nixlBackendInitParams *init_params);
    ~nixlMusaMtEngine() = default;

    nixlMusaMtEngine(const nixlMusaMtEngine &) = delete;
    nixlMusaMtEngine &
    operator=(const nixlMusaMtEngine &) = delete;

    nixl_status_t
    registerMem(const nixlBlobDesc &mem, const nixl_mem_t &nixl_mem, nixlBackendMD *&out) override;
    nixl_status_t
    deregisterMem(nixlBackendMD *meta) override;

    nixl_status_t
    prepXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    postXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    checkXfer(nixlBackendReqH *handle) const override;
    nixl_status_t
    releaseReqH(nixlBackendReqH *handle) const override;

    nixl_status_t
    queryMem(const nixl_reg_dlist_t &descs, std::vector<nixl_query_resp_t> &resp) const override;

private:
    musaMtUtil musa_mt_utils_;
    std::unordered_map<int, std::weak_ptr<musaMtFileHandle>> musa_mt_file_map_;
    size_t thread_count_;
    std::unique_ptr<tf::Executor> executor_;
};
#endif