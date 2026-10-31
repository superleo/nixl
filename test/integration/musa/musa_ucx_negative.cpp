// SPDX-FileCopyrightText: Copyright (c) 2025-2026 MTHREADS CORPORATION. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// MUSA_UCX negative-path driver: every case must be rejected with a clear
// error and must leave the process in a consistent state.
//
// Each case runs in a forked child guarded by a 90 second watchdog; the
// parent aggregates the results. Run with NIXL_LOG_LEVEL=info so that the
// backend failure messages land in the captured stderr.
#include "nixl.h"

#include <musa_runtime_api.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kWatchdogSeconds = 90;

struct CaseResult {
    std::string name;
    bool pass = false;
};

std::vector<CaseResult> g_results;

void
installWatchdog() {
    struct sigaction action{};
    action.sa_handler = [](int) { _exit(124); };
    sigaction(SIGALRM, &action, nullptr);
    alarm(kWatchdogSeconds);
}

[[noreturn]] void
childFail(const std::string &msg) {
    std::cerr << "[neg] case failure: " << msg << std::endl;
    _exit(1);
}

nixlAgentConfig
agentConfig() {
    nixlAgentConfig cfg;
    cfg.useProgThread = true;
    return cfg;
}

void
musaCheck(musaError_t status, const std::string &what) {
    if (status != musaSuccess) {
        childFail(what + ": " + musaGetErrorString(status));
    }
}

struct Session {
    nixlAgent agent;
    nixlBackendH *backend = nullptr;

    explicit Session(const std::string &name) : agent(name, agentConfig()) {
        nixl_b_params_t params;
        nixl_mem_list_t mems;
        nixl_status_t st = agent.getPluginParams("MUSA_UCX", mems, params);
        if (st != NIXL_SUCCESS) {
            childFail("getPluginParams: " + std::to_string(st));
        }
        st = agent.createBackend("MUSA_UCX", params, backend);
        if (st != NIXL_SUCCESS) {
            childFail("createBackend: " + std::to_string(st));
        }
    }
};

// ---------------------------------------------------------------- NEG-00
// Positive control: a legitimate VRAM registration (and only that) must
// succeed. Without it, a machine without any working MUSA provider would
// trivially "pass" every rejection case, because every operation fails.
void
casePositiveControl() {
    Session session("musa_neg_00");
    void *ptr = nullptr;
    musaCheck(musaMalloc(&ptr, 4096), "musaMalloc");
    nixl_opt_args_t opt;
    opt.backends.push_back(session.backend);
    nixl_reg_dlist_t dlist(VRAM_SEG);
    dlist.addDesc(nixlBlobDesc(reinterpret_cast<uintptr_t>(ptr), 4096, 0));
    const nixl_status_t st = session.agent.registerMem(dlist, &opt);
    if (st != NIXL_SUCCESS) {
        musaCheck(musaFree(ptr), "musaFree");
        childFail("positive control: legitimate VRAM registration failed (status " +
                  std::to_string(st) +
                  "); no GPU/provider, or the negative cases below "
                  "prove nothing");
    }
    if (session.agent.deregisterMem(dlist, &opt) != NIXL_SUCCESS) {
        childFail("positive control: deregisterMem failed");
    }
    musaCheck(musaFree(ptr), "musaFree");
    _exit(0);
}

// ---------------------------------------------------------------- NEG-01
// Closing the musa memory domain out of UCX (here: UCX_TLS without any
// musa-capable transport) must make engine creation fail with a capability
// error instead of silently falling back.
void
caseTlsWithoutMusa() {
    setenv("UCX_TLS", "self", 1);
    nixlAgent agent("musa_neg_01", agentConfig());
    nixl_b_params_t params;
    nixl_mem_list_t mems;
    nixl_status_t st = agent.getPluginParams("MUSA_UCX", mems, params);
    if (st != NIXL_SUCCESS) {
        childFail("getPluginParams");
    }
    nixlBackendH *backend = nullptr;
    st = agent.createBackend("MUSA_UCX", params, backend);
    if (st == NIXL_SUCCESS) {
        childFail("engine creation unexpectedly succeeded without a musa memory domain");
    }
    if (backend != nullptr) {
        childFail("backend handle must stay null on failure");
    }
    _exit(0);
}

