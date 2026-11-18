#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "marker.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "traccia.h"

namespace fs = std::filesystem;

static std::string minuscolo(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string colore_resolve(const std::string& nome) {
    // I sedici colori dei marker di Resolve, coi nomi italiani accanto a quelli inglesi.
    static const char* colori[][3] = {
        {"blu", "blue", "Blue"},          {"ciano", "cyan", "Cyan"},         {"verde", "green", "Green"},
        {"giallo", "yellow", "Yellow"},   {"rosso", "red", "Red"},           {"rosa", "pink", "Pink"},
        {"viola", "purple", "Purple"},    {"fucsia", "fuchsia", "Fuchsia"},  {"rosato", "rose", "Rose"},
        {"lavanda", "lavender", "Lavender"}, {"azzurro", "sky", "Sky"},     {"menta", "mint", "Mint"},
        {"limone", "lemon", "Lemon"},     {"sabbia", "sand", "Sand"},        {"marrone", "cocoa", "Cocoa"},
        {"crema", "cream", "Cream"}};
    const std::string n = minuscolo(nome);
    for (const auto& c : colori)
        if (n == c[0] || n == c[1]) return c[2];
    return {};
}

static std::string pulito(std::string s) {
    for (char& c : s)
        if (c == ';' || c == '\n' || c == '\r' || c == '\t') c = ',';
    return s;
}

static std::string json(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += c;
    }
    return r;
}

// Decodifica di una query string: %XX e + per lo spazio.
static std::string decodifica(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') r += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) { r += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
        else r += s[i];
    }
    return r;
}

std::string parametro_query(const std::string& query, const std::string& nome) {
    size_t p = 0;
    while (p < query.size()) {
        size_t f = query.find('&', p);
        if (f == std::string::npos) f = query.size();
        const std::string voce = query.substr(p, f - p);
        const size_t u = voce.find('=');
        if (voce.substr(0, u) == nome) return u == std::string::npos ? "" : decodifica(voce.substr(u + 1));
        p = f + 1;
    }
    return {};
}

Marcatore::Marcatore(const std::string& percorso_csv, std::function<Cadenza()> cadenza, std::function<int()> adesso)
    : cadenza_(std::move(cadenza)) {
    registra_in(percorso_csv, std::move(adesso));
}

Marcatore::~Marcatore() { ferma(); }

void Marcatore::registra_in(const std::string& percorso_csv, std::function<int()> adesso) {
    std::lock_guard<std::mutex> l(m_);
    percorso_ = percorso_csv;
    adesso_ = std::move(adesso);
    quanti_ = 0;
    elenco_.clear();
    if (percorso_.empty()) return;                    // anteprima: solo le pagine, nessun file
    FILE* f = _wfopen(fs::u8path(percorso_).c_str(), L"w");
    if (f) {
        std::fputs("timecode;colour;duration;source;text\n", f);
        std::fclose(f);
    }
}

void Marcatore::smetti() {
    std::lock_guard<std::mutex> l(m_);
    percorso_.clear();
}

bool Marcatore::attivo() const {
    std::lock_guard<std::mutex> l(m_);
    return !percorso_.empty();
}

std::string Marcatore::elenco_json() const {
    std::lock_guard<std::mutex> l(m_);
    std::string r;
    for (const Marker& k : elenco_)
        r += std::string(r.empty() ? "" : ",") + "{\"tc\":" + std::to_string(k.tc) + ",\"timecode\":\"" + tc_testo(k.tc, cadenza_()) +
             "\",\"colore\":\"" + k.colore + "\",\"fonte\":\"" + k.fonte + "\",\"testo\":\"" + json(k.testo) + "\"}";
    return "[" + r + "]";
}

Marker Marcatore::aggiungi(std::string testo, const std::string& colore, int durata, const std::string& fonte) {
    Marker k;
    const std::string c = colore_resolve(colore);
    if (!c.empty()) k.colore = c;
    k.durata = durata > 0 ? durata : 1;
    k.fonte = fonte;
    k.testo = pulito(std::move(testo));
    std::lock_guard<std::mutex> l(m_);
    if (percorso_.empty()) return k;                  // non si registra: il marker non va da nessuna parte
    k.tc = adesso_();
    elenco_.push_back(k);
    // Una riga per marker, chiusa subito: se il programma cade, i marker già dati restano.
    if (FILE* f = _wfopen(fs::u8path(percorso_).c_str(), L"a")) {
        // timecode fra virgolette: col drop-frame contiene il punto e virgola del separatore (9.80)
        std::fprintf(f, "\"%s\";%s;%d;%s;%s\n", tc_testo(k.tc, cadenza_()).c_str(), k.colore.c_str(), k.durata,
                     k.fonte.c_str(), k.testo.c_str());
        std::fclose(f);
    }
    ++quanti_;
    std::printf("  marker %d at %s, %s%s%s\n", quanti_.load(), tc_testo(k.tc, cadenza_()).c_str(), k.colore.c_str(),
                k.testo.empty() ? "" : " \"", k.testo.empty() ? "" : (k.testo + "\"").c_str());
    return k;
}

