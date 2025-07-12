// simple_notepad.c
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <richedit.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#include "resource.h"
#include "webview_host.h" // ���ڰ������� C ���ݵ�����

// <WebView2.h> �����Ϊ g_controller ���ʶ����������ڿ����Ƴ���
// ������ļ���������ֱ��ʹ���� WebView2 ��ص��������ͣ�����Ա�����
// Ϊ�˽����ǰ�ı������⣬ȷ������û�� ComPtr ��ֱ��ʹ�á�
// �����ȷ����������ʱ��������Ϊ webview_host.h �Ѿ����ٱ�¶ ComPtr��
#include <WebView2.h> 

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

#ifdef __cplusplus
extern "C" {
#endif
    // ������ webview_host.cpp ��ʵ��
    void InitWebView2Host(HWND hwnd);
    void WebViewPostCommand(const wchar_t* command, const wchar_t* payload);
    void WebViewSetVisibility(BOOL bVisible); // ��������
    void WebViewSetBounds(int x, int y, int width, int height); // ��������



#ifdef __cplusplus
}
#endif


// RTF ����/����ص����ֲ���
typedef struct {
    FILE* fp;              // ԭ�������ļ���
    const BYTE* memPtr;    // �ڴ�����ָ��
    size_t memSize;        // �����ܴ�С
    size_t memPos;         // ��ǰ��ȡλ��
} STREAM_COOKIE;

// �����ڴ����� RTF �Ľṹ�ͻص�
typedef struct {
    BYTE* buf;      // ������ָ��
    size_t size;    // ����������
    size_t pos;     // ��ǰд��λ��
} MEM_COOKIE;

