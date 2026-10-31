// SPDX-FileCopyrightText: Copyright (c) 2025-2026 MTHREADS CORPORATION. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// MUSA_UCX single-host dual-process end-to-end driver.
//
// Usage: musa_ucx_e2e <memA> <memB> <bytes> <post-threads> <devA> <devB>
//   memA/memB: host | device (sender side A, receiver side B)
//   bytes:     transfer size in bytes
//   post-threads: backend 'num_threads' init parameter
//   devA/devB: MUSA device ids for sides A and B (ignored for host memory)
//
// The process forks after argument parsing; both peers initialize the MUSA
// runtime and NIXL/UCX after the fork. The NIXL backend (ucp_init) is created
// before any MUSA allocation: UCX's memtype cache only tracks allocations
// made after its hooks are installed, and VRAM registered across that
// boundary would be detected as host memory. Every run is guarded by a 90
// second watchdog; watchdog expiry is a failure, never a skip.
//
// Optional environment extensions (defaults keep the documented behavior):
//   NIXL_MUSA_E2E_ITERS=<n>       repeat register/post/release cycles n times
//   NIXL_MUSA_E2E_NDESC=<n>       split the buffer into n (unaligned) descriptors
//   NIXL_MUSA_E2E_PROG_MODE=bg|caller   background or caller-only progress
//   NIXL_MUSA_E2E_KILL_RECV=1     receiver exits after metadata exchange; the
//                                 sender must observe a bounded failure
//   NIXL_MUSA_E2E_CHECK_RES=1     record VmRSS and MUSA device free memory
//                                 before/after the run
//   NIXL_MUSA_E2E_TCP_LISTEN=<ip:port> / _TCP_CONNECT=<ip:port>
//                                 run the two peers as separate processes
//                                 (receiver listens, sender connects)
#include "nixl.h"

#include <musa_runtime_api.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int kWatchdogSeconds = 90;
constexpr int kExitFail = 1;
constexpr int kExitWatchdog = 124;

[[noreturn]] void
fail(const std::string &msg) {
    std::cerr << "[e2e] FAIL: " << msg << std::endl;
    std::exit(kExitFail);
}

void
watchdogHandler(int) {
    static const char message[] = "[e2e] FAIL: watchdog timeout\n";
    ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)ignored;
    _exit(kExitWatchdog);
}

void
installWatchdog() {
    struct sigaction action{};
    action.sa_handler = watchdogHandler;
    sigaction(SIGALRM, &action, nullptr);
    alarm(kWatchdogSeconds);
}

std::string
musaStr(musaError_t status) {
    return std::string(musaGetErrorString(status)) + " (" +
        std::to_string(static_cast<int>(status)) + ")";
}

void
musaCheck(musaError_t status, const std::string &what) {
    if (status != musaSuccess) {
        fail(what + ": " + musaStr(status));
    }
}

// Data pattern distinguishing rank (peer), operation, descriptor index and
// offset, as required by the validation checklist.
uint8_t
patternByte(int rank, int op, int desc, size_t offset) {
    uint32_t h = static_cast<uint32_t>(offset & 0xffffffffu);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<uint8_t>(h ^ (rank * 0x5b) ^ (op * 0x1d) ^ (desc * 0x71));
}

void
fillStaging(std::vector<uint8_t> &staging, int rank, int op, int desc, size_t from, size_t len) {
    for (size_t i = 0; i < len; i++) {
        staging[from + i] = patternByte(rank, op, desc, from + i);
    }
}

struct DescRange {
    size_t offset = 0;
    size_t len = 0;
};

