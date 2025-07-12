// webview_host.cpp
#include <windows.h>
#include <shlwapi.h>
#include <WebView2.h>
#include <wrl.h>         // 必须包含，因为这里使用了 ComPtr
#include <wrl/event.h>
#include <wrl/client.h>  // 必须包含，因为这里使用了 ComPtr
#include <string>        // std::wstring
#include "webview_host.h" // 包含我们自己的头文件，获取 C 兼容函数声明

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "ole32.lib")

using namespace Microsoft::WRL;
using namespace std;

// C 侧需要实现这个函数，用来接收编辑后的文本
extern "C" void OnWebViewMessageReceived(const wchar_t* text);

// 全局智能指针，重新添加 static，使其仅在 webview_host.cpp 内部可见
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2>           g_webview;

// 增强的WebViewPostCommand函数，添加错误处理
void WebViewPostCommand(const wchar_t* command, const wchar_t* payload) {
    if (!g_webview || !command) return;

    std::wstring json = L"{\"command\":\"";
    json += command;
    json += L"\", \"payload\": \"";

    // 转义 payload
    if (payload) {
        for (const wchar_t* p = payload; *p; ++p) {
            switch (*p) {
            case L'"':  json += L"\\\""; break;
            case L'\\': json += L"\\\\"; break;
            case L'\n': json += L"\\n"; break;
            case L'\r': json += L"\\r"; break;
            case L'\t': json += L"\\t"; break;
            case L'\b': json += L"\\b"; break;
            case L'\f': json += L"\\f"; break;
            default:
                if (*p >= 0x0000 && *p <= 0x001F) {
                    wchar_t hex[8];
                    swprintf_s(hex, _countof(hex), L"\\u%04x", (unsigned int)*p);
                    json += hex;
                }
                else {
                    json += *p;
                }
                break;
            }
        }
    }

    json += L"\"}";

    // 添加错误处理
    HRESULT hr = g_webview->PostWebMessageAsJson(json.c_str());
    if (FAILED(hr)) {
        // 在调试模式下输出错误信息
#ifdef _DEBUG
        OutputDebugStringA("WebViewPostCommand failed\n");
#endif
    }
}

// 添加获取WebView内容的函数
extern "C" void WebViewGetContent() {
    if (g_webview) {
        WebViewPostCommand(L"getContent", L"");
    }
}

// 添加设置WebView内容的函数
extern "C" void WebViewSetContent(const wchar_t* content) {
    if (g_webview && content) {
        WebViewPostCommand(L"setContent", content);
    }
}

// *** 在这里添加 WebViewSetVisibility 和 WebViewSetBounds 的实现 ***
// 使用 extern "C" 包裹以防止 C++ 名字修饰
extern "C" void WebViewSetVisibility(BOOL bVisible) {
    if (g_controller) { // 检查 g_controller 是否有效
        // 设置 WebView2 控件的可见性
        g_controller->put_IsVisible(bVisible);
    }
}

extern "C" void WebViewSetBounds(int x, int y, int width, int height) {
    if (g_controller) { // 检查 g_controller 是否有效
        // 设置 WebView2 控件的位置和大小
        RECT bounds = { x, y, x + width, y + height };
        g_controller->put_Bounds(bounds);
    }
}


// 在InitWebView2Host函数中，修改WebView初始化部分：
extern "C" void InitWebView2Host(HWND hwnd) {
    // 1) 创建环境
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT envRes, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(envRes) || !env) return envRes;

                // 2) 基于环境创建 Controller
                env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT ctrlRes, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(ctrlRes) || !controller) return ctrlRes;

                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            // 3) 初始设置为不可见
                            g_controller->put_IsVisible(FALSE);

                            // 4) 设置初始大小
                            RECT rc;
                            GetClientRect(hwnd, &rc);
                            g_controller->put_Bounds(rc);

                            // 5) 加载本地 editor.html
                            wchar_t path[MAX_PATH];
                            GetModuleFileNameW(NULL, path, MAX_PATH);
                            PathRemoveFileSpecW(path);
                            wstring uri = L"file:///";
                            uri += path;
                            uri += L"\\editor.html";
                            g_webview->Navigate(uri.c_str());

                            // 6) 在开发阶段可以启用开发者工具
                            // g_webview->OpenDevToolsWindow(); // 生产环境可以注释掉

                            // 7) 接收前端消息
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* /*sender*/,
                                        ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            PWSTR msg;
                                            if (SUCCEEDED(args->TryGetWebMessageAsString(&msg))) {
                                                OnWebViewMessageReceived(msg);
                                                CoTaskMemFree(msg);
                                            }
                                            return S_OK;
                                    }).Get(),
                                        nullptr);

                            // 8) 导航完成后的处理
                            g_webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [](ICoreWebView2* /*sender*/,
                                        ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                            BOOL success;
                                            args->get_IsSuccess(&success);
                                            if (success) {
                                                // 页面加载成功，可以开始与页面交互
                                                // 发送初始化消息
                                                if (g_webview) {
                                                    std::wstring initMsg = L"{\"command\":\"init\", \"payload\":\"\"}";
                                                    g_webview->PostWebMessageAsJson(initMsg.c_str());
                                                }
                                            }
                                            return S_OK;
                                    }).Get(),
                                        nullptr);

                            return S_OK;
                        }).Get());

                return S_OK;
            }).Get());
}