Marker Marcatore::da_tastiera(const std::string& riga) {
    std::string testo = riga, colore;
    const size_t s = riga.find(' ');
    const std::string prima = riga.substr(0, s);
    if (!colore_resolve(prima).empty()) {
        colore = prima;
        testo = s == std::string::npos ? "" : riga.substr(s + 1);
    }
    return aggiungi(testo, colore, 1, "keyboard");
}

bool Marcatore::ascolta(int porta, std::string& errore) {
    static const bool pronto = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }();
    if (!pronto) { errore = "marker: Winsock failed to start"; return false; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL esclusivo = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&esclusivo), sizeof esclusivo);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(porta));
    a.sin_addr.s_addr = htonl(INADDR_ANY);              // Companion può stare su un'altra macchina
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 8) != 0) {
        errore = "marker: port " + std::to_string(porta) + " is already in use";
        closesocket(s);
        return false;
    }
    ascolto_ = static_cast<unsigned long long>(s);
    filo_ = std::thread([this] { servi(); });
    return true;
}

void Marcatore::ferma() {
    basta_ = true;
    if (filo_.joinable()) filo_.join();
    if (ascolto_ != ~0ull) { closesocket(static_cast<SOCKET>(ascolto_)); ascolto_ = ~0ull; }
}

void Marcatore::servi() {
    const SOCKET s = static_cast<SOCKET>(ascolto_);
    while (!basta_) {
        fd_set f;
        FD_ZERO(&f);
        FD_SET(s, &f);
        timeval t{0, 200000};                         // ogni 200 ms guarda se deve fermarsi
        if (select(0, &f, nullptr, nullptr, &t) <= 0) continue;
        SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        DWORD attesa = 1000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&attesa), sizeof attesa);
        std::string r;
        char b[2048];
        while (r.size() < 8192 && r.find("\r\n\r\n") == std::string::npos) {
            const int n = recv(c, b, sizeof b, 0);
            if (n <= 0) break;
            r.append(b, n);
        }
        int codice = 200;
        std::string tipo = "application/json; charset=utf-8";
        // Un sito qualunque aperto nel browser non deve poter comandare il registratore (REC, STOP, rapporto):
        // si rifiuta quello che il browser dichiara venire da un altro sito, o che porta un'origine diversa da
        // questa macchina. Companion e gli script non mandano queste intestazioni e passano (sezione 9.73).
        std::string testa = r.substr(0, r.find("\r\n\r\n"));
        for (char& ch : testa) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        bool estraneo = testa.find("\r\nsec-fetch-site: cross-site") != std::string::npos;
        if (const size_t o = testa.find("\r\norigin: "); o != std::string::npos) {
            const std::string origine = testa.substr(o + 10, testa.find("\r\n", o + 10) - (o + 10));
            estraneo = estraneo || !(origine.rfind("http://localhost:", 0) == 0 || origine.rfind("http://127.0.0.1:", 0) == 0);
        }
        const std::string corpo = estraneo ? (codice = 403, std::string("{\"errore\":\"requests from other websites are refused\"}"))
                                           : rispondi(r, codice, tipo);
        const std::string risposta = "HTTP/1.1 " + std::to_string(codice) + (codice == 200 ? " OK" : " Error") +
                                     "\r\nContent-Type: " + tipo + "\r\nCache-Control: no-store\r\n"
                                     "Connection: close\r\nContent-Length: " +
                                     std::to_string(corpo.size()) + "\r\n\r\n" + corpo;
        send(c, risposta.c_str(), static_cast<int>(risposta.size()), 0);
        closesocket(c);
    }
}

std::string Marcatore::rispondi(const std::string& richiesta, int& codice, std::string& tipo) {
    // Prima riga: «GET /marker?testo=… HTTP/1.1». Basta GET: Companion, un browser e curl lo sanno fare.
    const std::string riga = richiesta.substr(0, richiesta.find("\r\n"));
    const size_t a = riga.find(' '), b = riga.rfind(' ');
    if (a == std::string::npos || b <= a) { codice = 400; return "{\"errore\":\"malformed request\"}"; }
    const std::string bersaglio = riga.substr(a + 1, b - a - 1);
    const size_t q = bersaglio.find('?');
    const std::string percorso = bersaglio.substr(0, q);
    const std::string query = q == std::string::npos ? "" : bersaglio.substr(q + 1);
    if (percorso == "/marker") {
        if (!attivo()) { codice = 409; return "{\"errore\":\"not recording: no marker added\"}"; }
        const std::string d = parametro_query(query, "durata");
        // La pagina del registratore si firma, così nel registro si distingue da Companion.
        // «pagina» è il valore che la pagina ha sempre mandato; «page» è l'alias inglese.
        const std::string f = parametro_query(query, "fonte");
        const Marker k = aggiungi(parametro_query(query, "testo"), parametro_query(query, "colore"), d.empty() ? 1 : std::atoi(d.c_str()),
                                  f == "pagina" || f == "page" ? "page" : "network");
        return "{\"marker\":" + std::to_string(quanti_.load()) + ",\"timecode\":\"" + tc_testo(k.tc, cadenza_()) +
               "\",\"colore\":\"" + k.colore + "\",\"testo\":\"" + json(k.testo) + "\"}";
    }
    if (percorso == "/stato") return stato ? stato() : "{}";
    std::string corpo;
    if (pagine && pagine(percorso, query, corpo, tipo)) return corpo;
    codice = 404;
    return "{\"errore\":\"page not found\"}";
}
