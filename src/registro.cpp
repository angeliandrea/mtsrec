#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include "registro.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>
#include <vector>

#include "macchina.h"

namespace fs = std::filesystem;

static void tieni_gli_ultimi(const fs::path& cartella, size_t quanti) {
    std::error_code ec;
    std::vector<fs::path> f;
    for (const auto& e : fs::directory_iterator(cartella, ec))
        if (e.path().extension() == L".log") f.push_back(e.path());
    if (f.size() <= quanti) return;
    // il nome comincia con data e ora: l'ordine alfabetico è quello del tempo
    std::sort(f.begin(), f.end());
    for (size_t k = 0; k + quanti < f.size(); ++k) fs::remove(f[k], ec);
}

void avvia_registro() {
    const fs::path cartella = fs::path(cartella_utente()) / L"logs";
    std::error_code ec;
    fs::create_directories(cartella, ec);
    tieni_gli_ultimi(cartella, 9);   // più quello che si apre adesso: dieci

    std::time_t t = std::time(nullptr);
    std::tm ora{};
    localtime_s(&ora, &t);
    wchar_t nome[96];
    wcsftime(nome, 64, L"mtsrec %Y-%m-%d %H-%M-%S", &ora);
    const std::wstring file = (cartella / (std::wstring(nome) + L" " + std::to_wstring(GetCurrentProcessId()) + L".log")).wstring();
    FILE* registro = _wfopen(file.c_str(), L"ab");
    if (!registro) return;

    HANDLE lettura = nullptr, scrittura = nullptr;
    if (!CreatePipe(&lettura, &scrittura, nullptr, 0)) { std::fclose(registro); return; }
    std::fflush(stdout);
    std::fflush(stderr);
    const int prima = _dup(_fileno(stdout));     // dove andava stdout: la console o la pipe di chi ci ha lanciati
    const HANDLE out_prima = GetStdHandle(STD_OUTPUT_HANDLE), err_prima = GetStdHandle(STD_ERROR_HANDLE);
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(scrittura), _O_WRONLY | _O_BINARY);
    if (fd < 0 || _dup2(fd, _fileno(stdout)) != 0 || _dup2(fd, _fileno(stderr)) != 0) {
        std::fclose(registro);
        CloseHandle(lettura);
        return;
    }
    _close(fd);
    // _dup2 cambia anche le maniglie standard di Windows: si rimettono quelle di prima, così i programmi
    // lanciati da qui (gli script Python) ereditano la console di sempre e non una pipe che non è loro.
    SetStdHandle(STD_OUTPUT_HANDLE, out_prima);
    SetStdHandle(STD_ERROR_HANDLE, err_prima);

    std::thread([lettura, registro, prima] {
        char buf[4096];
        DWORD n = 0;
        bool a_capo = true;
        while (ReadFile(lettura, buf, sizeof buf, &n, nullptr) && n) {
            if (prima >= 0) _write(prima, buf, n);
            for (DWORD k = 0; k < n; ++k) {
                if (a_capo) {
                    SYSTEMTIME s;
                    GetLocalTime(&s);
                    std::fprintf(registro, "%02d:%02d:%02d.%03d  ", s.wHour, s.wMinute, s.wSecond, s.wMilliseconds);
                }
                std::fputc(buf[k], registro);
                a_capo = buf[k] == '\n';
            }
            std::fflush(registro);
        }
    }).detach();

    char v[64];
    std::strftime(v, sizeof v, "%Y-%m-%d %H:%M:%S", &ora);
    std::printf("Session log started %s\n", v);
}

bool riga_da_tastiera(std::string& s) {
    s.clear();
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    char c = 0;
    DWORD n = 0;
    while (ReadFile(h, &c, 1, &n, nullptr) && n == 1) {
        if (c == '\n') return true;
        s += c;
    }
    return !s.empty();
}
