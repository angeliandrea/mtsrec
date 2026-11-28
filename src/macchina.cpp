#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <cfgmgr32.h>
#include <dxgi.h>
#include <setupapi.h>
#include <winioctl.h>
#include <shlobj.h>

#include "macchina.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <vector>

#include "DeckLinkAPI_h.h"
#include "controlli.h"
#include "formato.h"
#include "impostazioni.h"
#include "json.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace fs = std::filesystem;

static const char* NOMI_PASSI[10] = {
    "Operating system and CPU", "Memory and GPUs", "PCIe cards and links",
    "Desktop Video and DeckLink cards", "Drives", "Network: ATEM, HyperDeck, page",
    "Windows settings", "Package", "DNxHD encoding test", "Drive write test"};

// ------------------------------------------------------------------ piccoli aiuti
static std::string utf8(const wchar_t* s) {
    if (!s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string r(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r.data(), n, nullptr, nullptr);
    return r;
}

static std::wstring largo(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring r(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, r.data(), n);
    return r;
}

static std::string bstr(BSTR s) {
    std::string r = utf8(s);
    SysFreeString(s);
    return r;
}

static std::string minuscolo(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string registro(const wchar_t* chiave, const wchar_t* valore) {
    wchar_t b[512];
    DWORD n = sizeof b;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, chiave, valore, RRF_RT_REG_SZ, nullptr, b, &n) != ERROR_SUCCESS) return {};
    std::string r = utf8(b);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

static bool esiste(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Fino alla 0.9 beta 5 il programma si chiamava WinISORecorder e teneva i suoi dati in
// %USERPROFILE%\WinISORecorder (sezione 9.81). La prima volta la cartella vecchia si rinomina in quella
// nuova, così impostazioni, setup, mappa delle porte e rapporti restano. Se non si può rinominare
// (un WinISORecorder ancora aperto la tiene occupata) si riparte da una cartella nuova e la vecchia resta.
std::wstring cartella_utente() {
    std::wstring c = L".";
    if (const wchar_t* home = _wgetenv(L"USERPROFILE")) {
        c = std::wstring(home) + L"\\mtsrec";
        const std::wstring vecchia = std::wstring(home) + L"\\WinISORecorder";
        static bool provato = false;
        if (!provato) {
            provato = true;
            if (!esiste(c) && esiste(vecchia)) {
                if (MoveFileExW(vecchia.c_str(), c.c_str(), 0))
                    std::printf("settings moved from %%USERPROFILE%%\\WinISORecorder to %%USERPROFILE%%\\mtsrec\n");
                else
                    std::printf("⚠ %%USERPROFILE%%\\WinISORecorder could not be moved (error %lu): starting with empty settings\n",
                                GetLastError());
            }
        }
    }
    std::error_code ec;
    fs::create_directories(c, ec);
    return c;
}

// Il setup Inno di WinISORecorder (fino alla 0.9 beta 5) lasciava i collegamenti nel menu Start e sul
// desktop. L'MSI toglie programma e voce delle app installate, ma la cartella del menu Start non riusciva a
// toglierla: nelle prove del 18 novembre 2026 la sua azione non la vedeva (sezione 9.81). Lo fa il programma
// al primo avvio, e solo se il WinISORecorder vecchio non c'è più: toglie i soli WinISORecorder*.lnk e la
// cartella se resta vuota. Niente altro.
void togli_collegamenti_vecchi() {
    const wchar_t* locale = _wgetenv(L"LOCALAPPDATA");
    if (!locale || esiste(std::wstring(locale) + L"\\Programs\\WinISORecorder\\WinISORecorder.exe")) return;
    auto togli = [](const std::wstring& cartella, bool anche_cartella) {
        WIN32_FIND_DATAW d;
        HANDLE h = FindFirstFileW((cartella + L"\\WinISORecorder*.lnk").c_str(), &d);
        if (h == INVALID_HANDLE_VALUE) return;
        int tolti = 0;
        do {
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && DeleteFileW((cartella + L"\\" + d.cFileName).c_str())) ++tolti;
        } while (FindNextFileW(h, &d));
        FindClose(h);
        if (anche_cartella) RemoveDirectoryW(cartella.c_str());   // solo se è rimasta vuota
        if (tolti) std::printf("%d old WinISORecorder shortcut(s) removed\n", tolti);
    };
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p))) togli(std::wstring(p) + L"\\WinISORecorder", true);
    CoTaskMemFree(p);
    p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) togli(p, false);
    CoTaskMemFree(p);
}

std::wstring cartella_programma() {
    wchar_t exe[MAX_PATH];
    std::wstring c(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
    return c.substr(0, c.find_last_of(L'\\'));
}

// ------------------------------------------------------------------ sistema
struct Sistema {
    std::string windows, processore;
    int nuclei = 0, fili = 0, build = 0;
    double memoria_gb = 0;
    struct Video { std::string nome; double memoria_gb; };
    std::vector<Video> video;
};

static Sistema leggi_sistema() {
    Sistema s;
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    s.build = std::atoi(registro(nt, L"CurrentBuildNumber").c_str());
    s.windows = registro(nt, L"ProductName");
    // Windows 11 nel registro si chiama ancora «Windows 10»: lo dice il numero di build.
    if (s.build >= 22000 && s.windows.find("Windows 10") == 0) s.windows.replace(8, 2, "11");
    if (const std::string v = registro(nt, L"DisplayVersion"); !v.empty()) s.windows += " " + v;
    s.windows += " (build " + std::to_string(s.build) + ")";
    s.processore = registro(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    s.fili = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    DWORD n = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &n);
    std::vector<char> b(n);
    if (n && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                              reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(b.data()), &n))
        for (DWORD o = 0; o < n;) {
            auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(b.data() + o);
            ++s.nuclei;
            o += p->Size;
        }
    MEMORYSTATUSEX m{sizeof m};
    if (GlobalMemoryStatusEx(&m)) s.memoria_gb = m.ullTotalPhys / 1073741824.0;
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                s.video.push_back({utf8(d.Description), d.DedicatedVideoMemory / 1073741824.0});
            a->Release();
        }
        f->Release();
    }
    return s;
}

// ------------------------------------------------------------------ PCIe
// Le schede sul bus PCI con il collegamento che hanno davvero: una scheda di acquisizione in uno slot
// più stretto del suo, o con le linee condivise, porta meno di quello che promette.
struct SchedaPci {
    std::string nome, produttore, classe, venditore;
    int base = -1, larg = -1, larg_max = -1, vel = -1, vel_max = -1;
    int problema = 0;                  // codice di Gestione dispositivi; 28 = driver non installato
    bool acquisizione = false;
};

static DEVPROPKEY chiave_pci(ULONG pid) {
    return {{0x3ab22e31, 0x8264, 0x4b4e, {0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62}}, pid};
}

static double gts(int v) {             // il valore di Windows → GT/s per linea
    static const double t[] = {0, 2.5, 5, 8, 16, 32, 64};
    return v > 0 && v < 7 ? t[v] : 0;
}

