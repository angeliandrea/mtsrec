#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "carico.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>

#include "json.h"

Carico::Carico() : filo_([this] { lavora(); }) {}

Carico::~Carico() {
    basta_ = true;
    if (filo_.joinable()) filo_.join();
}

double Carico::cpu() const {
    std::lock_guard<std::mutex> l(m_);
    return cpu_;
}

void Carico::azzera_statistica() {
    std::lock_guard<std::mutex> l(m_);
    somma_cpu_ = max_cpu_ = max_ram_ = 0;
    max_gpu_ = -1;
    campioni_ = 0;
}

std::string Carico::statistica_json() const {
    std::lock_guard<std::mutex> l(m_);
    return "{\"cpu_media\":" + json_num(campioni_ ? somma_cpu_ / campioni_ : 0, 1) + ",\"cpu_max\":" + json_num(max_cpu_, 1) +
           ",\"gpu_max\":" + json_num(max_gpu_, 1) + ",\"ram_max\":" + json_num(max_ram_, 1) +
           ",\"campioni\":" + std::to_string(campioni_) + "}";
}

std::string Carico::json() const {
    std::lock_guard<std::mutex> l(m_);
    return "{\"cpu\":" + json_num(cpu_, 1) + ",\"gpu\":" + json_num(gpu_, 1) + ",\"ram\":" + json_num(ram_, 1) +
           ",\"ram_usata_gb\":" + json_num(ram_usata_, 1) + ",\"ram_gb\":" + json_num(ram_tot_, 1) + "}";
}

void Carico::lavora() {
    // La scheda video la dà Windows coi contatori «GPU Engine», uno per motore di ogni processo:
    // come Gestione attività, si somma per tipo di motore e si prende il tipo più carico.
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER c = nullptr;
    const bool gpu = PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS &&
                     PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) == ERROR_SUCCESS;
    if (gpu) PdhCollectQueryData(q);
    auto v = [](FILETIME f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    FILETIME i, k, u;
    GetSystemTimes(&i, &k, &u);
    ULONGLONG ri = v(i), rk = v(k), ru = v(u);
    std::vector<BYTE> buf;
    while (!basta_) {
        for (int n = 0; n < 5 && !basta_; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        GetSystemTimes(&i, &k, &u);
        const ULONGLONG di = v(i) - ri, dk = v(k) - rk, du = v(u) - ru;   // il kernel comprende l'inattività
        ri = v(i); rk = v(k); ru = v(u);
        const double cpu = dk + du ? 100.0 * static_cast<double>(dk + du - di) / static_cast<double>(dk + du) : 0.0;

        MEMORYSTATUSEX m{sizeof m};
        GlobalMemoryStatusEx(&m);

        double g = -1;
        if (gpu && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            DWORD byte = 0, quanti = 0;
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &byte, &quanti, nullptr) == PDH_MORE_DATA) {
                buf.resize(byte);
                auto* a = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
                if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &byte, &quanti, a) == ERROR_SUCCESS) {
                    std::map<std::wstring, double> per_tipo;   // «engtype_3D», «engtype_VideoEncode»…
                    for (DWORD x = 0; x < quanti; ++x) {
                        if (a[x].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && a[x].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                        const std::wstring nome = a[x].szName;
                        const size_t p = nome.find(L"engtype_");
                        per_tipo[p == std::wstring::npos ? L"?" : nome.substr(p)] += a[x].FmtValue.doubleValue;
                    }
                    g = 0;
                    for (const auto& [t, val] : per_tipo) g = std::max(g, val);
                    g = std::min(g, 100.0);
                }
            }
        }
        std::lock_guard<std::mutex> l(m_);
        cpu_ = cpu;
        gpu_ = g;
        ram_ = m.dwMemoryLoad;
        ram_tot_ = m.ullTotalPhys / 1073741824.0;
        ram_usata_ = (m.ullTotalPhys - m.ullAvailPhys) / 1073741824.0;
        somma_cpu_ += cpu;
        ++campioni_;
        max_cpu_ = std::max(max_cpu_, cpu);
        max_gpu_ = std::max(max_gpu_, g);
        max_ram_ = std::max(max_ram_, ram_);
    }
    if (q) PdhCloseQuery(q);
}
