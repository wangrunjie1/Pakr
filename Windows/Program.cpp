#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>
#include <wrl.h>
#include <WebView2.h>

#include <cwchar>
#include <sstream>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

struct AppConfig {
    std::wstring url;
    std::wstring title;
    std::wstring id;
};

bool ReadConfig(HINSTANCE instance, AppConfig& config) {
    const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!resource) return false;
    const auto size = SizeofResource(instance, resource);
    const auto data = static_cast<const wchar_t*>(LockResource(LoadResource(instance, resource)));
    if (!data || size == 0 || size % sizeof(wchar_t) != 0) return false;
    std::wistringstream input(std::wstring(data, size / sizeof(wchar_t)));
    return std::getline(input, config.url) && std::getline(input, config.title) && std::getline(input, config.id)
        && !config.url.empty() && !config.title.empty() && !config.id.empty();
}

bool IsHttps(PCWSTR url) {
    if (!url || _wcsnicmp(url, L"https://", 8) != 0) return false;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    return WinHttpCrackUrl(url, 0, 0, &parts) && parts.nScheme == INTERNET_SCHEME_HTTPS
        && parts.dwHostNameLength != 0 && parts.dwUserNameLength == 0 && parts.dwPasswordLength == 0;
}

class BrowserWindow {
public:
    BrowserWindow(const AppConfig& config) : config_(config) {}

    int Run(HINSTANCE instance, int show) {
        WNDCLASSW windowClass{};
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"PakrBrowserWindow";
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        if (!RegisterClassW(&windowClass)) return 1;

        if (!CreateWindowExW(0, windowClass.lpszClassName, config_.title.c_str(), WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760, nullptr, nullptr, instance, this)) return 1;
        ShowWindow(window_, show);
        StartBrowser();

        MSG message{};
        int result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return result < 0 ? 1 : static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* app = static_cast<BrowserWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        auto* app = reinterpret_cast<BrowserWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (app && message == WM_SIZE) app->Resize();
        if (app && message == WM_DESTROY) {
            app->window_ = nullptr;
            if (app->controller_) app->controller_->Close();
            app->controller_.Reset();
            PostQuitMessage(app->exitCode_);
            return 0;
        }
        if (app && message == WM_SETFOCUS && app->controller_) {
            app->controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void Resize() {
        if (!controller_) return;
        RECT bounds{};
        if (GetClientRect(window_, &bounds)) controller_->put_Bounds(bounds);
    }

    void Fail(PCWSTR message) {
        if (!window_) return;
        exitCode_ = 1;
        MessageBoxW(window_, message, config_.title.c_str(), MB_OK | MB_ICONERROR);
        if (window_) DestroyWindow(window_);
    }

    void StartBrowser() {
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            Fail(L"The Windows app data folder is unavailable.");
            return;
        }
        profile_ = std::wstring(local) + L"\\Pakr\\" + config_.id + L"\\WebView2";
        CoTaskMemFree(local);

        PWSTR version = nullptr;
        const auto runtime = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
        CoTaskMemFree(version);
        if (FAILED(runtime)) {
            Fail(L"Install Microsoft Edge WebView2 Runtime, then reopen this app.");
            return;
        }

        const auto result = CreateCoreWebView2EnvironmentWithOptions(nullptr, profile_.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT status, ICoreWebView2Environment* environment) -> HRESULT {
                    if (!window_) return S_OK;
                    if (FAILED(status) || !environment) {
                        Fail(L"The embedded browser could not start.");
                        return S_OK;
                    }
                    const auto requested = environment->CreateCoreWebView2Controller(window_,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                if (!window_) return S_OK;
                                if (FAILED(result) || !controller || FAILED(OpenPage(controller))) {
                                    Fail(L"The embedded browser could not open this page.");
                                }
                                return S_OK;
                            }).Get());
                    if (FAILED(requested)) Fail(L"The browser window could not be created.");
                    return S_OK;
                }).Get());
        if (FAILED(result)) Fail(L"The embedded browser could not start.");
    }

    HRESULT OpenPage(ICoreWebView2Controller* controller) {
        controller_ = controller;
        ComPtr<ICoreWebView2> browser;
        auto result = controller_->get_CoreWebView2(&browser);
        if (FAILED(result)) return result;
        ComPtr<ICoreWebView2Settings> settings;
        result = browser->get_Settings(&settings);
        if (FAILED(result)) return result;
        if (FAILED(result = settings->put_AreHostObjectsAllowed(FALSE))) return result;
        if (FAILED(result = settings->put_IsWebMessageEnabled(FALSE))) return result;

        EventRegistrationToken navigation{}, popup{}, permission{};
        result = browser->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                PWSTR url = nullptr;
                const bool allowed = SUCCEEDED(args->get_Uri(&url)) && IsHttps(url);
                CoTaskMemFree(url);
                return allowed ? S_OK : args->put_Cancel(TRUE);
            }).Get(), &navigation);
        if (FAILED(result)) return result;
        result = browser->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                const auto handled = args->put_Handled(TRUE);
                if (FAILED(handled)) return handled;
                PWSTR url = nullptr;
                if (SUCCEEDED(args->get_Uri(&url)) && IsHttps(url)) sender->Navigate(url);
                CoTaskMemFree(url);
                return S_OK;
            }).Get(), &popup);
        if (FAILED(result)) return result;
        result = browser->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            }).Get(), &permission);
        if (FAILED(result)) return result;
        Resize();
        return browser->Navigate(config_.url.c_str());
    }

    const AppConfig& config_;
    HWND window_ = nullptr;
    int exitCode_ = 0;
    std::wstring profile_;
    ComPtr<ICoreWebView2Controller> controller_;
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    AppConfig config;
    if (!ReadConfig(instance, config) || !IsHttps(config.url.c_str())) {
        MessageBoxW(nullptr, L"Invalid application configuration. Download this app again.", L"Pakr", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    const int exitCode = BrowserWindow(config).Run(instance, show);
    CoUninitialize();
    return exitCode;
}