static std::vector<SchedaPci> leggi_pci() {
    std::vector<SchedaPci> out;
    HDEVINFO h = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA d{sizeof d};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &d); ++i) {
        auto testo = [&](DWORD p) {
            wchar_t b[1024] = {};
            return SetupDiGetDeviceRegistryPropertyW(h, &d, p, nullptr, reinterpret_cast<BYTE*>(b), sizeof b - 2, nullptr)
                       ? utf8(b) : std::string();
        };
        auto intero = [&](ULONG pid) {
            const DEVPROPKEY k = chiave_pci(pid);
            DEVPROPTYPE t = 0;
            BYTE v[8] = {};
            if (!SetupDiGetDevicePropertyW(h, &d, &k, &t, v, sizeof v, nullptr, 0)) return -1;
            return t == DEVPROP_TYPE_BYTE ? static_cast<int>(v[0]) : static_cast<int>(*reinterpret_cast<UINT32*>(v));
        };
        SchedaPci s;
        s.nome = testo(SPDRP_FRIENDLYNAME);
        if (s.nome.empty()) s.nome = testo(SPDRP_DEVICEDESC);
        s.produttore = testo(SPDRP_MFG);
        s.classe = testo(SPDRP_CLASS);
        const std::string id = testo(SPDRP_HARDWAREID);   // PCI\VEN_1EDB&DEV_…
        if (const size_t v = id.find("VEN_"); v != std::string::npos) s.venditore = id.substr(v + 4, 4);
        s.base = intero(3);
        const int sotto = intero(4);
        s.vel = intero(9); s.larg = intero(10); s.vel_max = intero(11); s.larg_max = intero(12);
        ULONG stato = 0, problema = 0;
        if (CM_Get_DevNode_Status(&stato, &problema, d.DevInst, 0) == CR_SUCCESS && (stato & DN_HAS_PROBLEM))
            s.problema = static_cast<int>(problema);
        const std::string tutto = minuscolo(s.nome + " " + s.produttore);
        // Blackmagic e AJA dal codice del produttore, che c'è anche senza driver; gli altri dal nome,
        // o dalla classe multimediale video. Le sottoclassi audio (01, 03) sono le uscite audio
        // delle schede video e della scheda madre: non acquisiscono niente.
        s.acquisizione = s.venditore == "1EDB" || s.venditore == "F1D0" || (s.base == 4 && (sotto == 0 || sotto == 0x80));
        for (const char* m : {"blackmagic", "decklink", "aja", "magewell", "deltacast", "bluefish", "datapath",
                              "yuan", "osprey", "matrox", "avermedia", "elgato"})
            if (tutto.find(m) != std::string::npos) s.acquisizione = true;
        if (s.produttore.empty() && s.venditore == "1EDB") s.produttore = "Blackmagic Design";
        if (s.produttore.empty() && s.venditore == "F1D0") s.produttore = "AJA Video Systems";
        // Ponti e periferiche di sistema non interessano: schede video, acquisizione, dischi, rete, e chi non ha driver.
        const bool conta = s.acquisizione || s.base == 1 || s.base == 2 || s.base == 3 || s.problema;
        if (conta) out.push_back(s);
    }
    SetupDiDestroyDeviceInfoList(h);
    return out;
}

// ------------------------------------------------------------------ DeckLink
struct PortaDL {
    std::string modello, nome, connessioni, interfaccia;
    bool ingresso = false, uscita = false, rileva_formato = false, p25 = false, i50 = false, u25 = false;
    long long canali_audio = 0;
};
struct DeckLink { bool driver = false; std::string versione; std::vector<PortaDL> porte; };

static DeckLink leggi_decklink() {
    DeckLink r;
    IDeckLinkAPIInformation* info = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_CDeckLinkAPIInformation, nullptr, CLSCTX_ALL, IID_IDeckLinkAPIInformation,
                                   reinterpret_cast<void**>(&info)))) {
        BSTR v = nullptr;
        if (SUCCEEDED(info->GetString(BMDDeckLinkAPIVersion, &v))) r.versione = bstr(v);
        info->Release();
    }
    IDeckLinkIterator* it = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator,
                                reinterpret_cast<void**>(&it))))
        return r;
    r.driver = true;
    IDeckLink* d = nullptr;
    while (it->Next(&d) == S_OK) {
        PortaDL p;
        BSTR s = nullptr;
        if (SUCCEEDED(d->GetModelName(&s))) p.modello = bstr(s);
        if (SUCCEEDED(d->GetDisplayName(&s))) p.nome = bstr(s);
        IDeckLinkProfileAttributes* a = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&a)))) {
            LONGLONG v = 0;
            BOOL f = FALSE;
            if (SUCCEEDED(a->GetInt(BMDDeckLinkVideoIOSupport, &v))) {
                p.ingresso = (v & bmdDeviceSupportsCapture) != 0;
                p.uscita = (v & bmdDeviceSupportsPlayback) != 0;
            }
            if (SUCCEEDED(a->GetInt(BMDDeckLinkVideoInputConnections, &v))) {
                if (v & bmdVideoConnectionSDI) p.connessioni = "SDI";
                if (v & bmdVideoConnectionHDMI) p.connessioni += p.connessioni.empty() ? "HDMI" : " and HDMI";
            }
            if (SUCCEEDED(a->GetInt(BMDDeckLinkDeviceInterface, &v)))
                p.interfaccia = v == bmdDeviceInterfacePCI ? "PCIe" : v == bmdDeviceInterfaceUSB ? "USB"
                              : v == bmdDeviceInterfaceThunderbolt ? "Thunderbolt" : "?";
            if (SUCCEEDED(a->GetInt(BMDDeckLinkMaximumAudioChannels, &v))) p.canali_audio = v;
            if (SUCCEEDED(a->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &f))) p.rileva_formato = f;
            a->Release();
        }
        IDeckLinkInput* in = nullptr;
        if (p.ingresso && SUCCEEDED(d->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void**>(&in)))) {
            BMDDisplayMode effettivo;
            BOOL si = FALSE;
            if (SUCCEEDED(in->DoesSupportVideoMode(bmdVideoConnectionUnspecified, bmdModeHD1080p25, bmdFormat8BitYUV,
                                                   bmdNoVideoInputConversion, bmdSupportedVideoModeDefault, &effettivo, &si)))
                p.p25 = si;
            if (SUCCEEDED(in->DoesSupportVideoMode(bmdVideoConnectionUnspecified, bmdModeHD1080i50, bmdFormat8BitYUV,
                                                   bmdNoVideoInputConversion, bmdSupportedVideoModeDefault, &effettivo, &si)))
                p.i50 = si;
            // UHD (sezione 9.74): solo le porte 12G-SDI e le HDMI 2.0 lo ricevono
            if (SUCCEEDED(in->DoesSupportVideoMode(bmdVideoConnectionUnspecified, bmdMode4K2160p25, bmdFormat8BitYUV,
                                                   bmdNoVideoInputConversion, bmdSupportedVideoModeDefault, &effettivo, &si)))
                p.u25 = si;
            in->Release();
        }
        r.porte.push_back(p);
        d->Release();
    }
    it->Release();
    return r;
}