// ---------------------------------------------------------------- NEG-02
// VRAM descriptor with a device id outside the visible device count.
void
caseBadDeviceId() {
    int count = 0;
    musaCheck(musaGetDeviceCount(&count), "musaGetDeviceCount");
    Session session("musa_neg_02");
    void *ptr = nullptr;
    musaCheck(musaMalloc(&ptr, 4096), "musaMalloc");
    nixl_opt_args_t opt;
    opt.backends.push_back(session.backend);
    nixl_reg_dlist_t dlist(VRAM_SEG);
    dlist.addDesc(nixlBlobDesc(reinterpret_cast<uintptr_t>(ptr), 4096, count + 1000));
    const nixl_status_t st = session.agent.registerMem(dlist, &opt);
    if (st == NIXL_SUCCESS) {
        childFail("register accepted an out-of-range device id");
    }
    musaCheck(musaFree(ptr), "musaFree");
    _exit(0);
}

// ---------------------------------------------------------------- NEG-03
// Host memory presented as VRAM must be rejected before any UCX mapping.
void
caseHostPointerAsVram() {
    Session session("musa_neg_03");
    void *ptr = std::malloc(4096);
    if (!ptr) {
        childFail("malloc");
    }
    nixl_opt_args_t opt;
    opt.backends.push_back(session.backend);
    nixl_reg_dlist_t dlist(VRAM_SEG);
    dlist.addDesc(nixlBlobDesc(reinterpret_cast<uintptr_t>(ptr), 4096, 0));
    const nixl_status_t st = session.agent.registerMem(dlist, &opt);
    std::free(ptr);
    if (st == NIXL_SUCCESS) {
        childFail("register accepted a host pointer as VRAM");
    }
    _exit(0);
}

// ---------------------------------------------------------------- NEG-04
// MUSA device memory presented as DRAM: UCX reports the real memory type and
// the registration must be rejected (contract mismatch), not staged silently.
void
caseDevicePointerAsDram() {
    Session session("musa_neg_04");
    void *ptr = nullptr;
    musaCheck(musaMalloc(&ptr, 4096), "musaMalloc");
    nixl_opt_args_t opt;
    opt.backends.push_back(session.backend);
    nixl_reg_dlist_t dlist(DRAM_SEG);
    dlist.addDesc(nixlBlobDesc(reinterpret_cast<uintptr_t>(ptr), 4096, 0));
    const nixl_status_t st = session.agent.registerMem(dlist, &opt);
    musaCheck(musaFree(ptr), "musaFree");
    if (st == NIXL_SUCCESS) {
        childFail("register accepted device memory as DRAM");
    }
    _exit(0);
}

// ---------------------------------------------------------------- NEG-05/06
// Zero length and address+length overflow must fail validation.
void
caseBadRanges() {
    Session session("musa_neg_05");
    nixl_opt_args_t opt;
    opt.backends.push_back(session.backend);

    nixl_reg_dlist_t zeroLen(DRAM_SEG);
    zeroLen.addDesc(nixlBlobDesc(reinterpret_cast<uintptr_t>(std::malloc(16)), 0, 0));
    if (session.agent.registerMem(zeroLen, &opt) == NIXL_SUCCESS) {
        childFail("register accepted a zero-length descriptor");
    }

    nixl_reg_dlist_t nullAddr(DRAM_SEG);
    nullAddr.addDesc(nixlBlobDesc(0, 64, 0));
    if (session.agent.registerMem(nullAddr, &opt) == NIXL_SUCCESS) {
        childFail("register accepted a null address");
    }

    nixl_reg_dlist_t overflow(DRAM_SEG);
    overflow.addDesc(nixlBlobDesc(UINTPTR_MAX - 32, 64, 0));
    if (session.agent.registerMem(overflow, &opt) == NIXL_SUCCESS) {
        childFail("register accepted an overflowing address range");
    }
    _exit(0);
}