// Split [0, bytes) into n ranges with deliberately non-8-byte-aligned
// boundaries so that sub-descriptor transfers stay exercised.
std::vector<DescRange>
splitRanges(size_t bytes, size_t n) {
    std::vector<DescRange> ranges;
    if (n == 1) {
        ranges.push_back({0, bytes});
        return ranges;
    }
    size_t base = bytes / n;
    size_t extra = bytes % n;
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len = base + (i < extra ? 1 : 0);
        // Nudge the interior boundaries off 8-byte alignment where possible,
        // shrinking the previous descriptor so ranges stay disjoint.
        if (i + 1 < n && off > 0 && off % 8 == 0 && off >= 3) {
            ranges.back().len -= 3;
            off -= 3;
            len += 3;
        }
        ranges.push_back({off, len});
        off += len;
    }
    ranges.back().len = bytes - ranges.back().offset;
    return ranges;
}

struct PeerIO {
    int readFd = -1;
    int writeFd = -1;
    bool useSocket = false;

    [[noreturn]] void
    ioFail(const std::string &what) const {
        fail("peer channel " + what + ": " + std::strerror(errno));
    }

    void
    sendAll(const void *data, size_t size) const {
        const auto *p = static_cast<const uint8_t *>(data);
        while (size > 0) {
            ssize_t n;
            if (useSocket) {
                n = ::send(writeFd, p, size, MSG_NOSIGNAL);
            } else {
                n = ::write(writeFd, p, size);
            }
            if (n <= 0) {
                ioFail("write");
            }
            p += n;
            size -= static_cast<size_t>(n);
        }
    }

    void
    recvAll(void *data, size_t size) const {
        auto *p = static_cast<uint8_t *>(data);
        while (size > 0) {
            ssize_t n;
            if (useSocket) {
                n = ::recv(readFd, p, size, 0);
            } else {
                n = ::read(readFd, p, size);
            }
            if (n <= 0) {
                ioFail("read");
            }
            p += n;
            size -= static_cast<size_t>(n);
        }
    }

    void
    sendString(const std::string &str) const {
        const uint32_t len = static_cast<uint32_t>(str.size());
        sendAll(&len, sizeof(len));
        sendAll(str.data(), str.size());
    }

    std::string
    recvString() const {
        uint32_t len = 0;
        recvAll(&len, sizeof(len));
        if (len > (64u << 20)) {
            fail("peer channel: implausible metadata length");
        }
        std::string str(len, '\0');
        if (len > 0) {
            recvAll(str.data(), len);
        }
        return str;
    }
};

struct Config {
    bool senderDevice = false;
    bool receiverDevice = false;
    size_t bytes = 0;
    unsigned threads = 0;
    int devA = 0;
    int devB = 0;
    unsigned iters = 1;
    unsigned ndesc = 1;
    bool callerProgress = false;
    bool killReceiver = false;
    bool checkRes = false;
    std::string tcpListen;
    std::string tcpConnect;
};

Config
parseConfig(int argc, char **argv) {
    if (argc != 7) {
        std::cerr << "usage: " << argv[0]
                  << " <host|device> <host|device> <bytes> <post-threads> <devA> <devB>"
                  << std::endl;
        std::exit(kExitFail);
    }
    Config cfg;
    const std::string memA = argv[1];
    const std::string memB = argv[2];
    if ((memA != "host" && memA != "device") || (memB != "host" && memB != "device")) {
        fail("memory kinds must be 'host' or 'device'");
    }
    cfg.senderDevice = memA == "device";
    cfg.receiverDevice = memB == "device";
    cfg.bytes = std::strtoull(argv[3], nullptr, 10);
    cfg.threads = static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10));
    cfg.devA = std::atoi(argv[5]);
    cfg.devB = std::atoi(argv[6]);
    if (cfg.bytes == 0 || cfg.devA < 0 || cfg.devB < 0) {
        fail("invalid argument values");
    }
    auto envUint = [](const char *name, unsigned def, unsigned max) {
        const char *v = std::getenv(name);
        if (!v || !*v) {
            return def;
        }
        unsigned long parsed = std::strtoul(v, nullptr, 10);
        if (parsed > max) {
            fail(std::string(name) + " out of range");
        }
        return static_cast<unsigned>(parsed);
    };
    cfg.iters = envUint("NIXL_MUSA_E2E_ITERS", 1, 100000);
    cfg.ndesc = envUint("NIXL_MUSA_E2E_NDESC", 1, 64);
    if (const char *v = std::getenv("NIXL_MUSA_E2E_PROG_MODE")) {
        if (std::string(v) == "caller") {
            cfg.callerProgress = true;
        }
    }
    if (const char *v = std::getenv("NIXL_MUSA_E2E_KILL_RECV")) {
        cfg.killReceiver = std::atoi(v) == 1;
    }
    if (const char *v = std::getenv("NIXL_MUSA_E2E_CHECK_RES")) {
        cfg.checkRes = std::atoi(v) == 1;
    }
    if (const char *v = std::getenv("NIXL_MUSA_E2E_TCP_LISTEN")) {
        cfg.tcpListen = v;
    }
    if (const char *v = std::getenv("NIXL_MUSA_E2E_TCP_CONNECT")) {
        cfg.tcpConnect = v;
    }
    return cfg;
}