static DWORD CALLBACK MemStreamOutCallback(DWORD_PTR dwCookie,
    LPBYTE pBuff, LONG cb, LONG* pcb)
{
    MEM_COOKIE* mc = (MEM_COOKIE*)dwCookie;
    // ��̬����
    if (mc->pos + cb > mc->size) {
        size_t newSize = (mc->size + cb) * 2;
        if (newSize < mc->size) return 1; // ������
        BYTE* newBuf = (BYTE*)realloc(mc->buf, newSize);
        if (!newBuf) return 1; // realloc ʧ�ܣ����ط����ʾ����
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
    return 1; // ����
}

static HINSTANCE g_hInst;
static BOOL g_isDirty = FALSE;
static HFONT g_hFont = NULL; // ����Ϊ��̬�������Ա��� WM_DESTROY ���ͷ�
static HANDLE g_hResFont = NULL; // ����Ϊ��̬�������Ա��� WM_DESTROY ���ͷ�

// ȫ�ֱ������ڴ洢�� RichEdit ��ȡ�Ĵ��ı�
static WCHAR* g_plainTextBuf = NULL; // ���ڴ洢���ı��Ļ�����
static int g_plainTextLen = 0;       // ���ı��ĳ��ȣ������� null ��ֹ����


#define ID_EDIT             1001
#define ID_FILE_EXIT        2001
#define ID_EDIT_HIDE        2002
#define ID_FILE_SAVE_RTF    2003    // �� RTF ��ʽ����
#define ID_FILE_OPEN_RTF    2004    // �� RTF ��ʽ��
#define ID_TOGGLE_HIDDEN    2005    // �л���ʾ��������
#define ID_EDIT_PASTE_RTF   2006
#define ID_EDIT_COPY_BOTH   2007
#define ID_EDIT_UNDO        2008
#define ID_VIEW_VERTICAL    3001
#define ID_VIEW_HORIZONTAL  3002
#define ID_START_TIMER      4001
#define ID_TIMER            4002

#define EM_BEGINUNDO    (WM_USER+217)
#define EM_ENDUNDO      (WM_USER+218)


// �����ݼ�ӳ�䣺Ctrl+C �� ����(RTF+�ı�)��Ctrl+V �� ճ����Ctrl+H �� ����ѡ�У�Ctrl+Shift+H �� �л���ʾ
ACCEL g_accelTable[] = {
    { FVIRTKEY | FCONTROL,      'C',  ID_EDIT_COPY_BOTH  },
    { FVIRTKEY | FCONTROL,      'V',  ID_EDIT_PASTE_RTF  },
    { FVIRTKEY | FCONTROL,      'H',  ID_EDIT_HIDE       },
    { FVIRTKEY | FCONTROL,      'Z',  ID_EDIT_UNDO       },
    { FVIRTKEY | FCONTROL | FSHIFT,'H',  ID_TOGGLE_HIDDEN   },
};
const int g_accelCount = sizeof(g_accelTable) / sizeof(g_accelTable[0]);



// �������벢�Ż�������������û����Ϊ�����ء�������
#define MAX_HIDDEN_RANGES 1024 // ����������Ӧ�Ը�����������
static CHARRANGE g_hiddenRanges[MAX_HIDDEN_RANGES];
static int         g_hiddenCount = 0;

// һ����־����ǰ���ڡ��鿴���ء�ģʽ�����ǡ�������ͼ��ģʽ
static BOOL        g_showHidden = FALSE;

// ȫ�� Rich Edit �ؼ����
static HWND g_hEdit;


// ɨ�������ĵ��������� CFE_HIDDEN �����μ�¼�� g_hiddenRanges
// ����������ı����ݱ仯ʱ�����ã��Ը������������б�
void ScanHiddenRanges() {
    g_hiddenCount = 0;
    int len = GetWindowTextLengthW(g_hEdit);

    // ���浱ǰѡ�����Ա���ɨ���ָ�
    CHARRANGE crOriginal;
    SendMessage(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&crOriginal);

    for (int i = 0; i < len; ++i) {
        // ѡ�� [i, i+1)
        CHARRANGE cr = { i, i + 1 };
        SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);

        // ȡ����ַ��ĸ�ʽ
        CHARFORMAT2 cf = { sizeof(cf) };
        cf.cbSize = sizeof(cf); // �������� cbSize
        cf.dwMask = CFM_HIDDEN; // ֻ��ȡ��������
        SendMessage(g_hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

        if (cf.dwEffects & CFE_HIDDEN) {
            // ����������㣬��������ҵ�ĩβ
            int start = i;
            while (i + 1 < len) {
                CHARRANGE cr2 = { i + 1, i + 2 };
                SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr2);
                CHARFORMAT2 cf2 = { sizeof(cf2) };
                cf2.cbSize = sizeof(cf2);
                cf2.dwMask = CFM_HIDDEN;
                SendMessage(g_hEdit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf2);
                if (cf2.dwEffects & CFE_HIDDEN) {
                    ++i;  // ������������
                }
                else {
                    break;
                }
            }
            // ��¼������������ [start, i+1)
            if (g_hiddenCount < MAX_HIDDEN_RANGES) {
                g_hiddenRanges[g_hiddenCount++] = (CHARRANGE){ start, i + 1 };
            }
            else {
                // �������ط�Χ���ƣ���������
                MessageBoxW(NULL, L"�������������������ƣ����������������δ����ȷ���١�", L"����", MB_OK | MB_ICONWARNING);
                break; // ֹͣɨ��
            }
        }
    }
    // �ָ�ԭ����ѡ��
    SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&crOriginal);
}

