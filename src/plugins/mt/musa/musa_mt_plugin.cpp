/* SPDX-FileCopyrightText: Copyright (c) 2025-2026 Moore Threads / Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include "backend/backend_plugin.h"
#include "musa_mt_backend.h"

using musa_mt_plugin_t = nixlBackendPluginCreator<nixlMusaMtEngine>;

#ifdef STATIC_PLUGIN_MUSA_MT
nixlBackendPlugin *
createStaticMUSA_MTPlugin() {
    return musa_mt_plugin_t::create(
        NIXL_PLUGIN_API_VERSION, "MUSA_MT", "0.1.0", {}, {DRAM_SEG, VRAM_SEG, FILE_SEG});
}
#else
extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    return musa_mt_plugin_t::create(
        NIXL_PLUGIN_API_VERSION, "MUSA_MT", "0.1.0", {}, {DRAM_SEG, VRAM_SEG, FILE_SEG});
}

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {}
#endif