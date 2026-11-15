#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <winhttp.h>
#include <wrl.h>
#include <WebView2.h>

#include <algorithm>
#include <cwchar>
#include <sstream>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kControlBarHeight = 30;
constexpr int kCaptionButtonWidth = 38;
constexpr int kResizeGrip = 4;

struct AppConfig {
    std::wstring url;
    std::wstring title;
    std::wstring id;
    int width = 1100;
    int height = 760;
    bool maximized = false;
    bool frameless = false;
};

bool ReadConfig(HINSTANCE instance, AppConfig& config) {
    const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!resource) return false;
    const auto size = SizeofResource(instance, resource);
    const auto data = static_cast<const wchar_t*>(LockResource(LoadResource(instance, resource)));
    if (!data || size == 0 || size % sizeof(wchar_t) != 0) return false;
    std::wistringstream input(std::wstring(data, size / sizeof(wchar_t)));
    if (!std::getline(input, config.url) || !std::getline(input, config.title) || !std::getline(input, config.id)
        || config.url.empty() || config.title.empty() || config.id.empty()) return false;
    std::wstring options;
    if (!std::getline(input, options)) return true;
    int maximized = 0, frameless = 0;
    wchar_t trailing = 0;
    if (swscanf_s(options.c_str(), L"%d,%d,%d,%d%c", &config.width, &config.height, &maximized, &frameless, &trailing, 1u) != 4
        || config.width < 360 || config.width > 3840 || config.height < 280 || config.height > 2160
        || (maximized != 0 && maximized != 1) || (frameless != 0 && frameless != 1)) return false;
    config.maximized = maximized != 0;
    config.frameless = frameless != 0;
    return true;
}

bool IsHttps(PCWSTR url) {
    if (!url || _wcsnicmp(url, L"https://", 8) != 0) return false;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    return WinHttpCrackUrl(url, 0, 0, &parts) && parts.nScheme == INTERNET_SCHEME_HTTPS
        && parts.dwHostNameLength != 0 && parts.dwUserNameLength == 0 && parts.dwPasswordLength == 0;
}