// ------------------------------------------------------------------ dischi
struct Disco {
    std::string lettera, etichetta, file_system, tipo, bus, modello;
    double totale_gb = 0, libero_gb = 0, misurato_mb_s = 0;
    bool sistema = false;
    unsigned long seriale = 0;
};

static std::wstring file_dischi() { return cartella_utente() + L"\\dischi.json"; }

static std::string chiave_disco(const Disco& d) {
    char b[32];
    std::snprintf(b, sizeof b, "%s|%08lX", d.lettera.c_str(), d.seriale);
    return b;
}

static std::vector<Disco> leggi_dischi() {
    std::vector<Disco> out;
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::string misure = leggi_file(file_dischi());
    const DWORD maschera = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(maschera & (1u << i))) continue;
        const wchar_t lettera = static_cast<wchar_t>(L'A' + i);
        const std::wstring radice = std::wstring(1, lettera) + L":\\";
        const UINT tipo = GetDriveTypeW(radice.c_str());
        if (tipo != DRIVE_FIXED && tipo != DRIVE_REMOVABLE && tipo != DRIVE_REMOTE) continue;
        Disco d;
        d.lettera = utf8(std::wstring(1, lettera).c_str()) + ":";
        d.sistema = towupper(win[0]) == lettera;
        d.tipo = tipo == DRIVE_REMOTE ? "rete" : tipo == DRIVE_REMOVABLE ? "rimovibile" : "interno";
        wchar_t et[MAX_PATH] = {}, fsn[MAX_PATH] = {};
        DWORD ser = 0;
        if (!GetVolumeInformationW(radice.c_str(), et, MAX_PATH, &ser, nullptr, nullptr, fsn, MAX_PATH)) continue;
        d.etichetta = utf8(et);
        d.file_system = utf8(fsn);
        d.seriale = ser;
        ULARGE_INTEGER libero{}, totale{};
        if (GetDiskFreeSpaceExW(radice.c_str(), &libero, &totale, nullptr)) {
            d.totale_gb = totale.QuadPart / 1e9;
            d.libero_gb = libero.QuadPart / 1e9;
        }
        if (tipo != DRIVE_REMOTE) {
            // Il modello e il bus li dice il disco stesso; basta aprirlo senza diritti di lettura.
            const std::wstring vol = L"\\\\.\\" + std::wstring(1, lettera) + L":";
            HANDLE h = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                STORAGE_PROPERTY_QUERY q{StorageDeviceProperty, PropertyStandardQuery, {0}};
                std::vector<BYTE> b(1024);
                DWORD n = 0;
                if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, b.data(), static_cast<DWORD>(b.size()), &n, nullptr)) {
                    auto* sd = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(b.data());
                    switch (sd->BusType) {
                        case BusTypeNvme: d.bus = "NVMe"; break;
                        case BusTypeSata: d.bus = "SATA"; break;
                        case BusTypeUsb: d.bus = "USB"; break;
                        case BusTypeSas: d.bus = "SAS"; break;
                        case BusTypeRAID: d.bus = "RAID"; break;
                        case BusTypeScsi: d.bus = "SCSI"; break;
                        case BusTypeSpaces: d.bus = "Storage Spaces"; break;
                        default: d.bus = "other"; break;
                    }
                    std::string m;
                    if (sd->VendorIdOffset) m = reinterpret_cast<const char*>(b.data() + sd->VendorIdOffset);
                    if (sd->ProductIdOffset) m += reinterpret_cast<const char*>(b.data() + sd->ProductIdOffset);
                    while (!m.empty() && m.back() == ' ') m.pop_back();
                    while (!m.empty() && m.front() == ' ') m.erase(0, 1);
                    d.modello = m;
                }
                CloseHandle(h);
            }
        }
        d.misurato_mb_s = std::atof(json_valore(misure, chiave_disco(d)).c_str());
        out.push_back(d);
    }
    return out;
}

static std::string json_dischi(const std::vector<Disco>& v) {
    std::string r = "[";
    for (size_t k = 0; k < v.size(); ++k) {
        const Disco& d = v[k];
        r += std::string(k ? "," : "") + "{\"lettera\":" + json_testo(d.lettera) + ",\"etichetta\":" + json_testo(d.etichetta) +
             ",\"tipo\":" + json_testo(d.tipo) + ",\"bus\":" + json_testo(d.bus) + ",\"modello\":" + json_testo(d.modello) +
             ",\"file_system\":" + json_testo(d.file_system) + ",\"totale_gb\":" + json_num(d.totale_gb, 1) +
             ",\"libero_gb\":" + json_num(d.libero_gb, 1) + ",\"misurato_mb_s\":" + json_num(d.misurato_mb_s) +
             ",\"sistema\":" + json_si(d.sistema) + "}";
    }
    return r + "]";
}

std::string dischi_json() { return json_dischi(leggi_dischi()); }

double velocita_misurata(const std::string& cartella) {
    if (cartella.size() < 2 || cartella[1] != ':') return 0;          // in rete: si misura ogni volta
    for (const Disco& d : leggi_dischi())
        if (minuscolo(cartella.substr(0, 2)) == minuscolo(d.lettera)) return d.misurato_mb_s;
    return 0;
}

double misura_disco(const std::string& cartella) {
    ParametriCodifica pc;
    pc.canali_audio = 2;
    pc.wav = true;
    // Dieci file insieme come dieci tracce, cinque secondi, al più 20 GB: la cartella resta com'era.
    const double bs = disco_al_massimo(cartella, 10, pc, 5, 20'000'000'000LL);
    const std::u8string a = fs::absolute(fs::u8path(cartella)).u8string();
    const std::string assoluta(a.begin(), a.end());
    for (Disco& d : leggi_dischi()) {
        if (assoluta.size() < 2 || minuscolo(assoluta.substr(0, 2)) != minuscolo(d.lettera)) continue;
        std::string j = leggi_file(file_dischi());
        if (j.empty()) j = "{\n}\n";
        const std::string k = "\"" + chiave_disco(d) + "\"";
        // Una riga per disco: quella vecchia si toglie, la nuova va in testa.
        if (const size_t p = j.find(k); p != std::string::npos) {
            const size_t fine = j.find('\n', p);
            size_t inizio = j.rfind('\n', p);
            j.erase(inizio, fine - inizio);
        }
        const bool vuoto = j.find(':') == std::string::npos;
        j.insert(j.find('{') + 1, "\n  " + k + ": " + json_num(bs / 1e6) + (vuoto ? "" : ","));
        scrivi_file(file_dischi(), j);
    }
    return bs / 1e6;
}

// ------------------------------------------------------------------ rete
struct SchedaRete { std::string nome; std::vector<std::pair<unsigned long, int>> ip; };   // indirizzo e prefisso

static std::vector<SchedaRete> leggi_rete() {
    std::vector<SchedaRete> out;
    ULONG n = 32768;
    std::vector<BYTE> b(n);
    auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(b.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &n) != NO_ERROR)
        return out;
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        SchedaRete s;
        s.nome = utf8(a->FriendlyName);
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            s.ip.push_back({ntohl(sin->sin_addr.s_addr), u->OnLinkPrefixLength});
        }
        if (!s.ip.empty()) out.push_back(s);
    }
    return out;
}

static std::string ip_testo(unsigned long ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." +
           std::to_string(ip & 255);
}