struct DeviceBuffer {
    void *ptr = nullptr;
    size_t bytes = 0;
    bool device = false;
    int devId = 0;
};

void
allocAndFill(DeviceBuffer &buf,
             bool device,
             int devId,
             size_t bytes,
             bool zero,
             const std::vector<DescRange> &ranges) {
    buf.device = device;
    buf.devId = devId;
    buf.bytes = bytes;
    if (!device) {
        buf.ptr = std::malloc(bytes);
        if (!buf.ptr) {
            fail("host allocation failed");
        }
        if (zero) {
            std::memset(buf.ptr, 0, bytes);
        } else {
            auto *host = static_cast<uint8_t *>(buf.ptr);
            for (size_t d = 0; d < ranges.size(); d++) {
                for (size_t i = 0; i < ranges[d].len; i++) {
                    host[ranges[d].offset + i] =
                        patternByte(0, 0, static_cast<int>(d), ranges[d].offset + i);
                }
            }
        }
        return;
    }
    musaCheck(musaSetDevice(devId), "musaSetDevice");
    musaCheck(musaMalloc(&buf.ptr, bytes), "musaMalloc");
    std::vector<uint8_t> staging(bytes);
    if (zero) {
        std::fill(staging.begin(), staging.end(), 0);
    } else {
        for (size_t d = 0; d < ranges.size(); d++) {
            fillStaging(staging, 0, 0, static_cast<int>(d), ranges[d].offset, ranges[d].len);
        }
    }
    musaCheck(musaMemcpy(buf.ptr, staging.data(), bytes, musaMemcpyHostToDevice), "musaMemcpy H2D");
    // Synchronize the producer stream before any transfer, as required by the
    // validation checklist.
    musaCheck(musaDeviceSynchronize(), "musaDeviceSynchronize");
}

void
freeBuffer(DeviceBuffer &buf) {
    if (!buf.ptr) {
        return;
    }
    if (buf.device) {
        musaCheck(musaFree(buf.ptr), "musaFree");
    } else {
        std::free(buf.ptr);
    }
    buf.ptr = nullptr;
}

