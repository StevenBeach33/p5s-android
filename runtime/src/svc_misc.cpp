// Small services that only need to answer "yes, fine": parental controls, add-on content, logging, power.
<<<<<<< HEAD
#include <algorithm>
#include <cstdlib>
#include <cstring>
=======
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include <nx/runtime.h>

>>>>>>> 8b93a28 (beta)
#include "ipc.h"
#include <nx/cpu.h>
#include <nx/runtime.h>

namespace nx {

namespace {

std::shared_ptr<Service> make(const char* name, std::function<void(Service&)> setup = {}) {
    auto s = std::make_shared<Service>(name);
    if (setup) setup(*s);
    return s;
}

std::shared_ptr<Service> parental_control() {
    return make("IParentalControlService", [](Service& s) {
        s.cmds[1403] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // IsRestrictionEnabled
        s.cmds[1001] = [](IpcCtx&) { return 0u; };                          // CheckFreeCommunicationPermission
    });
}

void pctl(Service& s) {
    s.cmds[0] = [](IpcCtx& c) { c.push_object(parental_control()); return 0u; };  // CreateService
    s.cmds[1] = [](IpcCtx& c) { c.push_object(parental_control()); return 0u; };  // CreateServiceWithoutInitialize
}

ServiceRegistrar r_pctl("pctl", [] { return make("pctl", pctl); });
ServiceRegistrar r_pctl_s("pctl:s", [] { return make("pctl:s", pctl); });
ServiceRegistrar r_pctl_a("pctl:a", [] { return make("pctl:a", pctl); });
ServiceRegistrar r_pctl_r("pctl:r", [] { return make("pctl:r", pctl); });

// ILogger::Log: the game's own log, which usually says why it stops. A packet is a 24-byte header (process id, thread id,
// flags, padding, severity, verbosity, payload size), then chunks of (key, size, data) with LEB128 key and size:
// 2 text, 3 line number, 4 file, 5 function, 6 module. Long messages come in several packets; each is printed as it is.
void log_packet(const uint8_t* p, size_t n) {
    static std::atomic<int> shown{0};
    if (n < 24) return;
    const uint32_t payload = uint32_t(p[20]) | uint32_t(p[21]) << 8 | uint32_t(p[22]) << 16 | uint32_t(p[23]) << 24;
    const size_t end = std::min<size_t>(n, 24 + size_t(payload));
    std::string text, file, func, module;
    uint32_t line = 0;
    auto leb = [&](size_t& o, uint64_t& v) {
        v = 0;
        for (int shift = 0; shift < 63 && o < end; shift += 7) {
            uint8_t b = p[o++];
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    };
    for (size_t o = 24; o < end;) {
        uint64_t key, size;
        if (!leb(o, key) || !leb(o, size) || size > end - o) break;
        const char* d = reinterpret_cast<const char*>(p) + o;
        switch (key) {
            case 2: text.append(d, size_t(size)); break;
            case 3: if (size >= 4) std::memcpy(&line, d, 4); break;
            case 4: file.assign(d, size_t(size)); break;
            case 5: func.assign(d, size_t(size)); break;
            case 6: module.assign(d, size_t(size)); break;
            default: break;
        }
        o += size_t(size);
    }
    if (text.empty()) return;
    const int k = shown.fetch_add(1);
    if (k >= 500) {
        if (k == 500) log("lm: more game log messages are not shown");
        return;
    }
    for (char& ch : text)
        if (ch != '\n' && (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) == 0x7f)) ch = '?';
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    log("lm: severity %u [%s] %s:%u %s | %s", unsigned(p[18]), module.c_str(), file.c_str(), line, func.c_str(),
        text.c_str());
}

ServiceRegistrar r_lm("lm", [] {
    return make("lm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) {  // OpenLogger
            c.push_object(make("ILogger", [](Service& l) {
                l.cmds[0] = [](IpcCtx& c2) {  // Log
                    Buffer in = c2.in_buf(0);
                    if (in.addr && in.size) log_packet(reinterpret_cast<const uint8_t*>(in.addr), size_t(in.size));
                    return 0u;
                };
                l.cmds[1] = [](IpcCtx&) { return 0u; };  // SetDestination
            }));
            return 0u;
        };
    });
});

ServiceRegistrar r_apm("apm", [] {
    return make("apm", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push_object(make("ISession")); return 0u; };  // OpenSession
        s.cmds[1] = [](IpcCtx& c) {  // GetPerformanceMode: boost (docked), normal with NX_HANDHELD (svc_am.cpp)
            c.push<uint32_t>(std::getenv("NX_HANDHELD") ? 0 : 1);
            return 0u;
        };
    });
});

