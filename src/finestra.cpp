#include <windows.h>
#include <shellapi.h>
#include <wrl.h>

#include "finestra.h"

#include <cstdio>
#include <filesystem>

#include "WebView2.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const UINT CHIUDI_SENZA_CHIEDERE = WM_APP + 1;

// Quello che serve alla procedura della finestra, attaccato alla finestra stessa.
struct Stato {
    ComPtr<ICoreWebView2Controller> ctl;
    ComPtr<ICoreWebView2> vista;
    std::function<std::string()> perche_no;
    std::atomic<bool>* chiusa = nullptr;
};

std::wstring largo(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring r(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, r.data(), n);
    return r;
}

LRESULT CALLBACK procedura(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Stato* s = reinterpret_cast<Stato*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (s && s->ctl) {
            RECT r;
            GetClientRect(h, &r);
            s->ctl->put_Bounds(r);
        }
        return 0;
    case WM_CLOSE:
        // Chiudere la finestra chiude il registratore: mai mentre registra.
        if (s && s->perche_no) {
            const std::string no = s->perche_no();
            if (!no.empty()) {
                MessageBoxW(h, largo(no).c_str(), L"Multicam Timecode Sync Recorder", MB_OK | MB_ICONWARNING);
                return 0;
            }
        }
        if (s && s->chiusa) *s->chiusa = true;
        DestroyWindow(h);
        return 0;
    case CHIUDI_SENZA_CHIEDERE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (s && s->ctl) s->ctl->Close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}   // namespace

Finestra::~Finestra() {
    chiudi();
    if (filo_.joinable()) filo_.join();
}

void Finestra::apri(const std::string& url, const std::wstring& cartella_dati, std::function<std::string()> perche_no) {
    perche_no_ = std::move(perche_no);
    filo_ = std::thread(&Finestra::lavora, this, url, cartella_dati);
}

void Finestra::chiudi() {
    if (HWND h = static_cast<HWND>(hwnd_.load())) PostMessageW(h, CHIUDI_SENZA_CHIEDERE, 0, 0);
}

void Finestra::lavora(std::string url, std::wstring cartella_dati) {
    // WebView2 vuole un filo STA con la sua coda di messaggi.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::printf("⚠ window: COM initialisation failed, opening the web browser\n");
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    const HINSTANCE ist = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = procedura;
    wc.hInstance = ist;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 28));     // il fondo della pagina, finché non arriva
    wc.hIcon = LoadIconW(ist, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"mtsrecFinestra";
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Multicam Timecode Sync Recorder", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1600, 950, nullptr, nullptr, ist, nullptr);
    Stato stato;
    stato.perche_no = perche_no_;
    stato.chiusa = &chiusa_;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&stato));
    hwnd_ = h;
    ShowWindow(h, SW_SHOWMAXIMIZED);
    UpdateWindow(h);

    std::error_code ec;
    std::filesystem::create_directories(cartella_dati, ec);
    const std::wstring indirizzo = largo(url);
    auto ripiego = [h, url](HRESULT hr) {
        std::printf("⚠ window: WebView2 failed to start (0x%08lX), opening the web browser\n", static_cast<unsigned long>(hr));
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        PostMessageW(h, CHIUDI_SENZA_CHIEDERE, 0, 0);       // la finestra vuota non serve: il registratore resta
    };
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, cartella_dati.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [h, &stato, indirizzo, ripiego](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(r) || !env) { ripiego(r); return S_OK; }
                return env->CreateCoreWebView2Controller(h,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [h, &stato, indirizzo, ripiego](HRESULT r2, ICoreWebView2Controller* ctl) -> HRESULT {
                            if (FAILED(r2) || !ctl) { ripiego(r2); return S_OK; }
                            stato.ctl = ctl;
                            ctl->get_CoreWebView2(&stato.vista);
                            ComPtr<ICoreWebView2Settings> imp;
                            if (SUCCEEDED(stato.vista->get_Settings(&imp))) {
                                imp->put_IsStatusBarEnabled(FALSE);
                                imp->put_IsZoomControlEnabled(TRUE);
                            }
                            // Il titolo della pagina va sulla finestra.
                            stato.vista->add_DocumentTitleChanged(
                                Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                                    [h](ICoreWebView2* v, IUnknown*) -> HRESULT {
                                        LPWSTR t = nullptr;
                                        if (SUCCEEDED(v->get_DocumentTitle(&t)) && t) {
                                            SetWindowTextW(h, *t ? t : L"Multicam Timecode Sync Recorder");
                                            CoTaskMemFree(t);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);
                            // Un collegamento che vuole una finestra nuova va nel browser.
                            stato.vista->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                                        LPWSTR u = nullptr;
                                        if (SUCCEEDED(a->get_Uri(&u)) && u) {
                                            ShellExecuteW(nullptr, L"open", u, nullptr, nullptr, SW_SHOWNORMAL);
                                            CoTaskMemFree(u);
                                        }
                                        a->put_Handled(TRUE);
                                        return S_OK;
                                    }).Get(), nullptr);
                            RECT rc;
                            GetClientRect(h, &rc);
                            ctl->put_Bounds(rc);
                            stato.vista->Navigate(indirizzo.c_str());
                            return S_OK;
                        }).Get());
            }).Get());
    if (FAILED(hr)) ripiego(hr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    hwnd_ = nullptr;
    stato.vista.Reset();
    stato.ctl.Reset();
    CoUninitialize();
}