void
verifyBuffer(const DeviceBuffer &buf, const std::vector<DescRange> &ranges, const char *label) {
    std::vector<uint8_t> staging(buf.bytes);
    if (buf.device) {
        musaCheck(musaSetDevice(buf.devId), "musaSetDevice");
        musaCheck(musaMemcpy(staging.data(), buf.ptr, buf.bytes, musaMemcpyDeviceToHost),
                  "musaMemcpy D2H");
        musaCheck(musaDeviceSynchronize(), "musaDeviceSynchronize");
    } else {
        std::memcpy(staging.data(), buf.ptr, buf.bytes);
    }
    for (size_t d = 0; d < ranges.size(); d++) {
        for (size_t i = 0; i < ranges[d].len; i++) {
            const size_t off = ranges[d].offset + i;
            const uint8_t expected = patternByte(0, 0, static_cast<int>(d), off);
            if (staging[off] != expected) {
                char where[64];
                std::snprintf(where, sizeof(where), "%zu", off);
                fail(std::string(label) + " byte mismatch at offset " + where + " (desc " +
                     std::to_string(d) + "): expected " + std::to_string(expected) + ", got " +
                     std::to_string(staging[off]));
            }
        }
    }
    std::cout << "[e2e] " << label << ": byte-exact verification over " << ranges.size()
              << " descriptor(s) passed" << std::endl;
}

struct ResourceSnapshot {
    size_t vmRssKb = 0;
    size_t musaFree = 0;
    size_t musaTotal = 0;
};

ResourceSnapshot
takeSnapshot(int devId) {
    ResourceSnapshot snap;
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            snap.vmRssKb = static_cast<size_t>(std::strtoul(line.c_str() + 6, nullptr, 10));
            break;
        }
    }
    int count = 0;
    if (musaGetDeviceCount(&count) == musaSuccess && count > devId && devId >= 0) {
        if (musaSetDevice(devId) == musaSuccess) {
            musaMemGetInfo(&snap.musaFree, &snap.musaTotal);
        }
    }
    return snap;
}

void
reportSnapshots(const ResourceSnapshot &before, const ResourceSnapshot &after) {
    std::cout << "[e2e] RES: VmRSS_kb_before=" << before.vmRssKb
              << " VmRSS_kb_after=" << after.vmRssKb << " musa_free_before=" << before.musaFree
              << " musa_free_after=" << after.musaFree << " musa_total=" << after.musaTotal
              << std::endl;
}

nixl_mem_t
memType(bool device) {
    return device ? VRAM_SEG : DRAM_SEG;
}

struct AgentSession {
    nixlAgent agent;
    nixlBackendH *backend = nullptr;

    static nixlAgentConfig
    makeConfig(bool callerProgress) {
        nixlAgentConfig cfg;
        // Background progress by default; caller-only progress when requested.
        cfg.useProgThread = !callerProgress;
        return cfg;
    }

    AgentSession(const std::string &name, bool callerProgress, unsigned threads)
        : agent(name, makeConfig(callerProgress)) {
        nixl_b_params_t params;
        nixl_mem_list_t mems;
        nixl_status_t st = agent.getPluginParams("MUSA_UCX", mems, params);
        if (st != NIXL_SUCCESS) {
            fail("getPluginParams(MUSA_UCX): " + std::to_string(st));
        }
        params["num_threads"] = std::to_string(threads);
        st = agent.createBackend("MUSA_UCX", params, backend);
        if (st != NIXL_SUCCESS) {
            fail("createBackend(MUSA_UCX): " + std::to_string(st));
        }
    }
};

struct WireDesc {
    uint64_t addr;
    uint64_t len;
};

void
sendDescTable(const PeerIO &io, const std::vector<DescRange> &ranges, uintptr_t base) {
    const uint32_t n = static_cast<uint32_t>(ranges.size());
    io.sendAll(&n, sizeof(n));
    std::vector<WireDesc> wire(ranges.size());
    for (size_t i = 0; i < ranges.size(); i++) {
        wire[i] = {static_cast<uint64_t>(base + ranges[i].offset),
                   static_cast<uint64_t>(ranges[i].len)};
    }
    io.sendAll(wire.data(), wire.size() * sizeof(WireDesc));
}

std::vector<WireDesc>
recvDescTable(const PeerIO &io) {
    uint32_t n = 0;
    io.recvAll(&n, sizeof(n));
    if (n == 0 || n > 64) {
        fail("implausible descriptor count from peer");
    }
    std::vector<WireDesc> wire(n);
    io.recvAll(wire.data(), wire.size() * sizeof(WireDesc));
    return wire;
}