RECT FitWindow(RECT requested, const RECT& work) {
    const LONG width = std::min(requested.right - requested.left, work.right - work.left);
    const LONG height = std::min(requested.bottom - requested.top, work.bottom - work.top);
    const LONG left = std::clamp(requested.left, work.left, work.right - width);
    const LONG top = std::clamp(requested.top, work.top, work.bottom - height);
    return { left, top, left + width, top + height };
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
        windowClass.hbrBackground = config_.frameless
            ? reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))
            : GetSysColorBrush(COLOR_WINDOW);
        if (!RegisterClassW(&windowClass)) return 1;

        RECT work{};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            work.right = GetSystemMetrics(SM_CXSCREEN);
            work.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        const int width = std::min(config_.width, static_cast<int>(work.right - work.left));
        const int height = std::min(config_.height, static_cast<int>(work.bottom - work.top));
        const LONG left = work.left + (work.right - work.left - width) / 2;
        const LONG top = work.top + (work.bottom - work.top - height) / 2;
        normalBounds_ = { left, top, left + width, top + height };
        LoadBounds();
        if (!CreateWindowExW(0, windowClass.lpszClassName, config_.title.c_str(), WS_OVERLAPPEDWINDOW,
                normalBounds_.left, normalBounds_.top, normalBounds_.right - normalBounds_.left,
                normalBounds_.bottom - normalBounds_.top, nullptr, nullptr, instance, this)) return 1;
        if (config_.frameless) {
            const PCWSTR labels[] = { L"最小化", L"最大化", L"关闭" };
            for (int i = 0; i < 3; ++i) {
                controls_[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(101 + i)), instance, nullptr);
                if (!controls_[i]) { DestroyWindow(window_); return 1; }
            }
            Resize();
        }
        ShowWindow(window_, config_.maximized ? SW_SHOWMAXIMIZED : show);
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
    std::wstring PlacementKey() const { return L"Software\\Pakr\\WindowPlacement\\" + config_.id; }

    void LoadBounds() {
        RECT saved{};
        DWORD length = sizeof(saved);
        if (RegGetValueW(HKEY_CURRENT_USER, PlacementKey().c_str(), L"Bounds", RRF_RT_REG_BINARY,
                nullptr, &saved, &length) != ERROR_SUCCESS || length != sizeof(saved)) return;
        // Ignore corrupt values before subtracting coordinates.
        if (saved.left < -1000000 || saved.top < -1000000 || saved.right > 1000000 || saved.bottom > 1000000
            || saved.right <= saved.left || saved.bottom <= saved.top) return;
        MONITORINFO monitor{ sizeof(monitor) };
        if (GetMonitorInfoW(MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST), &monitor)) {
            normalBounds_ = FitWindow(saved, monitor.rcWork);
        }
    }

    void SaveBounds() const {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, PlacementKey().c_str(), 0, nullptr, 0,
                KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        RegSetValueExW(key, L"Bounds", 0, REG_BINARY,
            reinterpret_cast<const BYTE*>(&normalBounds_), sizeof(normalBounds_));
        RegCloseKey(key);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* app = static_cast<BrowserWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        auto* app = reinterpret_cast<BrowserWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (app && app->config_.frameless) {
            if (message == WM_GETMINMAXINFO) {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
                limits->ptMinTrackSize.x = app->Scale(240);
                limits->ptMinTrackSize.y = app->Scale(160);
                return 0;
            }
            if (message == WM_NCCALCSIZE) {
                if (wParam && IsZoomed(window)) {
                    MONITORINFO monitor{ sizeof(monitor) };
                    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
                        reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0] = monitor.rcWork;
                    }
                }
                return 0;
            }
            if (message == WM_NCHITTEST) {
                POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(window, &point);
                RECT client{};
                GetClientRect(window, &client);
                const int grip = app->Scale(kResizeGrip);
                if (!IsZoomed(window)) {
                    const bool left = point.x < grip, right = point.x >= client.right - grip;
                    if (point.y < grip) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
                    if (point.y >= client.bottom - grip) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
                    if (left || right) return left ? HTLEFT : HTRIGHT;
                }
                if (point.y < app->Scale(kControlBarHeight)
                    && point.x < client.right - app->CaptionInset() - 3 * app->Scale(kCaptionButtonWidth)) return HTCAPTION;
                return HTCLIENT;
            }
            if (message == WM_DRAWITEM && wParam >= 101 && wParam <= 103) {
                app->DrawCaptionButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
                return TRUE;
            }
            if (message == WM_COMMAND && lParam && HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                    case 101: ShowWindow(window, SW_MINIMIZE); return 0;
                    case 102: ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE); return 0;
                    case 103: SendMessageW(window, WM_CLOSE, 0, 0); return 0;
                }
            }
        }
        if (app && message == WM_SIZE) app->Resize();
        if (app && message == WM_WINDOWPOSCHANGED && IsWindowVisible(window) && !IsIconic(window) && !IsZoomed(window)) {
            GetWindowRect(window, &app->normalBounds_);
        }
        if (app && message == WM_CLOSE) app->SaveBounds();
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

    int Scale(int value) const {
        const UINT dpi = GetDpiForWindow(window_);
        return MulDiv(value, dpi ? dpi : 96, 96);
    }

    int CaptionInset() const { return IsZoomed(window_) ? 0 : Scale(kResizeGrip); }

    void DrawCaptionButton(const DRAWITEMSTRUCT& item) const {
        const RECT& rect = item.rcItem;
        const bool highContrast = [] {
            HIGHCONTRASTW settings{ sizeof(settings) };
            return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(settings), &settings, 0)
                && (settings.dwFlags & HCF_HIGHCONTRASTON);
        }();
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool close = item.CtlID == 103;
        const COLORREF background = highContrast ? GetSysColor(pressed ? COLOR_HIGHLIGHT : COLOR_WINDOW)
            : pressed ? (close ? RGB(175, 30, 40) : RGB(55, 55, 55)) : RGB(0, 0, 0);
        const auto brush = CreateSolidBrush(background);
        FillRect(item.hDC, &rect, brush);
        DeleteObject(brush);

        const int x = (rect.left + rect.right) / 2;
        const int y = (rect.top + rect.bottom) / 2;
        const int radius = Scale(5);
        const auto pen = CreatePen(PS_SOLID, Scale(1),
            highContrast ? GetSysColor(pressed ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT) : RGB(235, 235, 235));
        const auto oldPen = SelectObject(item.hDC, pen);
        const auto line = [&](int x1, int y1, int x2, int y2) {
            MoveToEx(item.hDC, x1, y1, nullptr);
            LineTo(item.hDC, x2, y2);
        };
        if (close) {
            line(x - radius, y - radius, x + radius, y + radius);
            line(x + radius, y - radius, x - radius, y + radius);
        } else if (item.CtlID == 101) {
            line(x - radius, y, x + radius + 1, y);
        } else {
            const auto oldBrush = SelectObject(item.hDC, GetStockObject(HOLLOW_BRUSH));
            const int offset = IsZoomed(window_) ? Scale(2) : 0;
            if (offset) {
                line(x - radius + offset, y - radius, x + radius, y - radius);
                line(x + radius, y - radius, x + radius, y + radius - offset);
            }
            Rectangle(item.hDC, x - radius, y - radius + offset, x + radius + 1 - offset, y + radius + 1);
            SelectObject(item.hDC, oldBrush);
        }
        SelectObject(item.hDC, oldPen);
        DeleteObject(pen);
        if (item.itemState & ODS_FOCUS) DrawFocusRect(item.hDC, &rect);
    }

    void Resize() {
        RECT bounds{};
        if (!GetClientRect(window_, &bounds)) return;
        if (config_.frameless) {
            const int buttonWidth = Scale(kCaptionButtonWidth);
            const int inset = CaptionInset();
            for (int i = 0; i < 3; ++i) {
                if (controls_[i]) MoveWindow(controls_[i], bounds.right - inset - (3 - i) * buttonWidth,
                    inset, buttonWidth, Scale(kControlBarHeight) - inset, TRUE);
            }
            if (controls_[1]) SetWindowTextW(controls_[1], IsZoomed(window_) ? L"还原" : L"最大化");
            // Keep the WebView off the title strip and the resize hit areas.
            bounds.left = inset;
            bounds.right = std::max<LONG>(inset, bounds.right - inset);
            bounds.bottom = std::max<LONG>(0, bounds.bottom - inset);
            bounds.top = std::min<LONG>(bounds.bottom, Scale(kControlBarHeight));
        }
        if (controller_) controller_->put_Bounds(bounds);
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
    HWND controls_[3]{};
    RECT normalBounds_{};
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
