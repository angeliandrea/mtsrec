// Il poco di JSON che serve: scrivere testi per la pagina e rileggere file piatti come le impostazioni.
// Niente libreria: i file li scrive il registratore stesso, una chiave per riga.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

// "testo" con le virgolette, pronto da mettere in un JSON.
inline std::string json_testo(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += static_cast<char>(c); }
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += static_cast<char>(c);
    }
    return r + "\"";
}

inline std::string json_num(double v, int decimali = 0) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", decimali, v);
    return b;
}

inline const char* json_si(bool v) { return v ? "true" : "false"; }

// Il valore di una chiave al primo livello di un JSON piatto: il testo senza virgolette, oppure
// il numero o true/false come sono scritti. Vuoto se la chiave non c'è.
inline std::string json_valore(const std::string& j, const std::string& chiave) {
    const std::string cerca = "\"" + chiave + "\"";
    size_t p = j.find(cerca);
    if (p == std::string::npos) return {};
    p = j.find(':', p + cerca.size());
    if (p == std::string::npos) return {};
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\n' || j[p] == '\r' || j[p] == '\t')) ++p;
    if (p >= j.size()) return {};
    std::string r;
    if (j[p] == '"') {
        for (++p; p < j.size() && j[p] != '"'; ++p) {
            if (j[p] == '\\' && p + 1 < j.size()) {
                ++p;
                if (j[p] == 'n') r += '\n';
                else if (j[p] == 't') r += '\t';
                else if (j[p] == 'r') r += '\r';
                else if (j[p] == 'u' && p + 4 < j.size()) {
                    r += static_cast<char>(std::strtol(j.substr(p + 1, 4).c_str(), nullptr, 16));
                    p += 4;
                } else r += j[p];
            } else r += j[p];
        }
        return r;
    }
    while (p < j.size() && j[p] != ',' && j[p] != '}' && j[p] != '\n' && j[p] != '\r') r += j[p++];
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

// Un file intero in una stringa; vuota se non c'è.
inline std::string leggi_file(const std::wstring& percorso) {
    std::string r;
    if (FILE* f = _wfopen(percorso.c_str(), L"rb")) {
        char b[65536];
        for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) r.append(b, n);
        std::fclose(f);
    }
    return r;
}

inline bool scrivi_file(const std::wstring& percorso, const std::string& testo) {
    FILE* f = _wfopen(percorso.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(testo.data(), 1, testo.size(), f) == testo.size();
    return std::fclose(f) == 0 && ok;
}
