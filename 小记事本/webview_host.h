// webview_host.h - 更新版本
#pragma once
#include <windows.h>
#include <WebView2.h>

// 声明C兼容的函数接口
#ifdef __cplusplus
extern "C" {
#endif
    // 初始化WebView2宿主
    void InitWebView2Host(HWND hwnd);

    // 向WebView发送命令
    void WebViewPostCommand(const wchar_t* command, const wchar_t* payload);

    // WebView2控件操作函数
    void WebViewSetVisibility(BOOL bVisible);
    void WebViewSetBounds(int x, int y, int width, int height);

    // 内容操作函数
    void WebViewGetContent(void);
    void WebViewSetContent(const wchar_t* content);

    // 消息回调函数声明（需要在C代码中实现）
    void OnWebViewMessageReceived(const wchar_t* text);

#ifdef __cplusplus
}
#endif