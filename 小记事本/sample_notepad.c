// simple_notepad.c
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <richedit.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#include "resource.h"
#include "webview_host.h" // 现在包含的是 C 兼容的声明

// <WebView2.h> 如果仅为 g_controller 访问而包含，现在可以移除。
// 但如果文件其他部分直接使用了 WebView2 相关的其他类型，则可以保留。
// 为了解决当前的编译问题，确保这里没有 ComPtr 的直接使用。
// 如果不确定，可以暂时保留，因为 webview_host.h 已经不再暴露 ComPtr。
#include <WebView2.h> 

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

#ifdef __cplusplus
extern "C" {
#endif
    // 声明在 webview_host.cpp 中实现
    void InitWebView2Host(HWND hwnd);
    void WebViewPostCommand(const wchar_t* command, const wchar_t* payload);
    void WebViewSetVisibility(BOOL bVisible); // 新增声明
    void WebViewSetBounds(int x, int y, int width, int height); // 新增声明



#ifdef __cplusplus
}
#endif


// RTF 流出/流入回调保持不变
typedef struct {
    FILE* fp;              // 原本用于文件流
    const BYTE* memPtr;    // 内存数据指针
    size_t memSize;        // 数据总大小
    size_t memPos;         // 当前读取位置
} STREAM_COOKIE;

// 用于内存流出 RTF 的结构和回调
typedef struct {
    BYTE* buf;      // 缓冲区指针
    size_t size;    // 缓冲总容量
    size_t pos;     // 当前写入位置
} MEM_COOKIE;

static DWORD CALLBACK MemStreamOutCallback(DWORD_PTR dwCookie,
    LPBYTE pBuff, LONG cb, LONG* pcb)
{
    MEM_COOKIE* mc = (MEM_COOKIE*)dwCookie;
    // 动态扩容
    if (mc->pos + cb > mc->size) {
        size_t newSize = (mc->size + cb) * 2;
        if (newSize < mc->size) return 1; // 溢出检查
        BYTE* newBuf = (BYTE*)realloc(mc->buf, newSize);
        if (!newBuf) return 1; // realloc 失败，返回非零表示错误
        mc->buf = newBuf;
        mc->size = newSize;
    }
    memcpy(mc->buf + mc->pos, pBuff, cb);
    mc->pos += cb;
    *pcb = cb;
    return 0;
}

DWORD CALLBACK EditStreamOutCallback(DWORD_PTR dwCookie, LPBYTE pBuff, LONG cb, LONG* pcb) {
    STREAM_COOKIE* cookie = (STREAM_COOKIE*)dwCookie;
    size_t written = fwrite(pBuff, 1, cb, cookie->fp);
    *pcb = (LONG)written;
    return 0;
}

DWORD CALLBACK EditStreamInCallback(DWORD_PTR dwCookie, LPBYTE pBuff, LONG cb, LONG* pcb) {
    STREAM_COOKIE* cookie = (STREAM_COOKIE*)dwCookie;

    if (cookie->fp) {
        size_t read = fread(pBuff, 1, cb, cookie->fp);
        *pcb = (LONG)read;
        return 0;
    }
    else if (cookie->memPtr) {
        size_t remaining = cookie->memSize - cookie->memPos;
        size_t toRead = min((size_t)cb, remaining);
        memcpy(pBuff, cookie->memPtr + cookie->memPos, toRead);
        cookie->memPos += toRead;
        *pcb = (LONG)toRead;
        return 0;
    }

    *pcb = 0;
    return 1; // 错误
}

static HINSTANCE g_hInst;
static BOOL g_isDirty = FALSE;
static HFONT g_hFont = NULL; // 声明为静态变量，以便在 WM_DESTROY 中释放
static HANDLE g_hResFont = NULL; // 声明为静态变量，以便在 WM_DESTROY 中释放

// 全局变量用于存储从 RichEdit 获取的纯文本
static WCHAR* g_plainTextBuf = NULL; // 用于存储纯文本的缓冲区
static int g_plainTextLen = 0;       // 纯文本的长度（不包含 null 终止符）


#define ID_EDIT             1001
#define ID_FILE_EXIT        2001
#define ID_EDIT_HIDE        2002
#define ID_FILE_SAVE_RTF    2003    // 用 RTF 格式保存
#define ID_FILE_OPEN_RTF    2004    // 用 RTF 格式打开
#define ID_TOGGLE_HIDDEN    2005    // 切换显示隐藏文字
#define ID_EDIT_PASTE_RTF   2006
#define ID_EDIT_COPY_BOTH   2007
#define ID_EDIT_UNDO        2008
#define ID_VIEW_VERTICAL    3001
#define ID_VIEW_HORIZONTAL  3002
#define ID_START_TIMER      4001
#define ID_TIMER            4002

#define EM_BEGINUNDO    (WM_USER+217)
#define EM_ENDUNDO      (WM_USER+218)


