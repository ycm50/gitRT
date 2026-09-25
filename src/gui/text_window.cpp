// 文本窗口：差异 / 提交历史 / 自检结果（《技术实现设计》§9.4）
#include "gui.h"

#include <algorithm>
#include <cstring>

namespace grt::gui {

const wchar_t* kTextWindowClass = L"GitRT.TextWindow";

enum : int { IDC_TXT_BODY = 200, IDC_TXT_COPY, IDC_TXT_CLOSE };

namespace {

struct TextState {
    std::wstring subtitle;
    std::wstring body;
    HWND sub = nullptr, bodyEdit = nullptr, copyBtn = nullptr, closeBtn = nullptr;
};

LRESULT CALLBACK TextProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<TextState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto* s = new TextState();
            if (cs->lpCreateParams) *s = *reinterpret_cast<TextState*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            return TRUE;
        }
        case WM_CREATE: {
            st->sub = MakeChild(hwnd, WC_STATICW, st->subtitle, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                0, 0, Th().fontSmall);
            st->bodyEdit = MakeChild(hwnd, WC_EDITW, ToCrlf(st->body),
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                         ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_BORDER,
                                     WS_EX_CLIENTEDGE, IDC_TXT_BODY, Th().fontMono);
            st->copyBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_COPY),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_TXT_COPY,
                                    Th().fontUi);
            st->closeBtn = MakeChild(hwnd, WC_BUTTONW, Str(IDS_BTN_CLOSE),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDC_TXT_CLOSE,
                                     Th().fontUi);
            ThemeApply(hwnd);
            return 0;
        }
        case WM_SIZE: {
            if (!st) break;
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            const int pad = Scale(10), btnH = Scale(30), btnW = Scale(110);
            const int subH = Scale(22);
            ::MoveWindow(st->sub, pad, pad / 2, rc.right - pad * 2, subH, TRUE);
            const int bodyTop = pad / 2 + subH + Scale(4);
            const int bodyBottom = rc.bottom - pad - btnH - Scale(8);
            ::MoveWindow(st->bodyEdit, pad, bodyTop, rc.right - pad * 2,
                         (std::max)(Scale(60), bodyBottom - bodyTop), TRUE);
            const int by = rc.bottom - pad - btnH;
            ::MoveWindow(st->copyBtn, rc.right - pad * 2 - btnW * 2 - Scale(8), by, btnW, btnH, TRUE);
            ::MoveWindow(st->closeBtn, rc.right - pad - btnW, by, btnW, btnH, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_TXT_CLOSE:
                case IDCANCEL:
                    ::DestroyWindow(hwnd);
                    return 0;
                case IDC_TXT_COPY:
                    CopyTextToClipboard(hwnd, GetText(st->bodyEdit));
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            LRESULT res = 0;
            if (HandleCtlColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), &res))
                return res;
            break;
        }
        case WM_ERASEBKGND: {
            if (Th().dark) {
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                ::FillRect(reinterpret_cast<HDC>(wp), &rc, Th().bgBrush);
                return 1;
            }
            break;
        }
        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            delete st;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureTextClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = TextProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = GitRTAppIcon();
    wc.hIconSm = GitRTAppIcon();
    wc.lpszClassName = kTextWindowClass;
    ::RegisterClassExW(&wc);
    done = true;
}

}  // namespace

void ShowTextWindow(HWND owner, UINT titleRes, const std::wstring& subtitle, const std::wstring& body) {
    EnsureTextClass();
    TextState init;
    init.subtitle = subtitle;
    init.body = body;
    HWND h = ::CreateWindowExW(WS_EX_CONTROLPARENT, kTextWindowClass, Str(titleRes).c_str(),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                               Scale(900), Scale(620), owner, nullptr, ::GetModuleHandleW(nullptr), &init);
    if (!h) return;
    CenterOnOwner(h, owner);
    ::ShowWindow(h, SW_SHOW);
    ::UpdateWindow(h);
}

}  // namespace grt::gui