// ���� RTF �ĺ���
void DoSaveRtf(HWND hwnd) {
    OPENFILENAMEW ofn = { 0 };
    WCHAR szFile[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Rich Text ��ʽ\0*.rtf\0�����ļ�\0*.*\0\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;  // �û�ȡ��

    FILE* fp = _wfopen(szFile, L"wb");
    if (!fp) {
        MessageBoxW(hwnd, L"�޷������ļ���", L"����ʧ��", MB_OK | MB_ICONERROR);
        return;
    }

    STREAM_COOKIE cookie = { 0 };
    cookie.fp = fp;

    EDITSTREAM es = { 0 };
    es.dwCookie = (DWORD_PTR)&cookie;
    es.pfnCallback = EditStreamOutCallback;

    // ע����� SF_UNICODE
    LRESULT res = SendMessage(g_hEdit,
        EM_STREAMOUT,
        SF_RTF | SF_UNICODE,
        (LPARAM)&es);
    fclose(fp);

    if (res < 0) {
        MessageBoxW(hwnd, L"����RTFʧ�ܣ��ļ����ܲ���д����̴���",
            L"�������", MB_OK | MB_ICONERROR);
        return;
    }

    // �ɹ���������־
    g_isDirty = FALSE;
}

// �������� RichEdit �ؼ���ȡ���ı�
// hEdit: RichEdit �ؼ��ľ��
// ppBuf: ָ��洢���ı���������ָ���ָ��
// pLen: ָ��洢���ı����ȵ�ָ��
void DoGetPlainTextFromEdit(HWND hEdit, WCHAR** ppBuf, int* pLen) {
    // ��ȡ�ı��ĳ��ȣ����� null ��ֹ����
    int textLength = GetWindowTextLengthW(hEdit);

    // ���֮ǰ�з���Ļ����������ͷŵ�
    if (*ppBuf != NULL) {
        free(*ppBuf);
        *ppBuf = NULL;
    }

    if (textLength > 0) {
        // �����㹻���ڴ����洢�ı� + null ��ֹ��
        *ppBuf = (WCHAR*)malloc((textLength + 1) * sizeof(WCHAR));
        if (*ppBuf == NULL) {
            *pLen = 0;
            // �����ڴ����ʧ�ܵ������������ʾ������Ϣ
            MessageBoxW(NULL, L"�ڴ����ʧ�ܣ�", L"����", MB_OK | MB_ICONERROR);
            return;
        }
        // ��ȡ�ı�����
        GetWindowTextW(hEdit, *ppBuf, textLength + 1);
        *pLen = textLength; // ��¼ʵ���ı����ȣ������� null ��ֹ����
    }
    else {
        *pLen = 0;
        *ppBuf = NULL; // ���û���ı���ȷ��������ָ��Ϊ NULL
    }
}

// �º������� RichEdit ��ȡ���ݣ����� CFE_HIDDEN ��ʽ���ı��� [HIDDEN]...[/HIDDEN] ����
// ����һ���·���� WCHAR* �������������߱���ʹ�� free() �ͷ�����
WCHAR* GetContentWithHiddenMarkersFromRichEdit(HWND hEdit, int* pLen) {
    int len = GetWindowTextLengthW(hEdit);
    if (len == 0) {
        *pLen = 0;
        return NULL;
    }

    // ��ʼ��������С�������Ҫ�����·���
    // ����ÿ�������ı������� [HIDDEN] �� [/HIDDEN] ��ǩ�ĳ��� (8+9=17�ַ�)
    // ������� MAX_HIDDEN_RANGES �������������ӵĶ���ռ� = MAX_HIDDEN_RANGES * 17
    size_t initialBufSize = (len + 1 + MAX_HIDDEN_RANGES * 17) * sizeof(WCHAR);
    WCHAR* buffer = (WCHAR*)malloc(initialBufSize);
    if (!buffer) {
        *pLen = 0;
        return NULL;
    }
    buffer[0] = L'\0'; // ȷ���� null ��β

    // ���浱ǰѡ��
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
            // ׷�� [HIDDEN] ��ǩ
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
            // ׷�� [/HIDDEN] ��ǩ
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

    // �ָ�ԭ����ѡ��
    SendMessage(hEdit, EM_EXSETSEL, 0, (LPARAM)&crOriginal);

    *pLen = currentOutputLen;
    // ���������ü���׼ȷ�Ĵ�С + null ��ֹ��
    WCHAR* finalBuffer = (WCHAR*)realloc(buffer, (*pLen + 1) * sizeof(WCHAR));
    if (finalBuffer) {
        return finalBuffer;
    }
    else {
        free(buffer); // realloc ʧ��ʱ�ͷžɵĻ�����
        return NULL;
    }
}

// �º��������� RichEdit ���ݣ������� [HIDDEN] ���Ӧ�� CFE_HIDDEN ��ʽ
void SetRichEditContentWithHiddenMarkers(HWND hEdit, const wchar_t* content) {
    if (!hEdit || !content) {
        SendMessage(hEdit, EM_SETTEXTEX, (WPARAM)SCF_ALL, (LPARAM)NULL); // �����������
        return;
    }

    SendMessage(hEdit, EM_SETTEXTEX, (WPARAM)SCF_ALL, (LPARAM)NULL); // ����������ݺ͸�ʽ

    const WCHAR* pCurrent = content;

    // ���浱ǰѡ���Ա��ָ�
    CHARRANGE crOriginal;
    SendMessage(hEdit, EM_EXGETSEL, 0, (LPARAM)&crOriginal);

    // ���ó������Է�ֹ�м�״̬����¼
    SendMessage(hEdit, EM_BEGINUNDO, 0, 0);

    const WCHAR* HIDDEN_START_TAG = L"[HIDDEN]";
    const WCHAR* HIDDEN_END_TAG = L"[/HIDDEN]";
    size_t startTagLen = wcslen(HIDDEN_START_TAG);
    size_t endTagLen = wcslen(HIDDEN_END_TAG);

    while (*pCurrent != L'\0') {
        const WCHAR* tagStart = wcsstr(pCurrent, HIDDEN_START_TAG);

        if (tagStart == NULL) {
            // û�и������ر�ǩ��׷��ʣ������
            if (*pCurrent != L'\0') {
                SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)pCurrent);
            }
            break;
        }
        else {
            // ׷�� [HIDDEN] ��ǩ֮ǰ����ͨ�ı�
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

            // ���Ҷ�Ӧ�� [/HIDDEN] ��ǩ
            const WCHAR* hiddenContentStart = tagStart + startTagLen;
            const WCHAR* tagEnd = wcsstr(hiddenContentStart, HIDDEN_END_TAG);

            if (tagEnd != NULL) {
                // ��ȡ�����ı�
                size_t hiddenTextLen = tagEnd - hiddenContentStart;
                WCHAR* hiddenText = (WCHAR*)malloc((hiddenTextLen + 1) * sizeof(WCHAR));
                if (hiddenText) {
                    wcsncpy_s(hiddenText, hiddenTextLen + 1, hiddenContentStart, hiddenTextLen);
                    hiddenText[hiddenTextLen] = L'\0';

                    long currentRichEditLen = GetWindowTextLengthW(hEdit);

                    // ���������ı�
                    SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)hiddenText);

                    // ���²�����ı�Ӧ�����ظ�ʽ
                    CHARRANGE crApply = { currentRichEditLen, currentRichEditLen + (long)hiddenTextLen };
                    SendMessage(hEdit, EM_EXSETSEL, 0, (LPARAM)&crApply);

                    CHARFORMAT2 cf = { sizeof(cf) };
                    cf.cbSize = sizeof(cf);
                    cf.dwMask = CFM_HIDDEN;
                    cf.dwEffects = CFE_HIDDEN; // �������ر�־
                    SendMessage(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

                    free(hiddenText);
                }
                // �ƶ� pCurrent ���رձ�ǩ֮��
                pCurrent = tagEnd + endTagLen;
            }
            else {
                // ��ʽ����[HIDDEN] û�� [/HIDDEN]����ʣ�ಿ����Ϊ��ͨ�ı�
                SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)tagStart);
                pCurrent = tagStart + wcslen(tagStart); // �ƶ���ĩβ
            }
        }
    }

    SendMessage(hEdit, EM_ENDUNDO, 0, 0); // ����������
    SendMessage(hEdit, EM_EXSETSEL, 0, (LPARAM)&crOriginal); // �ָ�ѡ��
    ScanHiddenRanges(); // �������ݺ�����ɨ����������
}