// 定义快捷键映射：Ctrl+C → 复制(RTF+文本)，Ctrl+V → 粘贴，Ctrl+H → 隐藏选中，Ctrl+Shift+H → 切换显示
ACCEL g_accelTable[] = {
    { FVIRTKEY | FCONTROL,      'C',  ID_EDIT_COPY_BOTH  },
    { FVIRTKEY | FCONTROL,      'V',  ID_EDIT_PASTE_RTF  },
    { FVIRTKEY | FCONTROL,      'H',  ID_EDIT_HIDE       },
    { FVIRTKEY | FCONTROL,      'Z',  ID_EDIT_UNDO       },
    { FVIRTKEY | FCONTROL | FSHIFT,'H',  ID_TOGGLE_HIDDEN   },
};
const int g_accelCount = sizeof(g_accelTable) / sizeof(g_accelTable[0]);



// 重新引入并优化用来存放所有用户标记为“隐藏”的区段
#define MAX_HIDDEN_RANGES 1024 // 增加容量以应对更多隐藏区域
static CHARRANGE g_hiddenRanges[MAX_HIDDEN_RANGES];
static int         g_hiddenCount = 0;

// 一个标志：当前是在“查看隐藏”模式，还是“隐藏视图”模式
static BOOL        g_showHidden = FALSE;

// 全局 Rich Edit 控件句柄
static HWND g_hEdit;


// 扫描整个文档，把所有 CFE_HIDDEN 的区段记录到 g_hiddenRanges
// 这个函数在文本内容变化时被调用，以更新隐藏区域列表
void ScanHiddenRanges() {
    g_hiddenCount = 0;
    int len = GetWindowTextLengthW(g_hEdit);

    // 保存当前选区，以便在扫描后恢复
    CHARRANGE crOriginal;
    SendMessage(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&crOriginal);

    for (int i = 0; i < len; ++i) {
        // 选中 [i, i+1)
        CHARRANGE cr = { i, i + 1 };
        SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);

        // 取这个字符的格式
        CHARFORMAT2 cf = { sizeof(cf) };
        cf.cbSize = sizeof(cf); // 必须设置 cbSize
        cf.dwMask = CFM_HIDDEN; // 只获取隐藏属性
        SendMessage(g_hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

        if (cf.dwEffects & CFE_HIDDEN) {
            // 发现隐藏起点，继续向后找到末尾
            int start = i;
            while (i + 1 < len) {
                CHARRANGE cr2 = { i + 1, i + 2 };
                SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr2);
                CHARFORMAT2 cf2 = { sizeof(cf2) };
                cf2.cbSize = sizeof(cf2);
                cf2.dwMask = CFM_HIDDEN;
                SendMessage(g_hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf2);
                if (cf2.dwEffects & CFE_HIDDEN) {
                    ++i;  // 仍在隐藏区段
                }
                else {
                    break;
                }
            }
            // 记录整个隐藏区段 [start, i+1)
            if (g_hiddenCount < MAX_HIDDEN_RANGES) {
                g_hiddenRanges[g_hiddenCount++] = (CHARRANGE){ start, i + 1 };
            }
            else {
                // 超出隐藏范围限制，给出警告
                MessageBoxW(NULL, L"隐藏区域数量超出限制，部分隐藏区域可能未被正确跟踪。", L"警告", MB_OK | MB_ICONWARNING);
                break; // 停止扫描
            }
        }
    }
    // 恢复原来的选区
    SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&crOriginal);
}