int
runReceiver(const Config &cfg, PeerIO io, const std::string &agentName) {
    installWatchdog();
    ResourceSnapshot snapBefore;
    if (cfg.checkRes) {
        snapBefore = takeSnapshot(cfg.devB);
    }

    // The backend (and with it the UCX context and its memory-type hooks)
    // must exist before any MUSA allocation: buffers allocated before
    // ucp_init are invisible to the memtype cache and would be detected as
    // host memory, failing the MUSA memory contract.
    AgentSession session(agentName, cfg.callerProgress, cfg.threads);
    nixl_opt_args_t opt;
    opt.backends.push_back(session.backend);

    const auto ranges = splitRanges(cfg.bytes, cfg.ndesc);
    DeviceBuffer dst;
    allocAndFill(dst, cfg.receiverDevice, cfg.devB, cfg.bytes, /*zero=*/true, ranges);

    nixl_reg_dlist_t reg(memType(cfg.receiverDevice));
    for (size_t i = 0; i < ranges.size(); i++) {
        reg.addDesc(nixlBlobDesc(
            reinterpret_cast<uintptr_t>(dst.ptr) + ranges[i].offset, ranges[i].len, cfg.devB));
    }
    nixl_status_t st = session.agent.registerMem(reg, &opt);
    if (st != NIXL_SUCCESS) {
        fail("receiver registerMem: " + std::to_string(st));
    }

    std::string meta;
    st = session.agent.getLocalMD(meta);
    if (st != NIXL_SUCCESS || meta.empty()) {
        fail("receiver getLocalMD");
    }
    io.sendString(meta);
    sendDescTable(io, ranges, reinterpret_cast<uintptr_t>(dst.ptr));

    const std::string senderMeta = io.recvString();
    std::string remoteName;
    st = session.agent.loadRemoteMD(senderMeta, remoteName);
    if (st != NIXL_SUCCESS) {
        fail("receiver loadRemoteMD");
    }

    if (cfg.killReceiver) {
        std::cout << "[e2e] receiver exiting early per KILL_RECV mode" << std::endl;
        _exit(0);
    }

    // H-09: on seeing the WRITE notification, validate the data immediately.
    nixl_notifs_t notifs;
    unsigned seen = 0;
    while (seen < cfg.iters) {
        st = session.agent.getNotifs(notifs, &opt);
        if (st != NIXL_SUCCESS) {
            fail("receiver getNotifs");
        }
        for (const auto &kv : notifs) {
            for (const auto &msg : kv.second) {
                if (msg == "e2e-write-done") {
                    seen++;
                }
            }
        }
        notifs.clear();
        usleep(500);
    }
    verifyBuffer(dst, ranges, "receiver-after-notif");

    // Wait for the sender's READ to finish before releasing anything. The
    // poll keeps the loop non-blocking; the periodic getNotifs keeps caller
    // progress alive so the sender's remote-initiated READ can execute on
    // this side (background progress would advance it from the helper thread).
    uint8_t done = 0;
    for (;;) {
        struct pollfd pfd{io.readFd, POLLIN, 0};
        const int r = ::poll(&pfd, 1, 100);
        if (r < 0 && errno != EINTR) {
            fail("poll for sender done marker");
        }
        if (r > 0 && (pfd.revents & POLLIN)) {
            io.recvAll(&done, sizeof(done));
            break;
        }
        nixl_notifs_t keepalive;
        st = session.agent.getNotifs(keepalive, &opt);
        if (st != NIXL_SUCCESS) {
            fail("receiver getNotifs keepalive");
        }
    }
    if (done != 0x5a) {
        fail("receiver: missing done marker from sender");
    }

    st = session.agent.deregisterMem(reg, &opt);
    if (st != NIXL_SUCCESS) {
        fail("receiver deregisterMem");
    }
    st = session.agent.invalidateRemoteMD(remoteName);
    if (st != NIXL_SUCCESS) {
        fail("receiver invalidateRemoteMD");
    }
    freeBuffer(dst);

    if (cfg.checkRes) {
        reportSnapshots(snapBefore, takeSnapshot(cfg.devB));
    }
    std::cout << "[e2e] receiver completed" << std::endl;
    return 0;
}