// *** ������ģʽ�л����� ***
static BOOL g_isWebViewMode = FALSE;
static BOOL g_webViewInitialized = FALSE;

void SwitchToWebViewMode(HWND hwnd) {
    if (!g_isWebViewMode) {
        // 1. ��ȡRichEdit�е����ݣ���ת�����ر��
        if (g_plainTextBuf) {
            free(g_plainTextBuf);
            g_plainTextBuf = NULL;
            g_plainTextLen = 0;
        }
        g_plainTextBuf = GetContentWithHiddenMarkersFromRichEdit(g_hEdit, &g_plainTextLen);

        // 2. ����RichEdit
        ShowWindow(g_hEdit, SW_HIDE);

        // 3. ��ʾWebView����������
        WebViewSetVisibility(TRUE);

        if (g_plainTextBuf) { // ��������Ƿ����
            WebViewSetContent(g_plainTextBuf);
        }
        else {
            WebViewSetContent(L""); // ��� RichEdit Ϊ�գ������ WebView
        }

        g_isWebViewMode = TRUE;

        // 4. ���´��ڱ���
        SetWindowTextW(hwnd, L"�ɶ�ĺ����ѵļ��±�:P - ����ģʽ");
    }
}

void SwitchToRichEditMode(HWND hwnd) {
    if (g_isWebViewMode) {
        // 1. ����WebView���͵�ǰ���� (��ͨ�� OnWebViewMessageReceived �첽����)
        WebViewPostCommand(L"getContent", L"");

        // 2. ����WebView
        WebViewSetVisibility(FALSE);

        // 3. ��ʾRichEdit
        // ���ݽ��� OnWebViewMessageReceived �첽���á�
        // ����ֻ����ʾ����
        ShowWindow(g_hEdit, SW_SHOW);
        SetFocus(g_hEdit);

        g_isWebViewMode = FALSE;

        // 4. ���´��ڱ���
        SetWindowTextW(hwnd, L"�ɶ�ĺ����ѵļ��±�:P - ����ģʽ");
    }
}



LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// -----------------------------------------
// WebView ���ûص����� Web �༭�������ݲ��� RichEdit �ؼ�
// -----------------------------------------
// OnWebViewMessageReceived ���������ֲ��䣬ȷ���� extern "C" ���ڣ�
// *** ��ǿ��WebView��Ϣ���� ***
#ifdef __cplusplus
extern "C" {
#endif
    void OnWebViewMessageReceived(const wchar_t* text) {
        if (!text) return;

        // �򵥵�JSON���� - ��ʵ��Ӧ�����������Ҫ��robust�Ľ���
        if (wcsstr(text, L"\"type\":\"contentResponse\"")) {
            // ���Ǵ�WebView��ȡ��������Ӧ
            // ��ȡcontent�ֶε�ֵ
            const wchar_t* contentStart = wcsstr(text, L"\"content\":\"");
            if (contentStart) {
                contentStart += 11; // ���� "content":"
                const wchar_t* contentEnd = wcsstr(contentStart, L"\"");
                if (contentEnd) {
                    size_t len = contentEnd - contentStart;
                    if (len > 0) {
                        wchar_t* content = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
                        if (content) {
                            wcsncpy_s(content, len + 1, contentStart, len);
                            content[len] = L'\0';

                            // ����RichEdit���ݣ������ǰ����WebViewģʽ��
                            // ��ʹ��ǰ��WebViewģʽ��Ҳ���ղ��������ݣ�Ϊ�´��л���׼��
                            SetRichEditContentWithHiddenMarkers(g_hEdit, content);


                            free(content);
                        }
                    }
                }
            }
        }
        else if (wcsstr(text, L"\"type\":\"contentChanged\"")) {
            // ���ݷ����仯
            g_isDirty = TRUE;
        }
        else if (wcsstr(text, L"\"type\":\"ready\"")) {
            // WebView�Ѿ���
            g_webViewInitialized = TRUE;
        }
        else if (wcsstr(text, L"\"type\":\"clipboardContentResponse\"")) { // �����ļ�����������Ӧ
            const wchar_t* contentStart = wcsstr(text, L"\"content\":\"");
            if (contentStart) {
                contentStart += 11;
                const wchar_t* contentEnd = wcsstr(contentStart, L"\"");
                if (contentEnd) {
                    size_t len = contentEnd - contentStart;
                    if (len > 0) {
                        // ���䲢��������
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(WCHAR));
                        if (hMem) {
                            WCHAR* pBuf = (WCHAR*)GlobalLock(hMem);
                            if (pBuf) {
                                wcsncpy_s(pBuf, len + 1, contentStart, len);
                                pBuf[len] = L'\0';
                                GlobalUnlock(hMem);

                                if (OpenClipboard(NULL)) { // ʹ�� NULL ��Ϊ�����ߴ����Ա�������
                                    EmptyClipboard();
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                    CloseClipboard();
                                }
                                else {
                                    GlobalFree(hMem); // ����޷��򿪼����壬���ͷ�
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

// ������ע�ᴰ����
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmdLine, int nCmdShow) {
    // ȷ�� Msftedit.dll �����أ����� Rich Edit Control 2.0 (or higher)
    // ֻ�� LoadLibraryW(L"Msftedit.dll"); ���ܻ�����°汾�� Rich Edit �ؼ�
    LoadLibraryW(L"Msftedit.dll");
    g_hInst = hInst;
    // ��ʼ�������ؼ���Ϊ�˼�����ϵͳ��
    InitCommonControls();

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SimpleNotepadClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClass(&wc)) {
        MessageBox(NULL, L"ע�ᴰ����ʧ��", L"����", MB_ICONERROR);
        return 0;
    }

    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"�ɶ�ĺ����ѵļ��±�:P",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInst, NULL
    );

    if (!hwnd) {
        MessageBox(NULL, L"��������ʧ��", L"����", MB_ICONERROR);
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    HACCEL hAccel = CreateAcceleratorTable(g_accelTable, g_accelCount);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        // �����ü��ٱ�������Ϣ
        if (!TranslateAccelerator(hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // Ӧ�ó����˳�ʱ�����ټ��ٱ�
    if (hAccel) DestroyAcceleratorTable(hAccel);

    return (int)msg.wParam;
}

// ���ڹ��̺�����������Ϣ
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    static int  secondsLeft = 0;

    switch (msg) {
    case WM_CREATE: {
        // �����˵�
        HMENU hMenu = CreateMenu();
        HMENU hFile = CreatePopupMenu();
        AppendMenu(hFile, MF_STRING, ID_FILE_OPEN_RTF, L"&��\tCtrl+O");
        AppendMenu(hFile, MF_STRING, ID_FILE_SAVE_RTF, L"&����\tCtrl+S");
        AppendMenu(hFile, MF_SEPARATOR, 0, NULL);
        AppendMenu(hFile, MF_STRING, ID_FILE_EXIT, L"�˳�\tAlt+F4");
        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hFile, L"&�ļ�");
        HMENU hEditMenu = CreatePopupMenu();
        AppendMenuW(hEditMenu, MF_STRING, ID_EDIT_HIDE, L"����ѡ������\tCtrl+H");
        AppendMenuW(hEditMenu, MF_STRING, ID_EDIT_PASTE_RTF, L"ճ����֧��RTF/�ı���\tCtrl+V");
        AppendMenuW(hEditMenu, MF_STRING, ID_EDIT_COPY_BOTH, L"���ƣ�RTF + �ı���\tCtrl+C");
        AppendMenuW(hEditMenu, MF_STRING, ID_EDIT_UNDO, L"����\tCtrl+Z");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hEditMenu, L"&�༭");
        HMENU hView = CreatePopupMenu();
        AppendMenuW(hView, MF_STRING, ID_TOGGLE_HIDDEN, L"�л��������ֿɼ�״̬\tCtrl+Shift+H");
        AppendMenuW(hView, MF_STRING, ID_VIEW_VERTICAL, L"�л�������ʾ"); // ����Ϊ����ʾģʽ
        AppendMenuW(hView, MF_STRING, ID_VIEW_HORIZONTAL, L"�л�������ʾ");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hView, L"&��ͼ");
        HMENU hFunctionMenu = CreatePopupMenu();
        AppendMenuW(hFunctionMenu, MF_STRING, ID_START_TIMER, L"��ʼ����ʱ 60s\tCtrl+T");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFunctionMenu, L"&���๦��");
        SetMenu(hwnd, hMenu);

        // 1) ���� RichEdit �ؼ������������ WS_HSCROLL �� ES_AUTOHSCROLL��
        g_hEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,      // ��չ��ʽ��3D �߿�
            MSFTEDIT_CLASS,        // ����
            L"",                   // ��ʼ�ı�
            WS_CHILD | WS_VISIBLE  // ������ʽ
            | WS_VSCROLL           // �������
            | ES_MULTILINE         // ֧�ֶ���
            | ES_AUTOVSCROLL       // �Զ��������
            | ES_WANTRETURN,       // �س���������
            0, 0, 0, 0,            // λ�����С���� WM_SIZE
            hwnd, (HMENU)ID_EDIT,  // ������ & �ؼ� ID
            g_hInst, NULL);        // ʵ����� & ����



        // 2) ����Դ��������
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
                            18,                    // ����߶�
                            0,                     // ��ȣ�0=�Զ���
                            0,                     // ������б�Ƕ�
                            0,                     // ������б�Ƕ�
                            FW_NORMAL,             // �����ϸ
                            FALSE,                 // �Ƿ�б��
                            FALSE,                 // �Ƿ��»���
                            FALSE,                 // �Ƿ�ɾ����
                            DEFAULT_CHARSET,       // �ַ���
                            OUT_DEFAULT_PRECIS,    // �������
                            CLIP_DEFAULT_PRECIS,   // ��������
                            DEFAULT_QUALITY,       // �������
                            DEFAULT_PITCH | FF_DONTCARE, // ���������
                            L"���ĸۺ�"           // ��������
                        );

                        if (g_hFont) {
                            SendMessage(g_hEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
                        }
                    }
                }
            }
        }

        // 3) ��ʼ��WebView2��������ʾ��
        // WebView2���ں�̨��ʼ������Ĭ�ϲ��ɼ�
        InitWebView2Host(hwnd);

        return 0;
    }

    case WM_SIZE: {
        // ֻ������ǰ�ɼ��Ŀؼ���С
        if (g_hEdit) {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);

            // ����RichEdit��С
            MoveWindow(g_hEdit, 0, 0, width, height, TRUE);

            // ͬʱ����WebView��С����ʹ���ɼ���
            WebViewSetBounds(0, 0, width, height);
        }
        return 0;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        case ID_FILE_OPEN_RTF: {
            // ��RTF�ļ�
            OPENFILENAMEW ofn = { 0 };
            WCHAR szFile[MAX_PATH] = L"";
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Rich Text ��ʽ\0*.rtf\0�����ļ�\0*.*\0\0";
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

                    // ����ɨ����������
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
            // ����ѡ�е��ı� (RichEdit specific)
            CHARRANGE cr;
            SendMessage(g_hEdit, EM_EXGETSEL, 0, (LPARAM)&cr);

            if (cr.cpMin != cr.cpMax) {
                CHARFORMAT2 cf = { sizeof(cf) };
                cf.dwMask = CFM_HIDDEN;
                cf.dwEffects = CFE_HIDDEN;

                SendMessage(g_hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
                ScanHiddenRanges(); // ����ɨ����������
                g_isDirty = TRUE;
            }
            break;
        }

        case ID_TOGGLE_HIDDEN: {
            if (g_isWebViewMode) {
                // �� WebView �����л������ı�������
                WebViewPostCommand(L"toggleHidden", L"");
            }
            else {
                // RichEdit ģʽ�µ��л�����������ʾ״̬
                g_showHidden = !g_showHidden;

                for (int i = 0; i < g_hiddenCount; ++i) {
                    SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&g_hiddenRanges[i]);

                    CHARFORMAT2 cf = { sizeof(cf) };
                    cf.dwMask = CFM_HIDDEN;
                    cf.dwEffects = g_showHidden ? 0 : CFE_HIDDEN;

                    SendMessage(g_hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
                }

                // �ָ�ѡ��
                CHARRANGE cr = { 0, 0 };
                SendMessage(g_hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);
            }
            break;
        }

        case ID_EDIT_COPY_BOTH: {
            if (g_isWebViewMode) {
                // WebView ģʽ�£����� WebView �������ݵ�������
                WebViewPostCommand(L"getClipboardContent", L"");
            }
            else {
                // RichEdit ģʽ�£�����RTF�ʹ��ı���������
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();

                    // ��ȡRTF��ʽ
                    MEM_COOKIE mc = { 0 };
                    mc.size = 1024;
                    mc.buf = (BYTE*)malloc(mc.size);

                    if (mc.buf) {
                        EDITSTREAM es = { 0 };
                        es.dwCookie = (DWORD_PTR)&mc;
                        es.pfnCallback = MemStreamOutCallback;

                        SendMessage(g_hEdit, EM_STREAMOUT, SF_RTF | SFF_SELECTION, (LPARAM)&es);

                        if (mc.pos > 0) {
                            // ע��RTF��ʽ
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

                    // ��ȡ���ı�
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
                // WebView ģʽ�£�ֻ֧��ճ�����ı�
                if (OpenClipboard(hwnd)) {
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (hData) {
                        WCHAR* pText = (WCHAR*)GlobalLock(hData);
                        if (pText) {
                            WebViewPostCommand(L"pasteContent", pText);
                            GlobalUnlock(hData);
                            g_isDirty = TRUE; // ���ݷ����仯
                        }
                    }
                    CloseClipboard();
                }
            }
            else {
                // RichEdit ģʽ�£�ճ��RTF���ı�
                if (OpenClipboard(hwnd)) {
                    UINT rtfFormat = RegisterClipboardFormatW(L"Rich Text Format");
                    HANDLE hData = GetClipboardData(rtfFormat);

                    if (hData) {
                        // ճ��RTF
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
                        // ճ�����ı�
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

                         // *** ������WebViewģʽ�л� ***
        case ID_VIEW_VERTICAL: {
            // �л���������ʾģʽ��WebView��
            SwitchToWebViewMode(hwnd);
            break;
        }

        case ID_VIEW_HORIZONTAL: {
            // �л���������ʾģʽ��RichEdit��
            SwitchToRichEditMode(hwnd);
            break;
        }

        case ID_START_TIMER: {
            // ��ʼ60�뵹��ʱ
            secondsLeft = 60;
            SetTimer(hwnd, ID_TIMER, 1000, NULL);

            WCHAR title[256];
            swprintf_s(title, sizeof(title) / sizeof(WCHAR), L"�ɶ�ĺ����ѵļ��±�:P - ����ʱ: %d��", secondsLeft);
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
                SetWindowTextW(hwnd, L"�ɶ�ĺ����ѵļ��±�:P - ʱ�䵽��");
                MessageBoxW(hwnd, L"60�뵹��ʱ������", L"����", MB_OK | MB_ICONINFORMATION);
            }
            else {
                WCHAR title[256];
                swprintf_s(title, sizeof(title) / sizeof(WCHAR), L"�ɶ�ĺ����ѵļ��±�:P - ����ʱ: %d��", secondsLeft);
                SetWindowTextW(hwnd, title);
            }
        }
        return 0;
    }

    case WM_CLOSE: {
        if (g_isDirty) {
            int result = MessageBoxW(hwnd, L"�ĵ����޸ģ��Ƿ񱣴棿", L"ȷ��", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (result == IDYES) {
                DoSaveRtf(hwnd);
            }
            else if (result == IDCANCEL) {
                return 0; // ȡ���ر�
            }
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY: {
        // �ͷ���Դ
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