static bool stessa_rete(const std::vector<SchedaRete>& v, const std::string& ip) {
    in_addr x{};
    if (ip.empty() || inet_pton(AF_INET, ip.c_str(), &x) != 1) return false;
    const unsigned long h = ntohl(x.s_addr);
    for (const auto& s : v)
        for (const auto& [a, pref] : s.ip) {
            const unsigned long m = pref ? 0xFFFFFFFFul << (32 - pref) : 0;
            if ((a & m) == (h & m)) return true;
        }
    return false;
}

// L'ATEM risponde al saluto UDP sulla 9910: si manda, si aspetta la risposta, e la sessione mezza
// aperta la chiude lui da solo. Non cambia niente nel mixer.
static bool atem_risponde(const std::string& ip) {
    static const bool pronto = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }();
    if (!pronto || ip.empty()) return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(9910);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) { closesocket(s); return false; }
    const unsigned char saluto[20] = {0x10, 0x14, 0x53, 0xAB, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    DWORD attesa = 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&attesa), sizeof attesa);
    sendto(s, reinterpret_cast<const char*>(saluto), sizeof saluto, 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
    char b[64];
    const bool ok = recv(s, b, sizeof b, 0) > 0;
    closesocket(s);
    return ok;
}

static bool tcp_risponde(const std::string& ip, int porta) {
    static const bool pronto = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }();
    if (!pronto || ip.empty()) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(porta));
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) { closesocket(s); return false; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s, &w); FD_SET(s, &e);
    timeval t{1, 500000};
    const bool ok = select(0, nullptr, &w, &e, &t) > 0 && FD_ISSET(s, &w);
    closesocket(s);
    return ok;
}

// Gli indirizzi delle reti locali di questa macchina, lei esclusa. Solo reti piccole, al più /22:
// una rete più grande non è quella di una regia, e salutarla tutta sarebbe rumore.
static std::vector<unsigned long> vicini() {
    std::vector<unsigned long> out;
    for (const auto& s : leggi_rete())
        for (const auto& [a, pref] : s.ip) {
            if (pref < 22 || pref > 30 || (a >> 16) == 0xA9FE) continue;           // niente 169.254: Windows senza rete
            const unsigned long m = 0xFFFFFFFFul << (32 - pref), base = a & m, fine = base | ~m;
            for (unsigned long h = base + 1; h < fine; ++h)
                if (h != a) out.push_back(h);
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Apparecchi cerca_apparecchi() {
    static const bool pronto = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }();
    Apparecchi r;
    if (!pronto) return r;
    const std::vector<unsigned long> v = vicini();
    auto indirizzo = [](unsigned long h, int porta) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(porta));
        a.sin_addr.s_addr = htonl(h);
        return a;
    };
    // ATEM: lo stesso saluto di atem_risponde a tutti, poi si ascolta chi risponde dalla 9910.
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const unsigned char saluto[20] = {0x10, 0x14, 0x53, 0xAB, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    for (unsigned long h : v) {
        const sockaddr_in a = indirizzo(h, 9910);
        sendto(u, reinterpret_cast<const char*>(saluto), sizeof saluto, 0, reinterpret_cast<const sockaddr*>(&a), sizeof a);
    }
    const auto fino = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < fino) {
        fd_set f;
        FD_ZERO(&f);
        FD_SET(u, &f);
        timeval t{0, 100000};
        if (select(0, &f, nullptr, nullptr, &t) <= 0) continue;
        char b[64];
        sockaddr_in da{};
        int n = sizeof da;
        if (recvfrom(u, b, sizeof b, 0, reinterpret_cast<sockaddr*>(&da), &n) <= 0 || ntohs(da.sin_port) != 9910) continue;
        const std::string ip = ip_testo(ntohl(da.sin_addr.s_addr));
        if (std::find(r.atem.begin(), r.atem.end(), ip) == r.atem.end()) r.atem.push_back(ip);
    }
    closesocket(u);
    // HyperDeck: un collegamento sulla 9993, a gruppi di 60 perché select su Windows ne regge 64.
    for (size_t i = 0; i < v.size(); i += 60) {
        std::vector<std::pair<SOCKET, unsigned long>> g;
        for (size_t k = i; k < std::min(v.size(), i + 60); ++k) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            const sockaddr_in a = indirizzo(v[k], 9993);
            connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof a);
            g.push_back({s, v[k]});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));        // in rete locale basta e avanza
        fd_set w;
        FD_ZERO(&w);
        for (const auto& [s, h] : g) FD_SET(s, &w);
        timeval t{0, 0};
        if (select(0, nullptr, &w, nullptr, &t) > 0)
            for (const auto& [s, h] : g) {
                int err = 0, n = sizeof err;
                if (FD_ISSET(s, &w) && getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &n) == 0 && !err)
                    r.hyperdeck.push_back(ip_testo(h));
            }
        for (const auto& [s, h] : g) closesocket(s);
    }
    return r;
}

// ------------------------------------------------------------------ impronta
std::string impronta_macchina() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Sistema s = leggi_sistema();
    const DeckLink dl = leggi_decklink();
    std::string r = s.processore + "|" + std::to_string(static_cast<int>(std::lround(s.memoria_gb))) + " GB";
    for (const auto& v : s.video) r += "|" + v.nome;
    r += "|Desktop Video " + dl.versione;
    for (const auto& p : dl.porte) r += "|" + p.nome;
    for (const auto& p : leggi_pci())
        if (p.acquisizione) r += "|" + p.nome + (p.problema ? " senza driver" : "");
    if (SUCCEEDED(co)) CoUninitialize();
    return r;
}

std::string profilo_salvato() { return leggi_file(cartella_utente() + L"\\macchina.json"); }

bool controllo_da_rifare(std::string& cosa) {
    const std::string p = profilo_salvato();
    if (p.empty()) { cosa = "first launch on this machine"; return true; }
    if (json_valore(p, "impronta") != impronta_macchina()) { cosa = "the machine has changed since the last check"; return true; }
    return false;
}

// ------------------------------------------------------------------ il controllo
Analisi::~Analisi() {
    if (filo_.joinable()) filo_.join();
}

void Analisi::avvia(const Impostazioni& imp) {
    if (in_corso_) return;
    if (filo_.joinable()) filo_.join();
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& p : passi_) { p[0] = "attesa"; p[1].clear(); p[2].clear(); }
        risultato_.clear();
    }
    in_corso_ = true;
    filo_ = std::thread([this, a = imp.atem, h = imp.hyperdeck, p = imp.porta] { lavora(a, h, p); });
}

void Analisi::passo(int n, const std::string& stato, const std::string& esito, const std::string& nota) {
    std::lock_guard<std::mutex> l(m_);
    passi_[n][0] = stato;
    passi_[n][1] = esito;
    passi_[n][2] = nota;
}

