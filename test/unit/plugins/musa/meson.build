# SPDX-FileCopyrightText: Copyright (c) 2025-2026 MTHREADS CORPORATION. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
musa_test_includes = include_directories('../../../../src/api/cpp', '../../../../src/plugins')
musa_cpu_cases = {
    'memory_policy': files('memory_policy_test.cpp'),
    'registration': files('registration_test.cpp'),
    'musa_policy': files('musa_policy_test.cpp', '../../../../src/plugins/musa/musa_policy.cpp'),
    'musa_runtime_fake': files('musa_runtime_test.cpp', 'fake_sdk/runtime.cpp',
                              '../../../../src/plugins/musa/musa_runtime.cpp',
                              '../../../../src/plugins/musa/musa_policy.cpp'),
}
foreach name, sources : musa_cpu_cases
    extra_includes = name == 'musa_runtime_fake' ? [include_directories('fake_sdk')] : []
    target = executable('test_' + name, sources,
        include_directories: [musa_test_includes] + extra_includes)
    test(name, target, suite: 'musa-cpu', timeout: 30)
endforeach

musa_test_meson = find_program('meson', required: false)
musa_test_python = find_program('python3', required: false)
if musa_test_meson.found() and musa_test_python.found()
    test('musa_build_contract', musa_test_python,
        args: [files('test_build_contract.py'), '-v'], suite: 'musa-configure', timeout: 180)
endif

if ucx_dep.found() and (enabled_plugins.get('UCX') or
                       (enabled_plugins.get('MUSA_UCX') and musa_dep.found()))
    musa_factory_test = executable('test_ucx_policy_factory', 'ucx_policy_factory_test.cpp',
        dependencies: [ucx_impl_interface],
        include_directories: [musa_test_includes, nixl_inc_dirs, utils_inc_dirs])
    test('ucx_policy_factory', musa_factory_test, suite: 'musa-ucx', timeout: 60)
    test('ucx_policy_rejects_sgl', musa_factory_test,
        args: ['--reject-sgl'], env: ['NIXL_UCX_SGL_ENABLE=1'], suite: 'musa-ucx', timeout: 60)
endif

if enabled_plugins.get('MUSA_UCX') and musa_dep.found() and ucx_dep.found()
    musa_plugin_test = executable('test_musa_plugin', 'musa_plugin_test.cpp',
        dependencies: [dependency('dl'), nixl_common_dep],
        include_directories: [nixl_inc_dirs, utils_inc_dirs])
    test('musa_plugin', musa_plugin_test, args: [musa_backend_lib.full_path()],
        depends: musa_backend_lib, suite: 'musa-plugin', timeout: 30)
endif

if get_option('musa_hardware_tests')
    if not (enabled_plugins.get('MUSA_UCX') and musa_dep.found() and ucx_dep.found())
        error('musa_hardware_tests requires a buildable MUSA_UCX plugin: ' +
              'set -Dmusa_path=<SDK prefix> and -Ducx_path=<UCX prefix> and keep MUSA_UCX enabled')
    endif
    musa_ucx_e2e = executable('musa_ucx_e2e', 'musa_ucx_e2e.cpp',
        dependencies: [nixl_dep, nixl_infra, musa_dep],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        link_with: [serdes_lib])
    # Documented hardware registration: single host, dual process, device/device,
    # 64 KiB, background progress, device 0, WRITE + notification and READ back.
    # Without a visible MUSA device or musa-capable UCX provider this must FAIL.
    test('musa_ucx_e2e_device_device_64k', musa_ucx_e2e,
        args: ['device', 'device', '65536', '0', '0', '0'],
        suite: 'musa-hardware', timeout: 150)
endif
