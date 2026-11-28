#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "esterni.h"
#include "macchina.h"

#include <algorithm>
#include <chrono>

static std::wstring largo(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring r(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, r.data(), n);
    return r;
}

// ------------------------------------------------------------------ Python del pacchetto
// Nel pacchetto Python e gli script stanno accanto al registratore, in python\ e script\:
// niente da installare sulla macchina. Se mancano, quelli di questo computer.
std::wstring Python::riga(const std::string& script, const std::string& argomenti) {
    wchar_t exe[MAX_PATH];
    std::wstring cartella(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
    cartella.resize(cartella.find_last_of(L'\\'));
    const std::wstring python = cartella + L"\\python\\python.exe", s = cartella + L"\\script\\" + largo(script);
    const bool nel_pacchetto = GetFileAttributesW(python.c_str()) != INVALID_FILE_ATTRIBUTES &&
                               GetFileAttributesW(s.c_str()) != INVALID_FILE_ATTRIBUTES;
    return (nel_pacchetto ? L"\"" + python + L"\" \"" + s + L"\""
                          : L"py -3.13 \"" + largo(WINISO_SCRIPT_DIR) + L"/" + largo(script) + L"\"") +
           L" " + largo(argomenti);
}

std::string Python::esegui(const std::string& script, const std::string& argomenti, int attesa_ms, std::string& errore) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE leggi = nullptr, scrivi = nullptr;
    if (!CreatePipe(&leggi, &scrivi, &sa, 0)) { errore = script + ": pipe not created"; return {}; }
    SetHandleInformation(leggi, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = scrivi;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    // Gli script non leggono dalla tastiera: ricevono NUL. Con l'ingresso del registratore ereditato, se quello
    // è una pipe su cui il filo della tastiera aspetta, Windows serializza le operazioni sul file e lo script
    // resta appeso alla sua chiusura finché la lettura non finisce (24 novembre 2026, sezione 9.87).
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    std::wstring r = riga(script, argomenti);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, r.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        errore = script + ": Python failed to start, error " + std::to_string(GetLastError());
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        CloseHandle(leggi);
        CloseHandle(scrivi);
        return {};
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    CloseHandle(scrivi);
    CloseHandle(pi.hThread);
    // Si legge mentre gira, se no un'uscita lunga riempie la pipe e lo script si blocca.
    std::string uscita;
    const auto limite = std::chrono::steady_clock::now() + std::chrono::milliseconds(attesa_ms);
    char b[4096];
    for (;;) {
        DWORD n = 0;
        if (PeekNamedPipe(leggi, nullptr, 0, nullptr, &n, nullptr) && n) {
            if (ReadFile(leggi, b, std::min<DWORD>(n, sizeof b), &n, nullptr) && n) uscita.append(b, n);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            while (ReadFile(leggi, b, sizeof b, &n, nullptr) && n) uscita.append(b, n);
            break;
        }
        if (std::chrono::steady_clock::now() > limite) {
            TerminateProcess(pi.hProcess, 1);
            errore = script + ": no response within " + std::to_string(attesa_ms / 1000) + " s";
            break;
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(leggi);
    return uscita;
}

bool Python::lancia(const std::string& script, const std::string& argomenti, std::string& errore, void** processo) {
    STARTUPINFOW si{};
    si.cb = sizeof si;
    std::wstring r = riga(script, argomenti);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, r.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        errore = script + ": Python failed to start, error " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    if (processo) *processo = pi.hProcess;
    else CloseHandle(pi.hProcess);
    return true;
}

// ------------------------------------------------------------------ registro ATEM
RegistroAtem::~RegistroAtem() { ferma(); }

std::wstring file_atem_vivo_largo() { return cartella_utente() + L"\\atem-vivo.json"; }

std::string file_atem_vivo() {
    const std::wstring w = file_atem_vivo_largo();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string r(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, r.data(), n, nullptr, nullptr);
    return r;
}

bool RegistroAtem::finito() const {
    return processo_ && WaitForSingleObject(processo_, 0) == WAIT_OBJECT_0;
}

bool RegistroAtem::avvia(const std::string& ip, const std::string& uscita, std::string& errore,
                         const std::string& vivo, const std::string& tagli, const std::string& modo) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE leggi = nullptr, scrivi = nullptr;
    if (!CreatePipe(&leggi, &scrivi, &sa, 0)) { errore = "ATEM log: pipe not created"; return false; }
    SetHandleInformation(scrivi, HANDLE_FLAG_INHERIT, 0);          // il figlio eredita solo la lettura
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = leggi;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);                // i suoi messaggi nella stessa console
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::string arg = "--ip " + ip;
    if (!uscita.empty()) arg += " --uscita \"" + uscita + "\"";
    if (!vivo.empty()) arg += " --vivo \"" + vivo + "\"";
    if (!tagli.empty()) arg += " --tagli \"" + tagli + "\"";
    if (!modo.empty()) arg += " --modo " + modo;
    std::wstring riga = Python::riga("registro_atem.py", arg);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, riga.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        errore = "ATEM log: Python failed to start, error " + std::to_string(GetLastError());
        CloseHandle(leggi);
        CloseHandle(scrivi);
        return false;
    }
    CloseHandle(leggi);
    CloseHandle(pi.hThread);
    processo_ = pi.hProcess;
    stdin_ = scrivi;
    return true;
}

void RegistroAtem::ferma() {
    if (stdin_) { CloseHandle(stdin_); stdin_ = nullptr; }
    if (processo_) {
        if (WaitForSingleObject(processo_, 3000) != WAIT_OBJECT_0) TerminateProcess(processo_, 1);
        CloseHandle(processo_);
        processo_ = nullptr;
    }
}

// ------------------------------------------------------------------ HyperDeck
std::string HyperDeck::comando(const std::string& ip, const std::string& testo, std::string& errore) {
    static const bool pronto = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }();
    if (!pronto) { errore = "HyperDeck: Winsock failed to start"; return {}; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(9993);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    DWORD attesa = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&attesa), sizeof attesa);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        errore = "HyperDeck " + ip + ": not responding";
        closesocket(s);
        return {};
    }
    // All'aggancio l'HyperDeck manda il suo «500 connection info», che finisce con una riga vuota:
    // si legge tutto e si butta, se no la risposta al comando si mescola con quello.
    char b[4096];
    std::string r;
    auto limite = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (r.find("\r\n\r\n") == std::string::npos && r.find("\n\n") == std::string::npos &&
           std::chrono::steady_clock::now() < limite) {
        const int n = recv(s, b, sizeof b, 0);
        if (n <= 0) break;
        r.append(b, n);
    }
    const std::string c = testo + "\n";
    send(s, c.c_str(), static_cast<int>(c.size()), 0);
    r.clear();
    limite = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (r.find('\n') == std::string::npos && std::chrono::steady_clock::now() < limite) {
        const int n = recv(s, b, sizeof b, 0);
        if (n <= 0) break;
        r.append(b, n);
    }
    closesocket(s);
    r = r.substr(0, r.find_first_of("\r\n"));
    if (r.empty()) errore = "HyperDeck " + ip + ": no response to \"" + testo + "\"";
    return r;
}