std::string Analisi::json() const {
    std::lock_guard<std::mutex> l(m_);
    if (!in_corso_ && !risultato_.empty()) return risultato_;
    std::string r = "{\"in_corso\":" + std::string(json_si(in_corso_)) + ",\"passi\":[";
    for (int k = 0; k < 10; ++k)
        r += std::string(k ? "," : "") + "{\"nome\":" + json_testo(NOMI_PASSI[k]) + ",\"stato\":" + json_testo(passi_[k][0]) +
             ",\"esito\":" + json_testo(passi_[k][1]) + ",\"nota\":" + json_testo(passi_[k][2]) + "}";
    return r + "]}";
}

std::string Analisi::verdetto() const {
    std::lock_guard<std::mutex> l(m_);
    return json_valore(risultato_.empty() ? profilo_salvato() : risultato_, "verdetto");
}

// Una cosa da fare: livello rosso (non gira), arancio (nessuna garanzia), giallo (consigliato), info.
struct DaFare { std::string livello, titolo; std::vector<std::string> come; };

static const char* peso(const std::string& livello) {
    return livello == "rosso" ? "obbligatorio" : livello == "arancio" ? "garanzia" : livello == "giallo" ? "consigliato" : "info";
}

void Analisi::lavora(std::string atem, std::string hyperdeck, int porta) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<DaFare> fare;
    auto esito_di = [&](size_t da) {      // il peggiore fra le cose da fare aggiunte da un passo
        std::string e = "verde";
        for (size_t k = da; k < fare.size(); ++k) {
            const std::string& l = fare[k].livello;
            if (l == "rosso") e = "rosso";
            else if (l == "arancio" && e != "rosso") e = "arancio";
            else if (l == "giallo" && e == "verde") e = "giallo";
        }
        return e;
    };
    char t[512];

    // 1. Sistema operativo e processore
    passo(0, "in corso");
    const Sistema sis = leggi_sistema();
    size_t da = fare.size();
    if (sis.build < 17763)
        fare.push_back({"arancio", "Windows version too old to guarantee operation",
                        {"The recorder is tested on up-to-date Windows 10 and 11",
                         "Update Windows from Settings › Windows Update"}});
    std::snprintf(t, sizeof t, "%s, %d cores, %d threads", sis.processore.c_str(), sis.nuclei, sis.fili);
    passo(0, "fatto", esito_di(da), t);

    // 2. Memoria e schede video
    passo(1, "in corso");
    da = fare.size();
    if (sis.memoria_gb < 7.5)
        fare.push_back({"arancio", "Low memory: " + json_num(sis.memoria_gb) + " GB",
                        {"Each track buffers one second of video, about 100 MB",
                         "Ten tracks need at least 16 GB, 32 GB recommended"}});
    else if (sis.memoria_gb < 15.5)
        fare.push_back({"giallo", "Memory at the limit: " + json_num(sis.memoria_gb) + " GB",
                        {"Enough for a few tracks", "32 GB recommended for ten tracks"}});
    std::string v;
    for (const auto& x : sis.video) v += (v.empty() ? "" : ", ") + x.nome;
    passo(1, "fatto", esito_di(da), json_num(sis.memoria_gb) + " GB memory" + (v.empty() ? "" : ", " + v));

    // 3. Schede PCIe e collegamenti
    passo(2, "in corso");
    const std::vector<SchedaPci> pci = leggi_pci();
    da = fare.size();
    int acquisizione = 0;
    for (const auto& p : pci) {
        if (!p.acquisizione) continue;
        ++acquisizione;
        if (p.problema == 28)
            fare.push_back({p.venditore == "1EDB" ? "rosso" : "info", p.nome + ": driver not installed",
                            p.venditore == "1EDB"
                                ? std::vector<std::string>{"Download Desktop Video from blackmagicdesign.com › Support",
                                                           "Install it and restart the computer", "Open the recorder again"}
                                : std::vector<std::string>{"The recorder uses DeckLink cards only: this card stays unused"}});
        else if (p.problema)
            fare.push_back({p.venditore == "1EDB" ? "rosso" : "info",
                            p.nome + ": Device Manager reports a problem (code " + std::to_string(p.problema) + ")",
                            {"Open Device Manager and check the card", "Reinstall the manufacturer's driver"}});
        if (p.larg > 0 && p.larg_max > 0 && p.larg < p.larg_max) {
            std::snprintf(t, sizeof t, "%s runs at x%d instead of x%d", p.nome.c_str(), p.larg, p.larg_max);
            fare.push_back({p.venditore == "1EDB" ? "arancio" : "info", t,
                            {"The slot is narrower than the card, or shares its lanes with another card",
                             "Move it to a slot that supports x" + std::to_string(p.larg_max) +
                                 ": the motherboard manual says which",
                             "With fewer lanes the inputs can drop frames"}});
        }
        const std::string tutto = minuscolo(p.nome + " " + p.produttore);
        const bool bm = p.venditore == "1EDB" || tutto.find("blackmagic") != std::string::npos || tutto.find("decklink") != std::string::npos;
        if (!bm && !p.problema)
            fare.push_back({"info", p.nome + (p.produttore.empty() ? "" : " by " + p.produttore) + ": not used",
                            {"The recorder captures only from Blackmagic DeckLink cards",
                             "This card can stay installed: it does not interfere"}});
    }
    std::snprintf(t, sizeof t, "%zu cards read, %d capture cards", pci.size(), acquisizione);
    passo(2, "fatto", esito_di(da), t);

    // 4. Desktop Video e DeckLink
    passo(3, "in corso");
    const DeckLink dl = leggi_decklink();
    da = fare.size();
    int ingressi = 0;
    for (const auto& p : dl.porte) ingressi += p.ingresso ? 1 : 0;
    if (!dl.driver)
        fare.push_back({"rosso", "Desktop Video is not installed, or is older than version 16",
                        {"Download Desktop Video 16 or later from blackmagicdesign.com › Support",
                         "Install it and restart the computer", "Open the recorder again"}});
    else if (dl.porte.empty())
        fare.push_back({"rosso", "No DeckLink card",
                        {"The recorder captures from Blackmagic DeckLink cards: without one it cannot record",
                         "If the card is installed, check that it appears in Desktop Video Setup"}});
    else if (!ingressi)
        fare.push_back({"rosso", "The DeckLink cards have no active inputs",
                        {"Open Desktop Video Setup and check the card profile: some connectors must be set as inputs",
                         "The recorder never changes profiles by itself"}});
    for (const auto& p : dl.porte) {
        if (!p.ingresso) continue;
        if (!p.p25 && !p.u25)
            fare.push_back({"giallo", p.nome + ": captures neither HD 1080 nor UHD 2160", {"This input will stay empty"}});
        if (p.interfaccia == "USB" || p.interfaccia == "Thunderbolt")
            fare.push_back({"info", p.nome + " is connected via " + p.interfaccia,
                            {"It works, but the cable and connection must stay undisturbed for the whole live event"}});
    }
    std::snprintf(t, sizeof t, "Desktop Video %s, %zu ports, %d inputs", dl.versione.empty() ? "not installed" : dl.versione.c_str(),
                  dl.porte.size(), ingressi);
    passo(3, "fatto", esito_di(da), t);

    // 5. Dischi
    passo(4, "in corso");
    const std::vector<Disco> dischi = leggi_dischi();
    da = fare.size();
    int misurati = 0;
    for (const auto& d : dischi) misurati += d.misurato_mb_s > 0 ? 1 : 0;
    std::snprintf(t, sizeof t, "%zu drives, %d measured", dischi.size(), misurati);
    passo(4, "fatto", esito_di(da), t);

    // 6. Rete
    passo(5, "in corso");
    const std::vector<SchedaRete> rete = leggi_rete();
    da = fare.size();
    // Indirizzi non scritti: si cercano nella rete, e la pagina li mette nelle impostazioni.
    std::string trovati;
    if (atem.empty() || hyperdeck.empty()) {
        const Apparecchi ap = cerca_apparecchi();
        if (atem.empty() && !ap.atem.empty()) { atem = ap.atem[0]; trovati += "ATEM " + atem; }
        if (hyperdeck.empty() && !ap.hyperdeck.empty()) {
            hyperdeck = ap.hyperdeck[0];
            trovati += (trovati.empty() ? "" : ", ") + std::string("HyperDeck ") + hyperdeck;
        }
    }
    const bool atem_rete = stessa_rete(rete, atem), atem_ok = atem_risponde(atem);
    const bool hd_rete = stessa_rete(rete, hyperdeck), hd_ok = tcp_risponde(hyperdeck, 9993);
    if (atem.empty())
        fare.push_back({"info", "No ATEM on the network",
                        {"Searched all networks of this machine: no response",
                         "Without the ATEM the Resolve project has no cuts and no source names",
                         "Markers from the recorder's button still work", "If it is on another network, enter its address in the page Settings"}});
    else if (!atem_ok)
        fare.push_back({"info", "ATEM " + atem + " is not responding",
                        {atem_rete ? "It is on this machine's network: check that it is powered on and connected"
                                   : "No network adapter of this machine is on its network: connect the machine to the control room network",
                         "The timecode still arrives embedded in the SDI: recording works, without cuts or names"}});
    if (!hyperdeck.empty() && !hd_ok)
        fare.push_back({"info", "HyperDeck " + hyperdeck + " is not responding",
                        {hd_rete ? "Check that it is powered on and that its Ethernet protocol is enabled"
                                 : "No network adapter of this machine is on its network",
                         "Recording works, without controlling the HyperDeck"}});
    std::string ip_qui;
    for (const auto& s : rete)
        for (const auto& [a, pref] : s.ip) ip_qui += (ip_qui.empty() ? "" : ", ") + ip_testo(a);
    passo(5, "fatto", esito_di(da),
          (atem.empty() ? std::string("no ATEM on the network") : "ATEM " + atem + (atem_ok ? " responding" : " not responding")) +
              (trovati.empty() ? "" : ", found automatically") + (ip_qui.empty() ? ", no network" : ", this machine " + ip_qui));

    // 7. Impostazioni di Windows
    passo(6, "in corso");
    std::vector<Voce> voci;
    controlli_sistema(&voci);
    da = fare.size();
    for (const auto& x : voci)
        if (x.esito != Semaforo::verde) {
            std::vector<std::string> come;
            if (x.testo.find("power plan") == 0)
                come = {"Control Panel › Power Options", "Choose \"High performance\"",
                        "With the Balanced plan the CPU slows down between frames"};
            else if (x.testo.find("Windows Update") == 0)
                come = {"Settings › Windows Update › Pause updates",
                        "So it does not download or restart during the live event"};
            else
                come = {"Restart the computer before the live event"};
            // Il rimedio scritto in fondo alla riga della console va nei passi, non nel titolo.
            fare.push_back({x.esito == Semaforo::rosso ? "rosso" : "giallo", x.testo.substr(0, x.testo.find(" — ")), come});
        }
    passo(6, "fatto", esito_di(da), std::to_string(voci.size()) + " checks");

    // 8. Pacchetto
    passo(7, "in corso");
    const std::wstring prog = cartella_programma();
    struct Pezzo { const char* nome; std::wstring percorso; const char* serve; };
    const std::vector<Pezzo> pezzi = {
        {"Python", prog + L"\\python\\python.exe", "ATEM cut log and Resolve project"},
        {"registro_atem.py", prog + L"\\script\\registro_atem.py", "ATEM cut log"},
        {"progetto_da_registratore.py", prog + L"\\script\\progetto_da_registratore.py", "Resolve project at end of recording"},
        {"crea_progetto_resolve.py", prog + L"\\script\\crea_progetto_resolve.py", "Resolve project at end of recording"},
        {"genera_fcpxml.py", prog + L"\\script\\genera_fcpxml.py", "project for other editing applications"},
        {"genera_premiere.py", prog + L"\\script\\genera_premiere.py", "XML and EDL for Premiere Pro"},
        {"clip_hyperdeck.py", prog + L"\\script\\clip_hyperdeck.py", "HyperDeck clips in the project"},
        {"rapporto_prova.py", prog + L"\\script\\rapporto_prova.py", "test report for the beta testers"},
        {"mappa_porte.py", prog + L"\\script\\mappa_porte.py", "input map to the ATEM aux outputs"},
        {"ritardi_porte.py", prog + L"\\script\\ritardi_porte.py", "input delay measurement"},
        {"elenco_registrazioni.py", prog + L"\\script\\elenco_registrazioni.py", "list of past recordings in Review"},
        {"recording page", prog + L"\\interfaccia\\registratore.html", "the page itself"},
        {"system check page", prog + L"\\interfaccia\\primo-avvio.html", "this page"}};
    da = fare.size();
    std::string pacchetto = "[";
    int mancano = 0;
    for (const auto& p : pezzi) {
        const bool c = esiste(p.percorso);
        mancano += c ? 0 : 1;
        pacchetto += std::string(pacchetto.size() > 1 ? "," : "") + "{\"nome\":" + json_testo(p.nome) +
                     ",\"presente\":" + json_si(c) + ",\"serve\":" + json_testo(p.serve) + "}";
        if (!c)
            fare.push_back({"giallo", p.nome + std::string(" is missing from the package"),
                            {std::string("Used for: ") + p.serve,
                             "Copy the whole recorder folder again, as it is, without removing anything"}});
    }
    const bool dnxhd = avcodec_find_encoder(AV_CODEC_ID_DNXHD) != nullptr;
    pacchetto += std::string(",{\"nome\":\"DNxHD encoder\",\"presente\":") + json_si(dnxhd) +
                 ",\"serve\":\"recording\"}]";
    if (!dnxhd)
        fare.push_back({"rosso", "The DNxHD encoder is missing", {"The ffmpeg libraries next to the recorder are a different version",
                                                                  "Copy the whole recorder folder again"}});
    const bool resolve = esiste(largo(std::string(std::getenv("PROGRAMDATA") ? std::getenv("PROGRAMDATA") : "C:\\ProgramData") +
                                      "\\Blackmagic Design\\DaVinci Resolve\\Support\\Developer\\Scripting\\Modules\\DaVinciResolveScript.py"));
    if (!resolve)
        fare.push_back({"info", "DaVinci Resolve is not installed on this machine",
                        {"The Resolve project at end of recording is not built here",
                         "The recording folder contains everything: the project can be built later on another machine"}});
    passo(7, "fatto", esito_di(da), mancano ? std::to_string(mancano) + " items missing" : "complete");

    // 9. Prova di codifica: tutte le porte d'ingresso insieme, per ogni livello di DNxHD.
    passo(8, "in corso", "", "one encode per input");
    da = fare.size();
    const int n = std::max(1, ingressi);
    // Se il processore regge più tracce delle porte, la prova si rifà con quel numero di codifiche
    // insieme, finché il numero non si ferma: prima scriveva «24+» (17 novembre 2026, sezione 9.71).
    constexpr int TETTO = 64;
    struct Codec { int mbit; double mb_s; double fps; int tracce; int provate; bool almeno; };
    std::vector<Codec> codec;
    auto prova_codec = [&](int mbit, const std::string& modo, int quante) {
        const Formato* f = formato(modo);
        ParametriCodifica pc;
        pc.mbit = mbit;
        pc.larghezza = f->larghezza;
        pc.altezza = f->altezza;
        pc.canali_audio = 2;
        pc.wav = true;
        int m = quante, tracce = 0;
        double fps = 0;
        for (int giro = 0; giro < 4; ++giro) {
            passo(8, "in corso", "", nome_codec(mbit) + ", " + std::to_string(m) + " simultaneous encodes");
            fps = codifica_al_massimo(m, pc, 5, 2);
            tracce = static_cast<int>(fps / (pc.cad.fps() * 1.3));
            if (tracce <= m || m >= TETTO) break;
            m = std::min(tracce, TETTO);
        }
        codec.push_back({mbit, byte_al_secondo(pc) / 1e6, fps, tracce, m, tracce > m});
    };
    if (dnxhd)
        for (int mbit : codec_del_formato("1080p25")) prova_codec(mbit, "1080p25", n);
    if (!codec.empty() && ingressi) {
        if (codec[0].tracce >= ingressi) {
            // va bene così
        } else if (codec.back().tracce >= 1) {
            std::string scelta;
            for (const auto& c : codec)
                if (c.tracce >= ingressi) { scelta = "DNxHD " + std::to_string(c.mbit); break; }
            std::snprintf(t, sizeof t, "The CPU handles %d tracks in DNxHD 185 with a 30%% margin; there are %d inputs",
                          codec[0].tracce, ingressi);
            fare.push_back({"arancio", t,
                            {scelta.empty() ? "Even the lightest DNxHD cannot handle all of them: record fewer tracks"
                                            : "With " + scelta + " it handles all of them: choose it in Settings",
                             "Or record fewer tracks in DNxHD 185",
                             "Keep other heavy applications closed while recording"}});
        } else {
            fare.push_back({"rosso", "The CPU cannot handle even one track in real time",
                            {"A more powerful machine is needed"}});
        }
    }
    // UHD (sezione 9.74): la prova in DNxHR solo se qualche porta riceve il 2160p25, perché costa quattro
    // volte l'HD. I consigli qui sopra restano sull'HD; in UHD il numero di tracce lo dice la pagina.
    int ingressi_uhd = 0;
    for (const auto& p : dl.porte) ingressi_uhd += p.ingresso && p.u25;
    if (dnxhd && ingressi_uhd)
        for (int mbit : codec_del_formato("2160p25")) prova_codec(mbit, "2160p25", ingressi_uhd);
    std::string prova = "[";
    for (size_t k = 0; k < codec.size(); ++k)
        prova += std::string(k ? "," : "") + "{\"mbit\":" + std::to_string(codec[k].mbit) + ",\"mb_s\":" + json_num(codec[k].mb_s, 1) +
                 ",\"fps\":" + json_num(codec[k].fps) + ",\"tracce\":" + std::to_string(codec[k].tracce) +
                 ",\"provate\":" + std::to_string(codec[k].provate) + ",\"almeno\":" + json_si(codec[k].almeno) + "}";
    prova += "]";
    passo(8, "fatto", esito_di(da),
          codec.empty() ? "not run" : std::to_string(codec[0].tracce) + (codec[0].almeno ? " or more" : "") + " tracks in DNxHD 185");

    // 10. Il disco si misura quando lo si sceglie: è lì che si scrive, ed è la scelta di chi registra.
    if (!misurati)
        fare.push_back({"info", "No drive measured yet",
                        {"A drive is measured from the destination selector, when you choose it",
                         "The measurement writes to a test folder for five seconds, then deletes it"}});
    passo(9, "fatto", misurati ? "verde" : "", misurati ? std::to_string(misurati) + " drives measured" : "measured when the destination is chosen");

    // Il verdetto
    std::string verdetto = "pronto";
    for (const auto& f : fare) {
        if (f.livello == "rosso") verdetto = "no";
        else if (f.livello == "arancio" && verdetto != "no") verdetto = "garanzie";
        else if (f.livello == "giallo" && verdetto == "pronto") verdetto = "indicazioni";
    }

    std::string j = "{\n\"programma\": \"mtsrec\",\n\"in_corso\": false,\n";
    char data[32];
    const std::time_t ora = std::time(nullptr);
    std::tm locale{};
    localtime_s(&locale, &ora);
    std::strftime(data, sizeof data, "%Y-%m-%d %H:%M", &locale);
    j += "\"data\": " + json_testo(data) + ",\n";
    j += "\"verdetto\": " + json_testo(verdetto) + ",\n";
    j += "\"impronta\": " + json_testo(impronta_macchina()) + ",\n";
    {
        std::lock_guard<std::mutex> l(m_);
        j += "\"passi\": [";
        for (int k = 0; k < 10; ++k)
            j += std::string(k ? "," : "") + "{\"nome\":" + json_testo(NOMI_PASSI[k]) + ",\"stato\":\"fatto\",\"esito\":" +
                 json_testo(passi_[k][1]) + ",\"nota\":" + json_testo(passi_[k][2]) + "}";
        j += "],\n";
    }
    j += "\"da_fare\": [";
    for (size_t k = 0; k < fare.size(); ++k) {
        j += std::string(k ? ",\n  " : "\n  ") + "{\"livello\":" + json_testo(fare[k].livello) + ",\"peso\":" +
             json_testo(peso(fare[k].livello)) + ",\"titolo\":" + json_testo(fare[k].titolo) + ",\"come\":[";
        for (size_t c = 0; c < fare[k].come.size(); ++c) j += std::string(c ? "," : "") + json_testo(fare[k].come[c]);
        j += "]}";
    }
    wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD npc = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(pc, &npc);
    j += "],\n\"sistema\": {\"computer\":" + json_testo(utf8(pc)) + ",\"windows\":" + json_testo(sis.windows) + ",\"processore\":" + json_testo(sis.processore) +
         ",\"nuclei\":" + std::to_string(sis.nuclei) + ",\"fili\":" + std::to_string(sis.fili) +
         ",\"memoria_gb\":" + json_num(sis.memoria_gb, 1) + ",\"schede_video\":[";
    for (size_t k = 0; k < sis.video.size(); ++k)
        j += std::string(k ? "," : "") + "{\"nome\":" + json_testo(sis.video[k].nome) + ",\"memoria_gb\":" + json_num(sis.video[k].memoria_gb, 1) + "}";
    j += "]},\n\"pcie\": [";
    for (size_t k = 0; k < pci.size(); ++k) {
        const auto& p = pci[k];
        j += std::string(k ? ",\n  " : "\n  ") + "{\"nome\":" + json_testo(p.nome) + ",\"produttore\":" + json_testo(p.produttore) +
             ",\"classe\":" + json_testo(p.classe) + ",\"acquisizione\":" + json_si(p.acquisizione) +
             ",\"larghezza\":" + std::to_string(p.larg) + ",\"larghezza_max\":" + std::to_string(p.larg_max) +
             ",\"gts\":" + json_num(gts(p.vel), 1) + ",\"gts_max\":" + json_num(gts(p.vel_max), 1) +
             ",\"problema\":" + std::to_string(p.problema) + "}";
    }
    j += "],\n\"decklink\": {\"driver\":" + std::string(json_si(dl.driver)) + ",\"versione\":" + json_testo(dl.versione) + ",\"porte\":[";
    for (size_t k = 0; k < dl.porte.size(); ++k) {
        const auto& p = dl.porte[k];
        j += std::string(k ? ",\n  " : "\n  ") + "{\"modello\":" + json_testo(p.modello) + ",\"nome\":" + json_testo(p.nome) +
             ",\"ingresso\":" + json_si(p.ingresso) + ",\"uscita\":" + json_si(p.uscita) +
             ",\"connessioni\":" + json_testo(p.connessioni) + ",\"interfaccia\":" + json_testo(p.interfaccia) +
             ",\"canali_audio\":" + std::to_string(p.canali_audio) + ",\"rileva_formato\":" + json_si(p.rileva_formato) +
             ",\"p25\":" + json_si(p.p25) + ",\"i50\":" + json_si(p.i50) + ",\"u25\":" + json_si(p.u25) + "}";
    }
    j += "]},\n\"ingressi\": " + std::to_string(ingressi) + ",\n";
    j += "\"dischi\": " + json_dischi(dischi) + ",\n";
    j += "\"rete\": {\"schede\":[";
    for (size_t k = 0; k < rete.size(); ++k) {
        j += std::string(k ? "," : "") + "{\"nome\":" + json_testo(rete[k].nome) + ",\"ip\":[";
        for (size_t c = 0; c < rete[k].ip.size(); ++c)
            j += std::string(c ? "," : "") + json_testo(ip_testo(rete[k].ip[c].first) + "/" + std::to_string(rete[k].ip[c].second));
        j += "]}";
    }
    j += "],\"atem\":{\"ip\":" + json_testo(atem) + ",\"stessa_rete\":" + json_si(atem_rete) + ",\"risponde\":" + json_si(atem_ok) + "}";
    j += ",\"hyperdeck\":{\"ip\":" + json_testo(hyperdeck) + ",\"stessa_rete\":" + json_si(hd_rete) + ",\"risponde\":" + json_si(hd_ok) + "}";
    j += ",\"porta_pagina\":" + std::to_string(porta) + "},\n";
    j += "\"windows\": [";
    for (size_t k = 0; k < voci.size(); ++k)
        j += std::string(k ? "," : "") + "{\"esito\":" + json_testo(voci[k].esito == Semaforo::verde ? "verde" : voci[k].esito == Semaforo::giallo ? "giallo" : "rosso") +
             ",\"testo\":" + json_testo(voci[k].testo) + "}";
    j += "],\n\"pacchetto\": " + pacchetto + ",\n\"resolve\": " + json_si(resolve) + ",\n";
    j += "\"prova\": {\"porte\":" + std::to_string(n) + ",\"codec\":" + prova + "}\n}\n";

    scrivi_file(cartella_utente() + L"\\macchina.json", j);
    {
        std::lock_guard<std::mutex> l(m_);
        risultato_ = j;
    }
    in_corso_ = false;
    if (SUCCEEDED(co)) CoUninitialize();
}