int
runSender(const Config &cfg,
          PeerIO io,
          const std::string &agentName,
          const std::string &receiverName) {
    installWatchdog();
    ResourceSnapshot snapBefore;
    if (cfg.checkRes) {
        snapBefore = takeSnapshot(cfg.devA);
    }

    // Backend before MUSA allocations, same contract as the receiver.
    AgentSession session(agentName, cfg.callerProgress, cfg.threads);
    nixl_opt_args_t opt;
    opt.backends.push_back(session.backend);

    const auto ranges = splitRanges(cfg.bytes, cfg.ndesc);
    DeviceBuffer src, readback;
    allocAndFill(src, cfg.senderDevice, cfg.devA, cfg.bytes, /*zero=*/false, ranges);
    allocAndFill(readback, cfg.senderDevice, cfg.devA, cfg.bytes, /*zero=*/true, ranges);

    nixl_reg_dlist_t regSrc(memType(cfg.senderDevice));
    nixl_reg_dlist_t regRb(memType(cfg.senderDevice));
    for (size_t i = 0; i < ranges.size(); i++) {
        regSrc.addDesc(nixlBlobDesc(
            reinterpret_cast<uintptr_t>(src.ptr) + ranges[i].offset, ranges[i].len, cfg.devA));
        regRb.addDesc(nixlBlobDesc(
            reinterpret_cast<uintptr_t>(readback.ptr) + ranges[i].offset, ranges[i].len, cfg.devA));
    }
    nixl_status_t st = session.agent.registerMem(regSrc, &opt);
    if (st != NIXL_SUCCESS) {
        fail("sender registerMem(src)");
    }
    st = session.agent.registerMem(regRb, &opt);
    if (st != NIXL_SUCCESS) {
        fail("sender registerMem(readback)");
    }

    std::string meta;
    st = session.agent.getLocalMD(meta);
    if (st != NIXL_SUCCESS || meta.empty()) {
        fail("sender getLocalMD");
    }
    io.sendString(meta);

    const std::string recvMeta = io.recvString();
    const auto remoteDescs = recvDescTable(io);
    if (remoteDescs.size() != ranges.size()) {
        fail("descriptor count mismatch from receiver");
    }
    std::string remoteName;
    st = session.agent.loadRemoteMD(recvMeta, remoteName);
    if (st != NIXL_SUCCESS) {
        fail("sender loadRemoteMD");
    }
    if (remoteName != receiverName) {
        fail("unexpected remote agent name: " + remoteName);
    }

    nixl_xfer_dlist_t localWrite(memType(cfg.senderDevice));
    nixl_xfer_dlist_t localRead(memType(cfg.senderDevice));
    nixl_xfer_dlist_t remote(memType(cfg.receiverDevice));
    for (size_t i = 0; i < ranges.size(); i++) {
        localWrite.addDesc(nixlBasicDesc(
            reinterpret_cast<uintptr_t>(src.ptr) + ranges[i].offset, ranges[i].len, cfg.devA));
        localRead.addDesc(nixlBasicDesc(
            reinterpret_cast<uintptr_t>(readback.ptr) + ranges[i].offset, ranges[i].len, cfg.devA));
        remote.addDesc(nixlBasicDesc(static_cast<uintptr_t>(remoteDescs[i].addr),
                                     static_cast<size_t>(remoteDescs[i].len),
                                     cfg.devB));
    }

    for (unsigned iter = 0; iter < cfg.iters; iter++) {
        nixl_opt_args_t xferOpt;
        xferOpt.backends.push_back(session.backend);
        xferOpt.notif = std::string("e2e-write-done");
        nixlXferReqH *reqW = nullptr;
        st =
            session.agent.createXferReq(NIXL_WRITE, localWrite, remote, remoteName, reqW, &xferOpt);
        if (st != NIXL_SUCCESS) {
            fail("sender createXferReq(WRITE) iter " + std::to_string(iter));
        }
        st = session.agent.postXferReq(reqW);
        if (st != NIXL_SUCCESS && st != NIXL_IN_PROG) {
            if (cfg.killReceiver) {
                std::cout << "[e2e] sender observed bounded failure after receiver exit: status="
                          << st << std::endl;
                _exit(0); // expected failure path of the KILL_RECV mode
            }
            fail("sender postXferReq(WRITE): " + std::to_string(st));
        }
        while (st != NIXL_SUCCESS) {
            st = session.agent.getXferStatus(reqW);
            if (st < 0) {
                if (cfg.killReceiver) {
                    std::cout << "[e2e] sender observed bounded failure after receiver exit: "
                                 "status="
                              << st << std::endl;
                    _exit(0);
                }
                fail("sender WRITE failed: " + std::to_string(st));
            }
            if (st != NIXL_SUCCESS && st != NIXL_IN_PROG) {
                fail("sender WRITE poll: unexpected status " + std::to_string(st));
            }
        }
        if (cfg.killReceiver) {
            fail("WRITE unexpectedly completed with a dead receiver");
        }
        st = session.agent.releaseXferReq(reqW);
        if (st != NIXL_SUCCESS) {
            fail("sender releaseXferReq(WRITE)");
        }

        nixlXferReqH *reqR = nullptr;
        nixl_opt_args_t readOpt;
        readOpt.backends.push_back(session.backend);
        st = session.agent.createXferReq(NIXL_READ, localRead, remote, remoteName, reqR, &readOpt);
        if (st != NIXL_SUCCESS) {
            fail("sender createXferReq(READ)");
        }
        st = session.agent.postXferReq(reqR);
        if (st != NIXL_SUCCESS && st != NIXL_IN_PROG) {
            fail("sender postXferReq(READ): " + std::to_string(st));
        }
        while (st != NIXL_SUCCESS) {
            st = session.agent.getXferStatus(reqR);
            if (st < 0) {
                fail("sender READ failed: " + std::to_string(st));
            }
            if (st != NIXL_SUCCESS && st != NIXL_IN_PROG) {
                fail("sender READ poll: unexpected status " + std::to_string(st));
            }
        }
        st = session.agent.releaseXferReq(reqR);
        if (st != NIXL_SUCCESS) {
            fail("sender releaseXferReq(READ)");
        }
    }

    if (cfg.iters == 1) {
        verifyBuffer(readback, ranges, "sender-readback");
    }

    if (!cfg.killReceiver) {
        const uint8_t done = 0x5a;
        io.sendAll(&done, sizeof(done));
    }

    st = session.agent.deregisterMem(regSrc, &opt);
    if (st != NIXL_SUCCESS) {
        fail("sender deregisterMem(src)");
    }
    st = session.agent.deregisterMem(regRb, &opt);
    if (st != NIXL_SUCCESS) {
        fail("sender deregisterMem(readback)");
    }
    st = session.agent.invalidateRemoteMD(remoteName);
    if (st != NIXL_SUCCESS) {
        fail("sender invalidateRemoteMD");
    }
    freeBuffer(readback);
    freeBuffer(src);

    if (cfg.checkRes) {
        reportSnapshots(snapBefore, takeSnapshot(cfg.devA));
    }
    std::cout << "[e2e] sender completed" << std::endl;
    return 0;
}