// 保存 RTF 的函数
void DoSaveRtf(HWND hwnd) {
    OPENFILENAMEW ofn = { 0 };
    WCHAR szFile[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Rich Text 格式\0*.rtf\0所有文件\0*.*\0\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;  // 用户取消

    FILE* fp = _wfopen(szFile, L"wb");
    if (!fp) {
        MessageBoxW(hwnd, L"无法创建文件！", L"保存失败", MB_OK | MB_ICONERROR);
        return;
    }

    STREAM_COOKIE cookie = { 0 };
    cookie.fp = fp;

    EDITSTREAM es = { 0 };
    es.dwCookie = (DWORD_PTR)&cookie;
    es.pfnCallback = EditStreamOutCallback;

    // 注意加上 SF_UNICODE
    LRESULT res = SendMessage(g_hEdit,
        EM_STREAMOUT,
        SF_RTF | SF_UNICODE,
        (LPARAM)&es);
    fclose(fp);

    if (res < 0) {
        MessageBoxW(hwnd, L"保存RTF失败，文件可能不可写或磁盘错误。",
            L"保存错误", MB_OK | MB_ICONERROR);
        return;
    }

    // 成功才清除脏标志
    g_isDirty = FALSE;
}

// 函数：从 RichEdit 控件获取纯文本
// hEdit: RichEdit 控件的句柄
// ppBuf: 指向存储纯文本缓冲区的指针的指针
// pLen: 指向存储纯文本长度的指针
void DoGetPlainTextFromEdit(HWND hEdit, WCHAR** ppBuf, int* pLen) {
    // 获取文本的长度（包含 null 终止符）
    int textLength = GetWindowTextLengthW(hEdit);

    // 如果之前有分配的缓冲区，先释放掉
    if (*ppBuf != NULL) {
        free(*ppBuf);
        *ppBuf = NULL;
    }

    if (textLength > 0) {
        // 分配足够的内存来存储文本 + null 终止符
        *ppBuf = (WCHAR*)malloc((textLength + 1) * sizeof(WCHAR));
        if (*ppBuf == NULL) {
            *pLen = 0;
            // 处理内存分配失败的情况，例如显示错误消息
            MessageBoxW(NULL, L"内存分配失败！", L"错误", MB_OK | MB_ICONERROR);
            return;
        }
        // 获取文本内容
        GetWindowTextW(hEdit, *ppBuf, textLength + 1);
        *pLen = textLength; // 记录实际文本长度（不包含 null 终止符）
    }
    else {
        *pLen = 0;
        *ppBuf = NULL; // 如果没有文本，确保缓冲区指针为 NULL
    }
}

// 新函数：从 RichEdit 提取内容，并将 CFE_HIDDEN 格式的文本用 [HIDDEN]...[/HIDDEN] 包裹
// 返回一个新分配的 WCHAR* 缓冲区。调用者必须使用 free() 释放它。
WCHAR* GetContentWithHiddenMarkersFromRichEdit(HWND hEdit, int* pLen) {
    int len = GetWindowTextLengthW(hEdit);
    if (len == 0) {
        *pLen = 0;
        return NULL;
    }

    // 初始缓冲区大小，如果需要会重新分配
    // 估计每段隐藏文本会增加 [HIDDEN] 和 [/HIDDEN] 标签的长度 (8+9=17字符)
    // 假设最多 MAX_HIDDEN_RANGES 个隐藏区域，增加的额外空间 = MAX_HIDDEN_RANGES * 17
    size_t initialBufSize = (len + 1 + MAX_HIDDEN_RANGES * 17) * sizeof(WCHAR);
    WCHAR* buffer = (WCHAR*)malloc(initialBufSize);
    if (!buffer) {
        *pLen = 0;
        return NULL;
    }
    buffer[0] = L'\0'; // 确保以 null 结尾

    // 保存当前选区
    CHARRANGE crOriginal;
    SendMessage(hEdit, EM_EXGETSEL, 0, (LPARAM)&crOriginal);

    long currentOutputLen = 0;
    const WCHAR* HIDDEN_START_TAG = L"[HIDDEN]";
    const WCHAR* HIDDEN_END_TAG = L"[/HIDDEN]";
    size_t startTagLen = wcslen(HIDDEN_START_TAG);
    size_t endTagLen = wcslen(HIDDEN_END_TAG);

    for (int i = 0; i < len; ) {
        CHARRANGE cr = { i, i + 1 };
        SendMessage(hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);

        CHARFORMAT2 cf = { sizeof(cf) };
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_HIDDEN;
        SendMessage(hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

        if (cf.dwEffects & CFE_HIDDEN) {
            // 追加 [HIDDEN] 标签
            if (currentOutputLen + startTagLen + 1 > initialBufSize / sizeof(WCHAR)) {
                initialBufSize = (currentOutputLen + startTagLen + 1) * 2 * sizeof(WCHAR);
                WCHAR* newBuffer = (WCHAR*)realloc(buffer, initialBufSize);
                if (!newBuffer) { free(buffer); *pLen = 0; return NULL; }
                buffer = newBuffer;
            }
            wcscpy_s(buffer + currentOutputLen, (initialBufSize / sizeof(WCHAR)) - currentOutputLen, HIDDEN_START_TAG);
            currentOutputLen += startTagLen;

            while (i < len) {
                CHARRANGE cr2 = { i, i + 1 };
                SendMessage(hEdit, EM_EXSETSEL, 0, (LPARAM)&cr2);
                CHARFORMAT2 cf2 = { sizeof(cf2) };
                cf2.cbSize = sizeof(cf2);
                cf2.dwMask = CFM_HIDDEN;
                SendMessage(hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf2);

                if (cf2.dwEffects & CFE_HIDDEN) {
                    if (currentOutputLen + 2 > initialBufSize / sizeof(WCHAR)) { // +1 for char, +1 for null terminator
                        initialBufSize = (currentOutputLen + 2) * 2 * sizeof(WCHAR);
                        WCHAR* newBuffer = (WCHAR*)realloc(buffer, initialBufSize);
                        if (!newBuffer) { free(buffer); *pLen = 0; return NULL; }
                        buffer = newBuffer;
                    }
                    WCHAR ch[2] = { 0 };
                    SendMessage(hEdit, EM_GETSELTEXT, 0, (LPARAM)&ch);
                    buffer[currentOutputLen++] = ch[0];
                    buffer[currentOutputLen] = L'\0'; // Null-terminate
                    ++i;
                }
                else {
                    break;
                }
            }
            // 追加 [/HIDDEN] 标签
            if (currentOutputLen + endTagLen + 1 > initialBufSize / sizeof(WCHAR)) {
                initialBufSize = (currentOutputLen + endTagLen + 1) * 2 * sizeof(WCHAR);
                WCHAR* newBuffer = (WCHAR*)realloc(buffer, initialBufSize);
                if (!newBuffer) { free(buffer); *pLen = 0; return NULL; }
                buffer = newBuffer;
            }
            wcscpy_s(buffer + currentOutputLen, (initialBufSize / sizeof(WCHAR)) - currentOutputLen, HIDDEN_END_TAG);
            currentOutputLen += endTagLen;

        }
        else {
            if (currentOutputLen + 2 > initialBufSize / sizeof(WCHAR)) {
                initialBufSize = (currentOutputLen + 2) * 2 * sizeof(WCHAR);
                WCHAR* newBuffer = (WCHAR*)realloc(buffer, initialBufSize);
                if (!newBuffer) { free(buffer); *pLen = 0; return NULL; }
                buffer = newBuffer;
            }
            WCHAR ch[2] = { 0 };
            SendMessage(hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);
            SendMessage(hEdit, EM_GETSELTEXT, 0, (LPARAM)&ch);
            buffer[currentOutputLen++] = ch[0];
            buffer[currentOutputLen] = L'\0'; // Null-terminate
            ++i;
        }
    }

    // 恢复原来的选区
    SendMessage(hEdit, EM_EXSETSEL, 0, (LPARAM)&crOriginal);

    *pLen = currentOutputLen;
    // 将缓冲区裁剪到准确的大小 + null 终止符
    WCHAR* finalBuffer = (WCHAR*)realloc(buffer, (*pLen + 1) * sizeof(WCHAR));
    if (finalBuffer) {
        return finalBuffer;
    }
    else {
        free(buffer); // realloc 失败时释放旧的缓冲区
        return NULL;
    }
}

// 新函数：设置 RichEdit 内容，并根据 [HIDDEN] 标记应用 CFE_HIDDEN 格式
void SetRichEditContentWithHiddenMarkers(HWND hEdit, const wchar_t* content) {
    if (!hEdit || !content) {
        SendMessage(hEdit, EM_SETTEXTEX, (WPARAM)SCF_ALL, (LPARAM)NULL); // 清空现有内容
        return;
    }

    SendMessage(hEdit, EM_SETTEXTEX, (WPARAM)SCF_ALL, (LPARAM)NULL); // 清空现有内容和格式

    const WCHAR* pCurrent = content;

    // 保存当前选区以备恢复
    CHARRANGE crOriginal;
    SendMessage(hEdit, EM_EXGETSEL, 0, (LPARAM)&crOriginal);

    // 禁用撤销，以防止中间状态被记录
    SendMessage(hEdit, EM_BEGINUNDO, 0, 0);

    const WCHAR* HIDDEN_START_TAG = L"[HIDDEN]";
    const WCHAR* HIDDEN_END_TAG = L"[/HIDDEN]";
    size_t startTagLen = wcslen(HIDDEN_START_TAG);
    size_t endTagLen = wcslen(HIDDEN_END_TAG);

    while (*pCurrent != L'\0') {
        const WCHAR* tagStart = wcsstr(pCurrent, HIDDEN_START_TAG);

        if (tagStart == NULL) {
            // 没有更多隐藏标签，追加剩余内容
            if (*pCurrent != L'\0') {
                SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)pCurrent);
            }
            break;
        }
        else {
            // 追加 [HIDDEN] 标签之前的普通文本
            if (tagStart > pCurrent) {
                size_t normalTextLen = tagStart - pCurrent;
                WCHAR* normalText = (WCHAR*)malloc((normalTextLen + 1) * sizeof(WCHAR));
                if (normalText) {
                    wcsncpy_s(normalText, normalTextLen + 1, pCurrent, normalTextLen);
                    normalText[normalTextLen] = L'\0';
                    SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)normalText);
                    free(normalText);
                }
            }

            // 查找对应的 [/HIDDEN] 标签
            const WCHAR* hiddenContentStart = tagStart + startTagLen;
            const WCHAR* tagEnd = wcsstr(hiddenContentStart, HIDDEN_END_TAG);

            if (tagEnd != NULL) {
                // 提取隐藏文本
                size_t hiddenTextLen = tagEnd - hiddenContentStart;
                WCHAR* hiddenText = (WCHAR*)malloc((hiddenTextLen + 1) * sizeof(WCHAR));
                if (hiddenText) {
                    wcsncpy_s(hiddenText, hiddenTextLen + 1, hiddenContentStart, hiddenTextLen);
                    hiddenText[hiddenTextLen] = L'\0';

                    long currentRichEditLen = GetWindowTextLengthW(hEdit);

                    // 插入隐藏文本
                    SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)hiddenText);

                    // 对新插入的文本应用隐藏格式
                    CHARRANGE crApply = { currentRichEditLen, currentRichEditLen + (long)hiddenTextLen };
                    SendMessage(hEdit, EM_EXSETSEL, 0, (LPARAM)&crApply);

                    CHARFORMAT2 cf = { sizeof(cf) };
                    cf.cbSize = sizeof(cf);
                    cf.dwMask = CFM_HIDDEN;
                    cf.dwEffects = CFE_HIDDEN; // 设置隐藏标志
                    SendMessage(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

                    free(hiddenText);
                }
                // 移动 pCurrent 到关闭标签之后
                pCurrent = tagEnd + endTagLen;
            }
            else {
                // 格式错误：[HIDDEN] 没有 [/HIDDEN]，将剩余部分视为普通文本
                SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)tagStart);
                pCurrent = tagStart + wcslen(tagStart); // 移动到末尾
            }
        }
    }

    SendMessage(hEdit, EM_ENDUNDO, 0, 0); // 结束撤销块
    SendMessage(hEdit, EM_EXSETSEL, 0, (LPARAM)&crOriginal); // 恢复选区
    ScanHiddenRanges(); // 设置内容后重新扫描隐藏区域
}