ServiceRegistrar r_aoc("aoc:u", [] {
    return make("aoc:u", [](Service& s) {
        s.cmds[2] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // CountAddOnContent (old)
        s.cmds[3] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // ListAddOnContent (old)
        s.cmds[7] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };
        s.cmds[8] = [](IpcCtx& c) {                                       // GetAddOnContentListChangedEvent
            auto e = std::make_shared<KEvent>();
            e->state = std::make_shared<EventState>();
            c.out_copy.push_back(handle_add(e));
            return 0u;
        };
    });
});

ServiceRegistrar r_ngc("ngc:u", [] {
    return make("ngc:u", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push<uint32_t>(1); return 0u; };  // GetContentVersion
        s.cmds[1] = [](IpcCtx& c) { c.push<uint32_t>(0); return 0u; };  // Check (0 = no profanity)
        s.cmds[2] = [](IpcCtx& c) {                                      // Mask (0 masked words)
            c.push<uint32_t>(0);
            auto in = c.in_buf(0), out = c.out_buf(0);
            if (in.addr && out.addr && in.addr != out.addr) {
                std::memcpy(reinterpret_cast<void*>(out.addr),
                            reinterpret_cast<const void*>(in.addr),
                            std::min(in.size, out.size));
            }
            return 0u;
        };
        s.cmds[3] = [](IpcCtx&) { return 0u; };                          // Reload
    });
});

ServiceRegistrar r_ngct("ngct:u", [] {
    return make("ngct:u", [](Service& s) {
        s.cmds[0] = [](IpcCtx& c) { c.push<uint8_t>(0); return 0u; };  // Match (false = clean)
        s.cmds[1] = [](IpcCtx& c) {                                     // Filter
            auto in = c.in_buf(0), out = c.out_buf(0);
            if (in.addr && out.addr && in.addr != out.addr) {
                std::memcpy(reinterpret_cast<void*>(out.addr),
                            reinterpret_cast<const void*>(in.addr),
                            std::min(in.size, out.size));
            }
            return 0u;
        };
    });
});

ServiceRegistrar r_fspsrv("fsp-srv", [] {
    return make("fsp-srv", [](Service& s) {
        s.cmds[1] = [](IpcCtx&) { return 0u; };  // Initialize / SetCurrentProcess
        s.cmds[200] = [](IpcCtx& c) {            // OpenDataStorageByCurrentProcess
            auto storage = make("IStorage", [](Service& st) {
                st.cmds[0] = [](IpcCtx& c) {     // Read(s64 offset, s64 size)
                    auto out = c.out_buf(0);
                    if (out.addr && out.size) std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
                    return 0u;
                };
                st.cmds[4] = [](IpcCtx& c) { c.push<int64_t>(0x10000); return 0u; };  // GetSize
            });
            c.push_object(storage);
            return 0u;
        };
        s.cmds[202] = [](IpcCtx& c) {            // OpenDataStorageBySystemDataId
            auto storage = make("IStorage", [](Service& st) {
                st.cmds[0] = [](IpcCtx& c) {     // Read(s64 offset, s64 size)
                    auto out = c.out_buf(0);
                    if (out.addr && out.size) std::memset(reinterpret_cast<void*>(out.addr), 0, out.size);
                    return 0u;
                };
                st.cmds[4] = [](IpcCtx& c) { c.push<int64_t>(0x10000); return 0u; };  // GetSize
            });
            c.push_object(storage);
            return 0u;
        };
    });
});

void NgcOk(CPU& c) { c.x[0] = 0; }

void NgcGetContentVersion(CPU& c) {
    if (c.x[1]) st<uint32_t>(c.x[1], 1u);
    c.x[0] = 0;
}

void NgcCheckProfanityWords(CPU& c) {
    // nn::Result ProfanityFilter::CheckProfanityWords(u32* outCheckResults, const CharT* const* words, size_t wordCount, ...)
    // Note: In some overloads wordCount is in x[2], in others words is in x[2] and wordCount is in x[3].
    // Check both x[2] and x[3]: if x[2] <= 64, count = x[2]; else if x[3] <= 64, count = x[3].
    uint64_t out_ptr = c.x[1];
    uint64_t count = (c.x[2] > 0 && c.x[2] <= 64) ? c.x[2] : ((c.x[3] > 0 && c.x[3] <= 64) ? c.x[3] : 1);
    if (out_ptr) {
        for (uint64_t i = 0; i < count; i++)
            st<uint32_t>(out_ptr + i * 4, 0u);
    }
    c.x[0] = 0;
}