// ------------------------------------------------------------------ prerequisiti (sezione 9.77)
// Gli indirizzi verificati il 17 novembre 2026: la pagina di supporto Blackmagic delle schede di acquisizione
// (Desktop Video 16.5 l'ultima), DaVinci Resolve 21, il runtime WebView2 di Microsoft.
struct Prerequisito { const char* id; const char* nome; const char* perche; bool essenziale; const char* link; };
static const Prerequisito PREREQUISITI[] = {
    {"desktop_video", "Blackmagic Desktop Video 16 or later",
     "The driver of the DeckLink cards: without it the recorder sees no inputs and cannot record.", true,
     "https://www.blackmagicdesign.com/support/family/capture-and-playback"},
    {"resolve", "DaVinci Resolve Studio",
     "Builds the Resolve project with the multicam clip and the cut timeline at the end of each recording. "
     "Without it you still get FCPXML, Premiere XML and EDL.", false,
     "https://www.blackmagicdesign.com/products/davinciresolve"},
    {"webview2", "Microsoft Edge WebView2 Runtime",
     "Gives the recorder its own window. Without it the recorder opens in the web browser, which works the same.", false,
     "https://developer.microsoft.com/microsoft-edge/webview2/"},
};

std::string link_prerequisito(const std::string& id) {
    for (const auto& p : PREREQUISITI)
        if (id == p.id) return p.link;
    return {};
}