int
openTcpListen(const std::string &ip, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("socket");
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        fail("bad listen ip");
    }
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        fail("bind");
    }
    if (listen(fd, 1) != 0) {
        fail("listen");
    }
    return fd;
}

PeerIO
acceptOn(const std::string &listenSpec) {
    const auto colon = listenSpec.rfind(':');
    if (colon == std::string::npos) {
        fail("TCP listen spec must be ip:port");
    }
    const std::string ip = listenSpec.substr(0, colon);
    const int port = std::atoi(listenSpec.c_str() + colon + 1);
    const int lfd = openTcpListen(ip, port);
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    const int fd = accept(lfd, reinterpret_cast<sockaddr *>(&peer), &len);
    if (fd < 0) {
        fail("accept");
    }
    ::close(lfd);
    return {/*readFd=*/fd, /*writeFd=*/fd, /*useSocket=*/true};
}

PeerIO
connectTo(const std::string &connectSpec) {
    const auto colon = connectSpec.rfind(':');
    if (colon == std::string::npos) {
        fail("TCP connect spec must be ip:port");
    }
    const std::string ip = connectSpec.substr(0, colon);
    const int port = std::atoi(connectSpec.c_str() + colon + 1);
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("socket");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        fail("bad connect ip");
    }
    for (int attempt = 0; attempt < 200; attempt++) {
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            return {/*readFd=*/fd, /*writeFd=*/fd, /*useSocket=*/true};
        }
        usleep(200000);
    }
    fail("connect to receiver");
}

} // namespace