void NgcMaskProfanityWordsInText(CPU& c) {
    // nn::Result ProfanityFilter::MaskProfanityWordsInText(int* outProfanityWordCount, CharT* inOutText, ...)
    // x[0] = this, x[1] = int* outProfanityWordCount (optional, can be nullptr!), x[2] = CharT* inOutText
    // Only write 0 to x[1] if x[1] is a valid pointer and NOT the text buffer itself
    if (c.x[1] && c.x[1] != c.x[2]) st<int32_t>(c.x[1], 0);
    c.x[0] = 0;
}

void NgcCountNumbers(CPU& c) {
    c.x[0] = 0;
}

void FsQueryMountSystemDataCacheSize(CPU& c) {
    // nn::Result nn::fs::QueryMountSystemDataCacheSize(size_t* outSize, nn::ncm::SystemDataId id)
    if (c.x[0]) st<uint64_t>(c.x[0], 0x1000ull);
    c.x[0] = 0;
}

NX_HLE("_ZN2nn3ngc15ProfanityFilterC1Ev", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilterC2Ev", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilterC1Eb", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilterC2Eb", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilterD1Ev", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilterD2Ev", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilterD0Ev", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvj", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvjb", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmPKc", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvjPKc", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilter8FinalizeEv", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEPj", NgcGetContentVersion);
NX_HLE("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEPm", NgcGetContentVersion);
NX_HLE("_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", NgcOk);
NX_HLE("_ZN2nn3ngc15ProfanityFilter11SetSkipModeENS1_19SkipAtSignCheckModeE", NgcOk);

// ContentsReader & MountSystemData safety guards
NX_HLE("_ZN2nn3ngc6detail14ContentsReader10InitializeEPvm", NgcOk);
NX_HLE("_ZN2nn3ngc6detail14ContentsReader13MountContentsEPvm", NgcOk);
NX_HLE("_ZN2nn3ngc6detail14ContentsReader13MountContentsEPvj", NgcOk);
NX_HLE("_ZN2nn3ngc6detail14ContentsReader15UnmountContentsEv", NgcOk);
NX_HLE("_ZN2nn3ngc6detail14ContentsReader8FinalizeEv", NgcOk);
NX_HLE("_ZN2nn2fs29QueryMountSystemDataCacheSizeEPmNS_3ncm12SystemDataIdE", FsQueryMountSystemDataCacheSize);
NX_HLE("_ZN2nn2fs15MountSystemDataEPKcNS_3ncm12SystemDataIdEPvm", NgcOk);

NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKcj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKcm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKcjj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKcjm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKcmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKcmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPmPKPKcmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKcj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKcm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKcjj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKcmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKcmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKtj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKtm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKtjj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKtjm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKtmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKtmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKtj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKtm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKtjj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKtmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKtmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKtj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKtm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKtjj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKtmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKtmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKPKtj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKPKtm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKPKtmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKPKtmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKDsj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKDsm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKDsjj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKDsjm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKDsmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKDsmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKDsj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKDsm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKDsjj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKDsmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKDsmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKDsj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKDsm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKDsjj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKDsmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjPKPKPKDsmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKPKDsj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKPKDsm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKPKDsjj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKPKDsmj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPiPKPKPKDsmm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjmPPKcm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjmPPKDsm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPijPPKcm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPijPPKcj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPijPPKDsm", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPijPPKDsj", NgcCheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPc", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcjj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcjm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcmj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcmm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPlPcj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPlPcm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPlPcmm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPcj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPcm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPcjj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPcmj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPt", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtjj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtjm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtmj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtmm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPlPtj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPlPtm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPlPtmm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPtj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPtm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPtjj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPtmj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDs", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsjj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsjm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsmj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsmm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPlPDsj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPlPDsm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPlPDsmm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPDsj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPDsm", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPDsjj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPjPDsmj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPjPKcmj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPjPKtmj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPjPKDsmj", NgcMaskProfanityWordsInText);
NX_HLE("_ZN2nn3ngc12CountNumbersEPKc", NgcCountNumbers);
NX_HLE("_ZN2nn3ngc12CountNumbersEPKt", NgcCountNumbers);
NX_HLE("_ZN2nn3ngc12CountNumbersEPKDs", NgcCountNumbers);

}  // namespace

}  // namespace nx