static bool webview2_presente() {
    const wchar_t* chiave = L"Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    for (const auto& [radice, base] : {std::pair{HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\"}, std::pair{HKEY_CURRENT_USER, L"Software\\"}}) {
        wchar_t v[64] = {};
        DWORD n = sizeof v;
        const std::wstring k = std::wstring(base) + chiave;
        if (RegGetValueW(radice, k.c_str(), L"pv", RRF_RT_REG_SZ, nullptr, v, &n) == ERROR_SUCCESS && v[0] &&
            std::wstring(v) != L"0.0.0.0")
            return true;
    }
    return false;
}

std::string prerequisiti_json() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const DeckLink dl = leggi_decklink();
    if (SUCCEEDED(co)) CoUninitialize();
    const bool desktop_video = dl.driver && std::atoi(dl.versione.c_str()) >= 16;
    const char* pd = std::getenv("PROGRAMDATA");
    const bool resolve = esiste(largo(std::string(pd ? pd : "C:\\ProgramData") +
                                      "\\Blackmagic Design\\DaVinci Resolve\\Support\\Developer\\Scripting\\Modules\\DaVinciResolveScript.py"));
    std::string j = "{\"desktop_video\":" + json_testo(dl.versione) + ",\"mancanti\":[";
    bool primo = true;
    for (const auto& p : PREREQUISITI) {
        const std::string id = p.id;
        const bool manca = id == "desktop_video" ? !desktop_video : id == "resolve" ? !resolve : !webview2_presente();
        if (!manca) continue;
        std::string perche = p.perche;
        if (id == "desktop_video" && dl.driver) perche = "Installed version " + dl.versione + " is too old. " + perche;
        j += std::string(primo ? "" : ",") + "{\"id\":" + json_testo(id) + ",\"nome\":" + json_testo(p.nome) +
             ",\"perche\":" + json_testo(perche) + ",\"essenziale\":" + json_si(p.essenziale) + ",\"link\":" + json_testo(p.link) + "}";
        primo = false;
    }
    return j + "]}";
}