static void
printPassSummary(const char *role, const Config &cfg) {
    std::cout << "[e2e] PASS";
    if (role != nullptr && *role != '\0') {
        std::cout << " (" << role << ")";
    }
    std::cout << ": memA=" << (cfg.senderDevice ? "device" : "host")
              << " memB=" << (cfg.receiverDevice ? "device" : "host") << " bytes=" << cfg.bytes
              << " threads=" << cfg.threads << " devA=" << cfg.devA << " devB=" << cfg.devB
              << " ndesc=" << cfg.ndesc << " iters=" << cfg.iters << std::endl;
}

int
main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    const Config cfg = parseConfig(argc, argv);
    const std::string receiverAgent = "musa_e2e_receiver";
    const std::string senderAgent = "musa_e2e_sender";

    if (!cfg.tcpListen.empty()) {
        // Two-host receiver role; the sender connects from the peer node.
        const int rc = runReceiver(cfg, acceptOn(cfg.tcpListen), receiverAgent);
        if (rc == 0) {
            printPassSummary("two-host receiver", cfg);
        }
        return rc;
    }
    if (!cfg.tcpConnect.empty()) {
        const int rc = runSender(cfg, connectTo(cfg.tcpConnect), senderAgent, receiverAgent);
        if (rc == 0) {
            printPassSummary("two-host sender", cfg);
        }
        return rc;
    }

    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) {
        fail("pipe");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        // Sender (child). Nothing MUSA/UCX related was touched before the fork.
        ::close(p2c[1]);
        ::close(c2p[0]);
        const PeerIO io{/*readFd=*/p2c[0], /*writeFd=*/c2p[1], /*useSocket=*/false};
        const int rc = runSender(cfg, io, senderAgent, receiverAgent);
        std::exit(rc == 0 ? 0 : kExitFail);
    }

    ::close(p2c[0]);
    ::close(c2p[1]);
    const PeerIO io{/*readFd=*/c2p[0], /*writeFd=*/p2c[1], /*useSocket=*/false};
    const int rc = runReceiver(cfg, io, receiverAgent);

    int childStatus = 0;
    if (waitpid(pid, &childStatus, 0) != pid) {
        fail("waitpid");
    }
    const int childRc = WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : -1;
    if (rc != 0 || childRc != 0) {
        std::cerr << "[e2e] FAIL: receiver rc=" << rc << " sender rc=" << childRc << std::endl;
        return kExitFail;
    }
    printPassSummary(nullptr, cfg);
    return 0;
}
