// webview_host.h - ���°汾
#pragma once
#include <windows.h>
#include <WebView2.h>

// ����C���ݵĺ����ӿ�
#ifdef __cplusplus
extern "C" {
#endif
    // ��ʼ��WebView2����
    void InitWebView2Host(HWND hwnd);

    // ��WebView��������
    void WebViewPostCommand(const wchar_t* command, const wchar_t* payload);

    // WebView2�ؼ���������
    void WebViewSetVisibility(BOOL bVisible);
    void WebViewSetBounds(int x, int y, int width, int height);

    // ���ݲ�������
    void WebViewGetContent(void);
    void WebViewSetContent(const wchar_t* content);

    // ��Ϣ�ص�������������Ҫ��C������ʵ�֣�
    void OnWebViewMessageReceived(const wchar_t* text);

#ifdef __cplusplus
}
#endif