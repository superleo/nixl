# SPDX-FileCopyrightText: Copyright (c) 2025-2026 MTHREADS CORPORATION. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Negative-path hardware driver for MUSA_UCX. Every case must be rejected with
# a clear error (see the validation checklist, section 4); a case that
# unexpectedly succeeds is a failure. Requires a visible MUSA device, so it is
# gated behind musa_hardware_tests and registered in the musa-hardware suite.
if get_option('musa_hardware_tests') and enabled_plugins.get('MUSA_UCX') and musa_dep.found() and ucx_dep.found()
    musa_ucx_negative = executable('musa_ucx_negative', 'musa_ucx_negative.cpp',
        dependencies: [nixl_dep, nixl_infra, musa_dep],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        link_with: [serdes_lib])
    test('musa_ucx_negative', musa_ucx_negative,
        suite: 'musa-hardware', timeout: 300)
endif
