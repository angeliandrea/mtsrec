#include <windows.h>

#include "impostazioni.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "formato.h"
#include "json.h"
#include "macchina.h"
#include "marker.h"

static std::wstring file_impostazioni() { return cartella_utente() + L"\\impostazioni.json"; }

void Impostazioni::carica() {
    const std::string j = leggi_file(file_impostazioni());
    if (j.empty()) return;
    da_json(j);
}

void Impostazioni::da_json(const std::string& j) {
    cartella = json_valore(j, "cartella");
    nome = json_valore(j, "nome");
    if (const std::string v = json_valore(j, "modo"); formato(v)) modo = v;
    mbit = codec_per(modo, std::atoi(json_valore(j, "mbit").c_str()));
    if (const int v = std::atoi(json_valore(j, "audio").c_str()); v == 2 || v == 8 || v == 16) audio = v;
    resolve = json_valore(j, "resolve") != "false";
    atem = json_valore(j, "atem");
    hyperdeck = json_valore(j, "hyperdeck");
    if (const int v = std::atoi(json_valore(j, "porta").c_str()); v > 0 && v < 65536) porta = v;
    ritardi = json_valore(j, "ritardi");
    tracce = json_valore(j, "tracce");
    setup = json_valore(j, "setup");
    abbinamenti_hd = json_valore(j, "abbinamenti_hd");
}

int Impostazioni::ritardo(const std::string& p) const {
    const std::string cerca = "|" + p + "=", tutto = "|" + ritardi;
    const size_t k = tutto.find(cerca);
    return k == std::string::npos ? 0 : std::atoi(tutto.c_str() + k + cerca.size());
}

void Impostazioni::metti_ritardo(const std::string& p, int n) {
    std::string r;
    for (size_t a = 0; a < ritardi.size();) {
        const size_t b = std::min(ritardi.find('|', a), ritardi.size());
        const std::string voce = ritardi.substr(a, b - a);
        if (!voce.empty() && voce.rfind(p + "=", 0) != 0) r += (r.empty() ? "" : "|") + voce;
        a = b + 1;
    }
    if (n > 0) r += (r.empty() ? "" : "|") + p + "=" + std::to_string(n);
    ritardi = r;
}

std::string Impostazioni::json() const {
    return "{\n  \"cartella\": " + json_testo(cartella) + ",\n  \"nome\": " + json_testo(nome) +
           ",\n  \"modo\": " + json_testo(modo) + ",\n  \"mbit\": " + std::to_string(mbit) +
           ",\n  \"audio\": " + std::to_string(audio) + ",\n  \"resolve\": " + json_si(resolve) +
           ",\n  \"atem\": " + json_testo(atem) + ",\n  \"hyperdeck\": " + json_testo(hyperdeck) +
           ",\n  \"porta\": " + std::to_string(porta) +
           ",\n  \"ritardi\": " + json_testo(ritardi) +
           ",\n  \"tracce\": " + json_testo(tracce) + ",\n  \"setup\": " + json_testo(setup) +
           ",\n  \"abbinamenti_hd\": " + json_testo(abbinamenti_hd) + "\n}\n";
}

bool Impostazioni::salva() const { return scrivi_file(file_impostazioni(), json()); }

// Un indirizzo IPv4 scritto bene, o vuoto: «niente ATEM» è una scelta valida.
static bool ip_valido(const std::string& s) {
    if (s.empty()) return true;
    int parti = 0, cifre = 0, valore = 0;
    for (char c : s) {
        if (c == '.') {
            if (!cifre) return false;
            ++parti; cifre = 0; valore = 0;
        } else if (c >= '0' && c <= '9') {
            valore = valore * 10 + (c - '0');
            if (++cifre > 3 || valore > 255) return false;
        } else return false;
    }
    return parti == 3 && cifre > 0;
}

