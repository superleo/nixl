/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 Contributors. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FILE_MT_ENGINE_BASE_HPP
#define FILE_MT_ENGINE_BASE_HPP

#include <nixl.h>
#include <nixl_types.h>
#include <backend/backend_engine.h>
#include <string>

/** Shared trivial nixlBackendEngine overrides for multi-threaded file backends.
 *  Vendor-agnostic: identical to the ROCm AIS_MT base, reused by MUSA_MT. */
template<typename Derived>
class FileMtEngineBase : public nixlBackendEngine {
public:
    explicit FileMtEngineBase(const nixlBackendInitParams *init_params)
        : nixlBackendEngine(init_params) {}

    bool
    supportsNotif() const override {
        return false;
    }

    bool
    supportsRemote() const override {
        return false;
    }

    bool
    supportsLocal() const override {
        return true;
    }

    nixl_mem_list_t
    getSupportedMems() const override {
        return {DRAM_SEG, VRAM_SEG, FILE_SEG};
    }

    nixl_status_t
    connect(const std::string &remote_agent) override {
        (void)remote_agent;
        return NIXL_SUCCESS;
    }

    nixl_status_t
    disconnect(const std::string &remote_agent) override {
        (void)remote_agent;
        return NIXL_SUCCESS;
    }

    nixl_status_t
    loadLocalMD(nixlBackendMD *input, nixlBackendMD *&output) override {
        output = input;
        return NIXL_SUCCESS;
    }

    nixl_status_t
    unloadMD(nixlBackendMD *input) override {
        (void)input;
        return NIXL_SUCCESS;
    }
};

#endif