// ---------------------------------------------------------------- NEG-07
// A transfer over an unregistered local range must be rejected.
void
caseUnregisteredRange() {
    Session session("musa_neg_07");
    void *registeredBuf = std::malloc(1024);
    void *looseBuf = std::malloc(1024);
    if (!registeredBuf || !looseBuf) {
        childFail("malloc");
    }
    nixl_opt_args_t opt;
    opt.backends.push_back(session.backend);
    nixl_reg_dlist_t dlist(DRAM_SEG);
    dlist.addDesc(nixlBlobDesc(reinterpret_cast<uintptr_t>(registeredBuf), 1024, 0));
    nixl_status_t st = session.agent.registerMem(dlist, &opt);
    if (st != NIXL_SUCCESS) {
        childFail("positive registration failed");
    }

    nixl_xfer_dlist_t local(DRAM_SEG);
    local.addDesc(nixlBasicDesc(reinterpret_cast<uintptr_t>(looseBuf), 1024, 0));
    nixl_xfer_dlist_t remote(DRAM_SEG);
    remote.addDesc(nixlBasicDesc(reinterpret_cast<uintptr_t>(registeredBuf), 1024, 0));
    nixlXferReqH *req = nullptr;
    st = session.agent.createXferReq(NIXL_WRITE, local, remote, "musa_neg_remote", req, &opt);
    if (st == NIXL_SUCCESS) {
        if (session.agent.releaseXferReq(req) != NIXL_SUCCESS) {
            childFail("releaseXferReq unexpectedly succeeded");
        }
        childFail("transfer accepted an unregistered local range");
    }
    if (session.agent.deregisterMem(dlist, &opt) != NIXL_SUCCESS) {
        childFail("deregisterMem after negative transfer test");
    }
    std::free(registeredBuf);
    std::free(looseBuf);
    _exit(0);
}

// ---------------------------------------------------------------- NEG-08
// Loading a corrupted remote metadata blob must fail cleanly.
void
caseCorruptRemoteMd() {
    Session session("musa_neg_08");
    std::string garbage(256, '\x7f');
    for (size_t i = 0; i < garbage.size(); i++) {
        garbage[i] = static_cast<char>(i);
    }
    std::string name;
    const nixl_status_t st = session.agent.loadRemoteMD(garbage, name);
    if (st == NIXL_SUCCESS) {
        childFail("loadRemoteMD accepted garbage metadata");
    }
    _exit(0);
}

// ---------------------------------------------------------------- NEG-09
// A pointer the runtime identifies as invalid must be rejected as VRAM.
void
caseInvalidVramPointer() {
    Session session("musa_neg_09");
    nixl_opt_args_t opt;
    opt.backends.push_back(session.backend);
    nixl_reg_dlist_t dlist(VRAM_SEG);
    dlist.addDesc(nixlBlobDesc(0xdeadbeef0000ull, 4096, 0));
    const nixl_status_t st = session.agent.registerMem(dlist, &opt);
    if (st == NIXL_SUCCESS) {
        childFail("register accepted an invalid device pointer");
    }
    _exit(0);
}

struct Case {
    const char *name;
    void (*fn)();
};

void
runCase(const Case &c) {
    std::cout << "[neg] RUN  " << c.name << std::endl;
    const pid_t pid = fork();
    if (pid == 0) {
        installWatchdog();
        c.fn();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const bool pass = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    g_results.push_back({c.name, pass});
    std::cout << "[neg] " << (pass ? "PASS" : "FAIL") << " " << c.name << std::endl;
}

} // namespace

int
main() {
    const Case cases[] = {{"NEG-00 positive-control-vram-registration", casePositiveControl},
                          {"NEG-01 tls-without-musa-rejected", caseTlsWithoutMusa},
                          {"NEG-02 bad-device-id-rejected", caseBadDeviceId},
                          {"NEG-03 host-pointer-as-vram-rejected", caseHostPointerAsVram},
                          {"NEG-04 device-pointer-as-dram-rejected", caseDevicePointerAsDram},
                          {"NEG-05 bad-ranges-rejected", caseBadRanges},
                          {"NEG-07 unregistered-range-rejected", caseUnregisteredRange},
                          {"NEG-08 corrupted-remote-md-rejected", caseCorruptRemoteMd},
                          {"NEG-09 invalid-vram-pointer-rejected", caseInvalidVramPointer}};

    for (const auto &c : cases) {
        runCase(c);
    }

    size_t failed = 0;
    for (const auto &r : g_results) {
        if (!r.pass) {
            failed++;
        }
    }
    std::cout << "[neg] summary: " << (g_results.size() - failed) << "/" << g_results.size()
              << " negative cases passed" << std::endl;
    return failed == 0 ? 0 : 1;
}
