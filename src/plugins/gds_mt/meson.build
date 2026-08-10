# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

gds_path = get_option('gds_path')

# --- Select the file-storage backend dependency for GDS_MT -------------------
# On MUSA (Moore Threads), muFile is API-identical to cuFile and libmusa stands
# in for libcudart. Rather than fork the GDS_MT source, we keep gds_mt_*.cpp
# unchanged and pick the linkage here:
#   * if -Dgds_use_musa=true, or CUDA is absent but MUSA is present  -> muFile
#   * otherwise                                                       -> cuFile
# A mufile_compat.h compatibility shim (see the -include below, created by this
# patch) translates the few remaining symbol/return-model differences so that
# the cuFile-authored sources compile and link against muFile untouched.
gds_use_musa = get_option('gds_use_musa')
if not gds_use_musa and not cuda_dep.found() and musa_dep.found()
    gds_use_musa = true
    message('GDS_MT: CUDA not found, MUSA present -- auto-selecting muFile backend')
endif

if gds_use_musa
    if not musa_dep.found()
        error('GDS_MT plugin requested muFile backend but MUSA dependency not found (set -Dmusa_path=)')
    endif
    # muFile ships alongside libmusa and reuses the cuFile-compatible header
    # layout under the MUSA include dir.
    mufile_lib_path = musa_home + '/lib'
    cufile_dep = declare_dependency(
        link_args: ['-L' + mufile_lib_path, '-lmufile', '-Wl,-rpath,' + mufile_lib_path],
        include_directories: include_directories(musa_home + '/include'))
    # cuda_dep is needed by the plugin's cudaSetDevice/cudaGetErrorString calls.
    # On MUSA the runtime provides those symbols via libmusa; alias cuda_dep.
    gds_runtime_dep = musa_dep
    # Inject the muFile<->cuFile translation shim into every TU in this plugin.
    # Use the absolute path so -include resolves regardless of include dirs.
    gds_compat_include = ['-include', meson.current_source_dir() / 'mufile_compat.h']
    gds_plugin_name = 'GDS_MT'
else
    if gds_path != ''
        gds_lib_path = gds_path + '/lib64'
        gds_inc_path = gds_path + '/include'
        cufile_dep = declare_dependency(
            link_args: ['-L' + gds_lib_path, '-lcufile'],
            include_directories: include_directories(gds_inc_path))
    else
        # Create a dummy dependency that does nothing
        cufile_dep = declare_dependency()
        subdir_done()
    endif
    gds_runtime_dep = cuda_dep
    gds_compat_include = []
    gds_plugin_name = 'GDS_MT'
endif

if 'GDS_MT' in static_plugins
    gds_mt_backend_lib = static_library(gds_plugin_name,
        'gds_mt_utils.cpp', 'gds_mt_utils.h',
        'gds_mt_backend.cpp', 'gds_mt_backend.h',
        'gds_mt_plugin.cpp',
        dependencies: [nixl_infra, nixl_common_dep, gds_runtime_dep, cufile_dep, taskflow_proj, file_utils_interface],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        install: false,
        cpp_args: compile_flags + gds_compat_include,
        name_prefix: 'libplugin_')  # Custom prefix for plugin libraries
else
    gds_mt_backend_lib = shared_library(gds_plugin_name,
        'gds_mt_utils.cpp', 'gds_mt_utils.h',
        'gds_mt_backend.cpp', 'gds_mt_backend.h',
        'gds_mt_plugin.cpp',
        dependencies: [nixl_infra, nixl_common_dep, gds_runtime_dep, cufile_dep, taskflow_proj, file_utils_interface],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        install: true,
        cpp_args: ['-fPIC'] + gds_compat_include,
        name_prefix: 'libplugin_',  # Custom prefix for plugin libraries
        install_dir: plugin_install_dir,
        install_rpath: '$ORIGIN/..')
    if get_option('buildtype') == 'debug'
        run_command('sh', '-c',
            'echo "' + gds_plugin_name + '=' + gds_mt_backend_lib.full_path() + '" >> ' + plugin_build_dir + '/pluginlist',
            check: true
        )
    endif
endif

gds_mt_backend_interface = declare_dependency(link_with: gds_mt_backend_lib)