void Impostazioni::da_query(const std::string& query, std::string& errore) {
    auto c_e = [&](const char* k) { return query.find(std::string(k) + "=") != std::string::npos; };
    if (c_e("cartella")) cartella = parametro_query(query, "cartella");
    if (c_e("nome")) {
        const std::string v = parametro_query(query, "nome");
        if (v.find_first_of("\\/:*?\"<>|") != std::string::npos) errore += "the name cannot contain \\ / : * ? \" < > |. ";
        else nome = v;
    }
    if (c_e("modo")) {
        const std::string v = parametro_query(query, "modo");
        if (formato(v)) { modo = v; mbit = codec_per(modo, mbit); }   // il codec segue il formato: 185 ↔ DNxHR HQ
        else errore += "unsupported format. ";
    }
    if (c_e("mbit")) {
        const int v = std::atoi(parametro_query(query, "mbit").c_str());
        if (codec_valido(modo, v)) mbit = v;
        else errore += uhd(modo) ? "UHD: DNxHR HQ, SQ or LB. " : "HD: DNxHD 185, 120 or 36. ";
    }
    if (c_e("audio")) {
        const int v = std::atoi(parametro_query(query, "audio").c_str());
        if (v == 2 || v == 8 || v == 16) audio = v; else errore += "audio: 2, 8 or 16 channels, never less than 2. ";
    }
    if (c_e("resolve")) resolve = parametro_query(query, "resolve") != "false";
    if (c_e("atem")) {
        const std::string v = parametro_query(query, "atem");
        if (ip_valido(v)) atem = v; else errore += "invalid ATEM address. ";
    }
    if (c_e("hyperdeck")) {
        const std::string v = parametro_query(query, "hyperdeck");
        if (ip_valido(v)) hyperdeck = v; else errore += "invalid HyperDeck address. ";
    }
    if (c_e("ritardo")) {
        const std::string p = parametro_query(query, "ritardo"), f = parametro_query(query, "fotogrammi");
        const int v = std::atoi(f.c_str());
        if (p.empty() || p.find_first_of("|=") != std::string::npos) errore += "invalid input. ";
        else if (f.empty() || f.find_first_not_of("0123456789") != std::string::npos || v > 25)
            errore += "delay: 0 to 25 frames. ";
        else metti_ritardo(p, v);
    }
    if (c_e("tracce")) {
        const std::string v = parametro_query(query, "tracce");
        bool ok = v.size() < 8000;
        for (unsigned char c : v) if (c < 0x20 || c == '"' || c == '\\') ok = false;
        if (ok) tracce = v;
        else errore += "invalid track list. ";
    }
    if (c_e("abbinamenti_hd")) {
        const std::string v = parametro_query(query, "abbinamenti_hd");
        bool ok = v.size() < 8000;
        for (unsigned char c : v) if (c < 0x20 || c == '"' || c == '\\') ok = false;
        if (ok) abbinamenti_hd = v;
        else errore += "invalid HyperDeck assignments. ";
    }
}

// ---------------------------------------------------------------- i setup con un nome

static std::wstring da_utf8(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring r(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, r.data(), n);
    return r;
}

static std::string a_utf8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string r(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, r.data(), n, nullptr, nullptr);
    return r;
}

static std::wstring cartella_setup() {
    const std::wstring c = cartella_utente() + L"\\setup";
    CreateDirectoryW(c.c_str(), nullptr);
    return c;
}

static std::string nome_setup_errore(const std::string& nome) {
    if (nome.empty() || nome.size() > 60) return "the setup name must be 1 to 60 characters";
    if (nome.find_first_of("\\/:*?\"<>|") != std::string::npos) return "the setup name cannot contain \\ / : * ? \" < > |";
    if (nome.front() == ' ' || nome.back() == ' ' || nome.back() == '.') return "the setup name cannot start or end with a space or end with a dot";
    return {};
}

static std::wstring file_setup(const std::string& nome) { return cartella_setup() + L"\\" + da_utf8(nome) + L".json"; }

std::string setup_elenco_json() {
    std::vector<std::string> nomi;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW((cartella_setup() + L"\\*.json").c_str(), &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = d.cFileName;
            nomi.push_back(a_utf8(n.substr(0, n.size() - 5)));
        } while (FindNextFileW(h, &d));
        FindClose(h);
    }
    std::sort(nomi.begin(), nomi.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    std::string r = "[";
    for (size_t k = 0; k < nomi.size(); ++k) r += (k ? "," : "") + json_testo(nomi[k]);
    return r + "]";
}

std::string setup_salva(const std::string& nome, Impostazioni& imp) {
    if (std::string e = nome_setup_errore(nome); !e.empty()) return e;
    Impostazioni s = imp;
    s.nome.clear();                    // il nome della registrazione è di ogni evento, non del setup
    s.setup = nome;
    if (!scrivi_file(file_setup(nome), s.json())) return "could not write the setup file";
    imp.setup = nome;
    imp.salva();
    return {};
}

std::string setup_carica(const std::string& nome, Impostazioni& imp) {
    if (std::string e = nome_setup_errore(nome); !e.empty()) return e;
    const std::string j = leggi_file(file_setup(nome));
    if (j.empty()) return "setup \"" + nome + "\" not found";
    const std::string nome_reg = imp.nome;
    const int porta = imp.porta;
    imp.da_json(j);
    imp.nome = nome_reg;               // restano quelli di adesso
    imp.porta = porta;
    imp.setup = nome;
    imp.salva();
    return {};
}

std::string setup_cancella(const std::string& nome, Impostazioni& imp) {
    if (std::string e = nome_setup_errore(nome); !e.empty()) return e;
    if (!DeleteFileW(file_setup(nome).c_str())) return "setup \"" + nome + "\" not found";
    if (imp.setup == nome) { imp.setup.clear(); imp.salva(); }
    return {};
}
