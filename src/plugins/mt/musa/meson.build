# SPDX-FileCopyrightText: Copyright (c) 2025-2026 Moore Threads / Contributors. All rights reserved.
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

# Locate the muFile library. It ships alongside the MUSA runtime (libmusa) and
# reuses the cuFile-compatible header layout. Allow an explicit override via
# -Dmufile_path, else auto-detect under MUSA_PATH / known locations.
mufile_path_opt = get_option('mufile_path')
if mufile_path_opt == ''
    musa_path = run_command('bash', '-c', 'echo ${MUSA_PATH:-${MUSA_HOME:-/usr/local/musa}}').stdout().strip()
    if musa_path == ''
        musa_path = '/usr/local/musa'
    endif
    mufile_search_dirs = [musa_path, '/opt/musa', '/usr/local']
    mufile_lib_found = false
    cc_local = meson.get_compiler('cpp')
    foreach search_dir : mufile_search_dirs
        mufile_lib_candidate = search_dir + '/lib'
        mufile_inc_candidate = search_dir + '/include'
        if fs.exists(mufile_lib_candidate) and fs.exists(mufile_inc_candidate)
            mufile_lib_check = cc_local.find_library('mufile',
                dirs: [mufile_lib_candidate], required: false)
            if mufile_lib_check.found()
                cufile_dep = declare_dependency(
                    link_args: ['-L' + mufile_lib_candidate, '-lmufile',
                                '-Wl,-rpath,' + mufile_lib_candidate],
                    include_directories: include_directories(mufile_inc_candidate))
                mufile_lib_found = true
                message('Found muFile at: ' + search_dir)
                break
            endif
        endif
    endforeach
    if not mufile_lib_found
        cufile_dep = declare_dependency()
        subdir_done()
    endif
else
    mufile_lib_path = mufile_path_opt + '/lib'
    mufile_inc_path = mufile_path_opt + '/include'
    cufile_dep = declare_dependency(
        link_args: ['-L' + mufile_lib_path, '-lmufile',
                    '-Wl,-rpath,' + mufile_lib_path],
        include_directories: include_directories(mufile_inc_path))
endif

if not musa_dep.found()
    subdir_done()
endif

# cuda_dep is required by the source's cudaSetDevice/cudaGetErrorString calls.
# On MUSA these resolve to libmusa via the musafile_compat.h shim, so alias.
musa_mt_runtime_dep = musa_dep

if 'MUSA_MT' in static_plugins
    musa_mt_backend_lib = static_library('MUSA_MT',
        'musa_mt_utils.cpp', 'musa_mt_utils.h', 'musafile_compat.h',
        'musa_mt_backend.cpp', 'musa_mt_backend.h',
        'musa_mt_plugin.cpp',
        dependencies: [nixl_infra, nixl_common_dep, musa_mt_runtime_dep, cufile_dep, taskflow_proj,
                         file_utils_interface, file_mt_common_dep],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        install: false,
        cpp_args: compile_flags,
        name_prefix: 'libplugin_')
else
    musa_mt_backend_lib = shared_library('MUSA_MT',
        'musa_mt_utils.cpp', 'musa_mt_utils.h', 'musafile_compat.h',
        'musa_mt_backend.cpp', 'musa_mt_backend.h',
        'musa_mt_plugin.cpp',
        dependencies: [nixl_infra, nixl_common_dep, musa_mt_runtime_dep, cufile_dep, taskflow_proj,
                         file_utils_interface, file_mt_common_dep],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        install: true,
        cpp_args: ['-fPIC'],
        name_prefix: 'libplugin_',
        install_dir: plugin_install_dir,
        install_rpath: '$ORIGIN/..')
    if get_option('buildtype') == 'debug'
        run_command('sh', '-c',
            'echo "MUSA_MT=' + musa_mt_backend_lib.full_path() +
            '" >> ' + plugin_build_dir + '/pluginlist',
            check: true
        )
    endif
endif

musa_mt_backend_interface = declare_dependency(
    link_with: musa_mt_backend_lib)