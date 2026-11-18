// Il carico della macchina per il pannello Sistema della pagina: processore, scheda video e memoria.
// Un filo lo legge ogni mezzo secondo, la pagina prende l'ultimo valore: chiederlo non pesa.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class Carico {
public:
    Carico();
    ~Carico();
    Carico(const Carico&) = delete;
    Carico& operator=(const Carico&) = delete;
    // {"cpu":23.5,"gpu":4.0,"ram":41.2,"ram_usata_gb":13.1,"ram_gb":31.9}; gpu -1 se Windows non la dà
    std::string json() const;
    double cpu() const;
    // Il carico durante una registrazione, per il rapporto di prova: si azzera al REC e si legge allo STOP.
    // {"cpu_media":15.3,"cpu_max":18.3,"gpu_max":4.0,"ram_max":41.2,"campioni":3600}
    void azzera_statistica();
    std::string statistica_json() const;

private:
    void lavora();

    mutable std::mutex m_;
    double cpu_ = 0, gpu_ = -1, ram_ = 0, ram_usata_ = 0, ram_tot_ = 0;
    double somma_cpu_ = 0, max_cpu_ = 0, max_gpu_ = -1, max_ram_ = 0;
    long long campioni_ = 0;
    std::atomic<bool> basta_{false};
    std::thread filo_;
};