// *** 新增：模式切换函数 ***
static BOOL g_isWebViewMode = FALSE;
static BOOL g_webViewInitialized = FALSE;

void SwitchToWebViewMode(HWND hwnd) {
    if (!g_isWebViewMode) {
        // 1. 获取RichEdit中的内容，并转换隐藏标记
        if (g_plainTextBuf) {
            free(g_plainTextBuf);
            g_plainTextBuf = NULL;
            g_plainTextLen = 0;
        }
        g_plainTextBuf = GetContentWithHiddenMarkersFromRichEdit(g_hEdit, &g_plainTextLen);

        // 2. 隐藏RichEdit
        ShowWindow(g_hEdit, SW_HIDE);

        // 3. 显示WebView并发送内容
        WebViewSetVisibility(TRUE);

        if (g_plainTextBuf) { // 检查内容是否存在
            WebViewSetContent(g_plainTextBuf);
        }
        else {
            WebViewSetContent(L""); // 如果 RichEdit 为空，则清空 WebView
        }

        g_isWebViewMode = TRUE;

        // 4. 更新窗口标题
        SetWindowTextW(hwnd, L"可恶的好朋友的记事本:P - 竖排模式");
    }
}

void SwitchToRichEditMode(HWND hwnd) {
    if (g_isWebViewMode) {
        // 1. 请求WebView发送当前内容 (将通过 OnWebViewMessageReceived 异步处理)
        WebViewPostCommand(L"getContent", L"");

        // 2. 隐藏WebView
        WebViewSetVisibility(FALSE);

        // 3. 显示RichEdit
        // 内容将由 OnWebViewMessageReceived 异步设置。
        // 现在只需显示它。
        ShowWindow(g_hEdit, SW_SHOW);
        SetFocus(g_hEdit);

        g_isWebViewMode = FALSE;

        // 4. 更新窗口标题
        SetWindowTextW(hwnd, L"可恶的好朋友的记事本:P - 横排模式");
    }
}



LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// -----------------------------------------
// WebView 调用回调：将 Web 编辑器中内容插入 RichEdit 控件
// -----------------------------------------
// OnWebViewMessageReceived 函数（保持不变，确保在 extern "C" 块内）
// *** 增强的WebView消息处理 ***
#ifdef __cplusplus
extern "C" {
#endif
    void OnWebViewMessageReceived(const wchar_t* text) {
        if (!text) return;

        // 简单的JSON解析 - 在实际应用中你可能需要更robust的解析
        if (wcsstr(text, L"\"type\":\"contentResponse\"")) {
            // 这是从WebView获取的内容响应
            // 提取content字段的值
            const wchar_t* contentStart = wcsstr(text, L"\"content\":\"");
            if (contentStart) {
                contentStart += 11; // 跳过 "content":"
                const wchar_t* contentEnd = wcsstr(contentStart, L"\"");
                if (contentEnd) {
                    size_t len = contentEnd - contentStart;
                    if (len > 0) {
                        wchar_t* content = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
                        if (content) {
                            wcsncpy_s(content, len + 1, contentStart, len);
                            content[len] = L'\0';

                            // 更新RichEdit内容（如果当前不在WebView模式）
                            // 即使当前在WebView模式，也接收并处理内容，为下次切换做准备
                            SetRichEditContentWithHiddenMarkers(g_hEdit, content);


                            free(content);
                        }
                    }
                }
            }
        }
        else if (wcsstr(text, L"\"type\":\"contentChanged\"")) {
            // 内容发生变化
            g_isDirty = TRUE;
        }
        else if (wcsstr(text, L"\"type\":\"ready\"")) {
            // WebView已就绪
            g_webViewInitialized = TRUE;
        }
        else if (wcsstr(text, L"\"type\":\"clipboardContentResponse\"")) { // 新增的剪贴板内容响应
            const wchar_t* contentStart = wcsstr(text, L"\"content\":\"");
            if (contentStart) {
                contentStart += 11;
                const wchar_t* contentEnd = wcsstr(contentStart, L"\"");
                if (contentEnd) {
                    size_t len = contentEnd - contentStart;
                    if (len > 0) {
                        // 分配并复制内容
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(WCHAR));
                        if (hMem) {
                            WCHAR* pBuf = (WCHAR*)GlobalLock(hMem);
                            if (pBuf) {
                                wcsncpy_s(pBuf, len + 1, contentStart, len);
                                pBuf[len] = L'\0';
                                GlobalUnlock(hMem);

                                if (OpenClipboard(NULL)) { // 使用 NULL 作为所有者窗口以避免问题
                                    EmptyClipboard();
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                    CloseClipboard();
                                }
                                else {
                                    GlobalFree(hMem); // 如果无法打开剪贴板，则释放
                                }
                            }
                            else {
                                GlobalFree(hMem);
                            }
                        }
                    }
                }
            }
        }
    }
#ifdef __cplusplus
}
#endif

// 创建并注册窗口类
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine, int nCmdShow) {
    // 确保 Msftedit.dll 被加载，这是 Rich Edit Control 2.0 (or higher)
    // 只有 LoadLibraryW(L"Msftedit.dll"); 才能获得最新版本的 Rich Edit 控件
    LoadLibraryW(L"Msftedit.dll");
    g_hInst = hInst;
    // 初始化公共控件（为了兼容新系统）
    InitCommonControls();

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SimpleNotepadClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClass(&wc)) {
        MessageBox(NULL, L"注册窗口类失败", L"错误", MB_ICONERROR);
        return 0;
    }

    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"可恶的好朋友的记事本:P",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInst, NULL
    );

    if (!hwnd) {
        MessageBox(NULL, L"创建窗口失败", L"错误", MB_ICONERROR);
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    HACCEL hAccel = CreateAcceleratorTable(g_accelTable, g_accelCount);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        // 优先让加速表消化消息
        if (!TranslateAccelerator(hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // 应用程序退出时，销毁加速表
    if (hAccel) DestroyAcceleratorTable(hAccel);

    return (int)msg.wParam;
}

// 窗口过程函数：处理消息
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    static int  secondsLeft = 0;

    switch (msg) {
    case WM_CREATE: {
        // 创建菜单
        HMENU hMenu = CreateMenu();
        HMENU hFile = CreatePopupMenu();
        AppendMenu(hFile, MF_STRING, ID_FILE_OPEN_RTF, L"&打开\tCtrl+O");
        AppendMenu(hFile, MF_STRING, ID_FILE_SAVE_RTF, L"&保存\tCtrl+S");
        AppendMenu(hFile, MF_SEPARATOR, 0, NULL);
        AppendMenu(hFile, MF_STRING, ID_FILE_EXIT, L"退出\tAlt+F4");
        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hFile, L"&文件");
        HMENU hEditMenu = CreatePopupMenu();
        AppendMenuW(hEditMenu, MF_STRING, ID_EDIT_HIDE, L"隐藏选中内容\tCtrl+H");
        AppendMenuW(hEditMenu, MF_STRING, ID_EDIT_PASTE_RTF, L"粘贴（支持RTF/文本）\tCtrl+V");
        AppendMenuW(hEditMenu, MF_STRING, ID_EDIT_COPY_BOTH, L"复制（RTF + 文本）\tCtrl+C");
        AppendMenuW(hEditMenu, MF_STRING, ID_EDIT_UNDO, L"撤销\tCtrl+Z");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hEditMenu, L"&编辑");
        HMENU hView = CreatePopupMenu();
        AppendMenuW(hView, MF_STRING, ID_TOGGLE_HIDDEN, L"切换隐藏文字可见状态\tCtrl+Shift+H");
        AppendMenuW(hView, MF_STRING, ID_VIEW_VERTICAL, L"切换竖排显示"); // 更改为纯显示模式
        AppendMenuW(hView, MF_STRING, ID_VIEW_HORIZONTAL, L"切换横排显示");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hView, L"&视图");
        HMENU hFunctionMenu = CreatePopupMenu();
        AppendMenuW(hFunctionMenu, MF_STRING, ID_START_TIMER, L"开始倒计时 60s\tCtrl+T");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFunctionMenu, L"&更多功能");
        SetMenu(hwnd, hMenu);

        // 1) 创建 RichEdit 控件（最初包含了 WS_HSCROLL 和 ES_AUTOHSCROLL）
        g_hEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,      // 扩展样式：3D 边框
            MSFTEDIT_CLASS,        // 类名
            L"",                   // 初始文本
            WS_CHILD | WS_VISIBLE  // 窗口样式
            | WS_VSCROLL           // 纵向滚动
            | ES_MULTILINE         // 支持多行
            | ES_AUTOVSCROLL       // 自动纵向滚动
            | ES_WANTRETURN,       // 回车插入新行
            0, 0, 0, 0,            // 位置与大小交给 WM_SIZE
            hwnd, (HMENU)ID_EDIT,  // 父窗口 & 控件 ID
            g_hInst, NULL);        // 实例句柄 & 参数



        // 2) 从资源加载字体
        HRSRC hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDR_MYFONT), RT_RCDATA);
        if (hRes) {
            HGLOBAL hGlobal = LoadResource(g_hInst, hRes);
            if (hGlobal) {
                LPVOID pData = LockResource(hGlobal);
                DWORD dwSize = SizeofResource(g_hInst, hRes);
                if (pData && dwSize > 0) {
                    DWORD dwNumFonts = 0;
                    g_hResFont = AddFontMemResourceEx(pData, dwSize, 0, &dwNumFonts);
                    if (g_hResFont) {
                        g_hFont = CreateFontW(
                            18,                    // 字体高度
                            0,                     // 宽度（0=自动）
                            0,                     // 字体倾斜角度
                            0,                     // 字体倾斜角度
                            FW_NORMAL,             // 字体粗细
                            FALSE,                 // 是否斜体
                            FALSE,                 // 是否下划线
                            FALSE,                 // 是否删除线
                            DEFAULT_CHARSET,       // 字符集
                            OUT_DEFAULT_PRECIS,    // 输出精度
                            CLIP_DEFAULT_PRECIS,   // 剪辑精度
                            DEFAULT_QUALITY,       // 输出质量
                            DEFAULT_PITCH | FF_DONTCARE, // 字体间距和族
                            L"汇文港黑"           // 字体名称
                        );

                        if (g_hFont) {
                            SendMessage(g_hEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
                        }
                    }
                }
            }
        }

        // 3) 初始化WebView2（但不显示）
        // WebView2会在后台初始化，但默认不可见
        InitWebView2Host(hwnd);

        return 0;
    }

    case WM_SIZE: {
        // 只调整当前可见的控件大小
        if (g_hEdit) {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);

            // 调整RichEdit大小
            MoveWindow(g_hEdit, 0, 0, width, height, TRUE);

            // 同时调整WebView大小（即使不可见）
            WebViewSetBounds(0, 0, width, height);
        }
        return 0;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        case ID_FILE_OPEN_RTF: {
            // 打开RTF文件
            OPENFILENAMEW ofn = { 0 };
            WCHAR szFile[MAX_PATH] = L"";
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Rich Text 格式\0*.rtf\0所有文件\0*.*\0\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

            if (GetOpenFileNameW(&ofn)) {
                FILE* fp = _wfopen(szFile, L"rb");
                if (fp) {
                    STREAM_COOKIE cookie = { 0 };
                    cookie.fp = fp;

                    EDITSTREAM es = { 0 };
                    es.dwCookie = (DWORD_PTR)&cookie;
                    es.pfnCallback = EditStreamInCallback;

                    SendMessage(g_hEdit, EM_STREAMIN, SF_RTF | SF_UNICODE, (LPARAM)&es);
                    fclose(fp);

                    // 重新扫描隐藏区域
                    ScanHiddenRanges();
                    g_isDirty = FALSE;
                }
            }
            break;
        }

        case ID_FILE_SAVE_RTF: {
            DoSaveRtf(hwnd);
            break;
        }

        case ID_FILE_EXIT: {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        }

        case ID_EDIT_HIDE: {
            // 隐藏选中的文本 (RichEdit specific)
            CHARRANGE cr;
            SendMessage(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&cr);

            if (cr.cpMin != cr.cpMax) {
                CHARFORMAT2 cf = { sizeof(cf) };
                cf.dwMask = CFM_HIDDEN;
                cf.dwEffects = CFE_HIDDEN;

                SendMessage(g_hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
                ScanHiddenRanges(); // 重新扫描隐藏区域
                g_isDirty = TRUE;
            }
            break;
        }

        case ID_TOGGLE_HIDDEN: {
            if (g_isWebViewMode) {
                // 向 WebView 发送切换隐藏文本的命令
                WebViewPostCommand(L"toggleHidden", L"");
            }
            else {
                // RichEdit 模式下的切换隐藏文字显示状态
                g_showHidden = !g_showHidden;

                for (int i = 0; i < g_hiddenCount; ++i) {
                    SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&g_hiddenRanges[i]);

                    CHARFORMAT2 cf = { sizeof(cf) };
                    cf.dwMask = CFM_HIDDEN;
                    cf.dwEffects = g_showHidden ? 0 : CFE_HIDDEN;

                    SendMessage(g_hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
                }

                // 恢复选择
                CHARRANGE cr = { 0, 0 };
                SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);
            }
            break;
        }

        case ID_EDIT_COPY_BOTH: {
            if (g_isWebViewMode) {
                // WebView 模式下，请求 WebView 发送内容到剪贴板
                WebViewPostCommand(L"getClipboardContent", L"");
            }
            else {
                // RichEdit 模式下，复制RTF和纯文本到剪贴板
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();

                    // 获取RTF格式
                    MEM_COOKIE mc = { 0 };
                    mc.size = 1024;
                    mc.buf = (BYTE*)malloc(mc.size);

                    if (mc.buf) {
                        EDITSTREAM es = { 0 };
                        es.dwCookie = (DWORD_PTR)&mc;
                        es.pfnCallback = MemStreamOutCallback;

                        SendMessage(g_hEdit, EM_STREAMOUT, SF_RTF | SFF_SELECTION, (LPARAM)&es);

                        if (mc.pos > 0) {
                            // 注册RTF格式
                            UINT rtfFormat = RegisterClipboardFormatW(L"Rich Text Format");
                            HGLOBAL hRtf = GlobalAlloc(GMEM_MOVEABLE, mc.pos + 1);
                            if (hRtf) {
                                char* pRtf = (char*)GlobalLock(hRtf);
                                memcpy(pRtf, mc.buf, mc.pos);
                                pRtf[mc.pos] = '\0';
                                GlobalUnlock(hRtf);
                                SetClipboardData(rtfFormat, hRtf);
                            }
                        }

                        free(mc.buf);
                    }

                    // 获取纯文本
                    CHARRANGE cr;
                    SendMessage(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&cr);

                    if (cr.cpMin != cr.cpMax) {
                        int len = cr.cpMax - cr.cpMin;
                        WCHAR* text = (WCHAR*)malloc((len + 1) * sizeof(WCHAR));
                        if (text) {
                            SendMessage(g_hEdit, EM_GETSELTEXT, 0, (LPARAM)text);

                            HGLOBAL hText = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(WCHAR));
                            if (hText) {
                                WCHAR* pText = (WCHAR*)GlobalLock(hText);
                                wcscpy_s(pText, len + 1, text);
                                GlobalUnlock(hText);
                                SetClipboardData(CF_UNICODETEXT, hText);
                            }

                            free(text);
                        }
                    }
                    CloseClipboard();
                }
            }
            break;
        }

        case ID_EDIT_PASTE_RTF: {
            if (g_isWebViewMode) {
                // WebView 模式下，只支持粘贴纯文本
                if (OpenClipboard(hwnd)) {
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (hData) {
                        WCHAR* pText = (WCHAR*)GlobalLock(hData);
                        if (pText) {
                            WebViewPostCommand(L"pasteContent", pText);
                            GlobalUnlock(hData);
                            g_isDirty = TRUE; // 内容发生变化
                        }
                    }
                    CloseClipboard();
                }
            }
            else {
                // RichEdit 模式下，粘贴RTF或纯文本
                if (OpenClipboard(hwnd)) {
                    UINT rtfFormat = RegisterClipboardFormatW(L"Rich Text Format");
                    HANDLE hData = GetClipboardData(rtfFormat);

                    if (hData) {
                        // 粘贴RTF
                        char* pRtf = (char*)GlobalLock(hData);
                        if (pRtf) {
                            STREAM_COOKIE cookie = { 0 };
                            cookie.memPtr = (BYTE*)pRtf;
                            cookie.memSize = strlen(pRtf);
                            cookie.memPos = 0;

                            EDITSTREAM es = { 0 };
                            es.dwCookie = (DWORD_PTR)&cookie;
                            es.pfnCallback = EditStreamInCallback;

                            SendMessage(g_hEdit, EM_STREAMIN, SF_RTF | SFF_SELECTION, (LPARAM)&es);
                            ScanHiddenRanges();
                            g_isDirty = TRUE;

                            GlobalUnlock(hData);
                        }
                    }
                    else {
                        // 粘贴纯文本
                        hData = GetClipboardData(CF_UNICODETEXT);
                        if (hData) {
                            WCHAR* pText = (WCHAR*)GlobalLock(hData);
                            if (pText) {
                                SendMessage(g_hEdit, EM_REPLACESEL, TRUE, (LPARAM)pText);
                                g_isDirty = TRUE;
                                GlobalUnlock(hData);
                            }
                        }
                    }

                    CloseClipboard();
                }
            }
            break;
        }

        case ID_EDIT_UNDO: {
            SendMessage(g_hEdit, EM_UNDO, 0, 0);
            break;
        }

                         // *** 新增：WebView模式切换 ***
        case ID_VIEW_VERTICAL: {
            // 切换到竖排显示模式（WebView）
            SwitchToWebViewMode(hwnd);
            break;
        }

        case ID_VIEW_HORIZONTAL: {
            // 切换到横排显示模式（RichEdit）
            SwitchToRichEditMode(hwnd);
            break;
        }

        case ID_START_TIMER: {
            // 开始60秒倒计时
            secondsLeft = 60;
            SetTimer(hwnd, ID_TIMER, 1000, NULL);

            WCHAR title[256];
            swprintf_s(title, sizeof(title) / sizeof(WCHAR), L"可恶的好朋友的记事本:P - 倒计时: %d秒", secondsLeft);
            SetWindowTextW(hwnd, title);
            break;
        }

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == ID_TIMER) {
            secondsLeft--;
            if (secondsLeft <= 0) {
                KillTimer(hwnd, ID_TIMER);
                SetWindowTextW(hwnd, L"可恶的好朋友的记事本:P - 时间到！");
                MessageBoxW(hwnd, L"60秒倒计时结束！", L"提醒", MB_OK | MB_ICONINFORMATION);
            }
            else {
                WCHAR title[256];
                swprintf_s(title, sizeof(title) / sizeof(WCHAR), L"可恶的好朋友的记事本:P - 倒计时: %d秒", secondsLeft);
                SetWindowTextW(hwnd, title);
            }
        }
        return 0;
    }

    case WM_CLOSE: {
        if (g_isDirty) {
            int result = MessageBoxW(hwnd, L"文档已修改，是否保存？", L"确认", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (result == IDYES) {
                DoSaveRtf(hwnd);
            }
            else if (result == IDCANCEL) {
                return 0; // 取消关闭
            }
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY: {
        // 释放资源
        if (g_hFont) {
            DeleteObject(g_hFont);
            g_hFont = NULL;
        }
        if (g_hResFont) {
            RemoveFontMemResourceEx(g_hResFont);
            g_hResFont = NULL;
        }
        if (g_plainTextBuf) {
            free(g_plainTextBuf);
            g_plainTextBuf = NULL;
        }

        PostQuitMessage(0);
        return 0;
    }

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}
