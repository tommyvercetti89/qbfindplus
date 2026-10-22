#include "actions.h"

#include <shlobj.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace qb {

namespace {

bool isTextExtension(std::wstring_view ext) {
    if (!ext.empty() && ext.front() == L'.') ext.remove_prefix(1);
    const wchar_t* kTextExts[] = {
        L"txt", L"log", L"json", L"xml", L"md", L"markdown", L"go", L"cpp", L"c", L"h", L"hpp",
        L"cs", L"py", L"js", L"ts", L"jsx", L"tsx", L"html", L"htm", L"css", L"scss", L"ini",
        L"cfg", L"conf", L"yaml", L"yml", L"bat", L"cmd", L"ps1", L"sh", L"sql", L"csv", L"rs",
        L"java", L"kt", L"toml", L"env", L"diff", L"patch", L"reg", L"proto", L"cmake", L"mak"
    };
    for (auto* te : kTextExts) {
        if (_wcsicmp(std::wstring(ext).c_str(), te) == 0) return true;
    }
    return false;
}

std::wstring prettyPrintJson(const std::wstring& src) {
    std::wstring out;
    out.reserve(src.size() + 256);
    int indent = 0;
    bool inQuote = false;
    bool escape = false;

    auto appendIndent = [&](int lvl) {
        out += L"\r\n";
        for (int i = 0; i < lvl; ++i) out += L"  ";
    };

    for (size_t i = 0; i < src.size(); ++i) {
        wchar_t c = src[i];
        if (escape) {
            out += c;
            escape = false;
            continue;
        }
        if (c == L'\\' && inQuote) {
            out += c;
            escape = true;
            continue;
        }
        if (c == L'"') {
            inQuote = !inQuote;
            out += c;
            continue;
        }
        if (inQuote) {
            out += c;
            continue;
        }

        // Whitespace outside quotes
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;

        if (c == L'{' || c == L'[') {
            out += c;
            ++indent;
            appendIndent(indent);
        } else if (c == L'}' || c == L']') {
            if (indent > 0) --indent;
            appendIndent(indent);
            out += c;
        } else if (c == L',') {
            out += c;
            appendIndent(indent);
        } else if (c == L':') {
            out += L": ";
        } else {
            out += c;
        }
    }
    return out;
}

bool decodeText(const std::vector<char>& bytes, std::wstring& out) {
    if (bytes.empty()) {
        out.clear();
        return true;
    }

    // UTF-8 BOM: EF BB BF
    if (bytes.size() >= 3 && (uint8_t)bytes[0] == 0xEF && (uint8_t)bytes[1] == 0xBB && (uint8_t)bytes[2] == 0xBF) {
        int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + 3, (int)bytes.size() - 3, nullptr, 0);
        if (n >= 0) {
            out.resize(n);
            MultiByteToWideChar(CP_UTF8, 0, bytes.data() + 3, (int)bytes.size() - 3, out.data(), n);
            return true;
        }
    }

    // UTF-16LE BOM: FF FE
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xFE) {
        size_t count = (bytes.size() - 2) / sizeof(wchar_t);
        out.resize(count);
        std::memcpy(out.data(), bytes.data() + 2, count * sizeof(wchar_t));
        return true;
    }

    // UTF-16BE BOM: FE FF
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFE && (uint8_t)bytes[1] == 0xFF) {
        size_t count = (bytes.size() - 2) / 2;
        out.resize(count);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.data() + 2);
        for (size_t i = 0; i < count; ++i) {
            out[i] = static_cast<wchar_t>((p[i * 2] << 8) | p[i * 2 + 1]);
        }
        return true;
    }

    // Check for null bytes in initial sample (indicates binary)
    size_t sample = std::min<size_t>(bytes.size(), 512);
    for (size_t i = 0; i < sample; ++i) {
        if (bytes[i] == 0) return false; // binary file
    }

    // Try UTF-8 with strict error validation
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (n > 0) {
        out.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), out.data(), n);
        return true;
    }

    // Fallback: ANSI / system default code page
    n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (n > 0) {
        out.resize(n);
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), out.data(), n);
        return true;
    }

    return false;
}

// Custom preview dialog procedure
struct PreviewDlgData {
    std::wstring title;
    std::wstring text;
    HFONT font = nullptr;
    HWND edit = nullptr;
};

LRESULT CALLBACK PreviewWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<PreviewDlgData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            d = static_cast<PreviewDlgData*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
            SetWindowTextW(h, d->title.c_str());

            RECT rc;
            GetClientRect(h, &rc);
            int btnH = 28, pad = 8;
            d->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", d->text.c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                      pad, pad, rc.right - 2 * pad, rc.bottom - btnH - 3 * pad,
                                      h, nullptr, cs->hInstance, nullptr);
            HWND btnOk = CreateWindowExW(0, L"BUTTON", L"OK",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                         rc.right - pad - 80, rc.bottom - pad - btnH, 80, btnH,
                                         h, reinterpret_cast<HMENU>(IDOK), cs->hInstance, nullptr);
            if (d->font) {
                SendMessageW(d->edit, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
                SendMessageW(btnOk, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
            }
            SetFocus(d->edit);
            return 0;
        }
        case WM_SIZE: {
            if (d && d->edit) {
                int wndW = LOWORD(l), wndH = HIWORD(l);
                int btnH = 28, pad = 8;
                MoveWindow(d->edit, pad, pad, wndW - 2 * pad, std::max(40, wndH - btnH - 3 * pad), TRUE);
                HWND btn = GetDlgItem(h, IDOK);
                if (btn) MoveWindow(btn, wndW - pad - 80, wndH - pad - btnH, 80, btnH, TRUE);
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
                DestroyWindow(h);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool previewFile(HWND parent, const std::wstring& path, const std::wstring& name, HFONT font) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    int64_t size = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (size > 256 * 1024) return false;

    size_t extDot = name.rfind(L'.');
    std::wstring ext = (extDot == std::wstring::npos) ? L"" : name.substr(extDot);
    if (!isTextExtension(ext) && size > 64 * 1024) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::vector<char> bytes(static_cast<size_t>(size));
    if (size > 0 && std::fread(bytes.data(), 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    std::wstring text;
    if (!decodeText(bytes, text)) return false;

    if (_wcsicmp(ext.c_str(), L".json") == 0) {
        text = prettyPrintJson(text);
    }

    constexpr size_t kMaxRunes = 4000;
    if (text.size() > kMaxRunes) {
        text = text.substr(0, kMaxRunes) + L"\r\n\r\n... [Truncated / Kısaltıldı]";
    }

    // Convert Unix newlines \n to Windows \r\n for standard Edit control
    std::wstring winText;
    winText.reserve(text.size() + 256);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
            winText += L"\r\n";
        } else {
            winText += text[i];
        }
    }

    static bool registered = false;
    static HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = PreviewWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"QBFindTextPreview";
        RegisterClassW(&wc);
        registered = true;
    }

    PreviewDlgData data;
    data.title = L"Preview - " + name;
    data.text = std::move(winText);
    data.font = font;

    RECT pr;
    GetWindowRect(parent, &pr);
    int w = 680, h = 480;
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"QBFindTextPreview", data.title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE,
                               x, y, w, h, parent, nullptr, hInst, &data);
    if (!dlg) return false;

    EnableWindow(parent, FALSE);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return true;
}

static std::vector<wchar_t> makeDoubleNull(const std::vector<std::wstring>& list) {
    std::vector<wchar_t> buf;
    for (const auto& s : list) {
        buf.insert(buf.end(), s.begin(), s.end());
        buf.push_back(L'\0');
    }
    buf.push_back(L'\0');
    return buf;
}

bool copyFilesToDesktop(HWND parent, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    PWSTR pDesktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &pDesktop)) || !pDesktop) {
        return false;
    }
    std::wstring dest(pDesktop);
    CoTaskMemFree(pDesktop);

    auto fromBuf = makeDoubleNull(paths);
    auto toBuf = makeDoubleNull({dest});

    SHFILEOPSTRUCTW op = {};
    op.hwnd = parent;
    op.wFunc = FO_COPY;
    op.pFrom = fromBuf.data();
    op.pTo = toBuf.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION;

    int ret = SHFileOperationW(&op);
    return (ret == 0 && !op.fAnyOperationsAborted);
}

bool putFilesOnClipboard(HWND parent, const std::vector<std::wstring>& paths, bool isCut) {
    if (paths.empty()) return false;
    for (int retry = 0; retry < 10; ++retry) {
        if (OpenClipboard(parent)) {
            EmptyClipboard();

            size_t totalChars = 0;
            for (const auto& p : paths) totalChars += p.size() + 1;
            totalChars += 1;

            size_t bytes = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GHND | GMEM_SHARE, bytes);
            if (hMem) {
                auto* pData = static_cast<char*>(GlobalLock(hMem));
                if (pData) {
                    auto* df = reinterpret_cast<DROPFILES*>(pData);
                    df->pFiles = sizeof(DROPFILES);
                    df->fWide = TRUE;
                    wchar_t* pDest = reinterpret_cast<wchar_t*>(pData + sizeof(DROPFILES));
                    for (const auto& p : paths) {
                        std::memcpy(pDest, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
                        pDest += p.size() + 1;
                    }
                    *pDest = L'\0';
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_HDROP, hMem);
                }
            }

            if (isCut) {
                UINT cf = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
                HGLOBAL hDropEffect = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DWORD));
                if (hDropEffect) {
                    auto* pEffect = static_cast<DWORD*>(GlobalLock(hDropEffect));
                    if (pEffect) {
                        *pEffect = DROPEFFECT_MOVE;
                        GlobalUnlock(hDropEffect);
                        SetClipboardData(cf, hDropEffect);
                    }
                }
            }

            CloseClipboard();
            return true;
        }
        Sleep(20);
    }
    return false;
}

bool deleteFilesToRecycleBin(HWND parent, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    auto fromBuf = makeDoubleNull(paths);

    SHFILEOPSTRUCTW op = {};
    op.hwnd = parent;
    op.wFunc = FO_DELETE;
    op.pFrom = fromBuf.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;

    int ret = SHFileOperationW(&op);
    return (ret == 0 && !op.fAnyOperationsAborted);
}

void showFileProperties(HWND parent, const std::wstring& path) {
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.hwnd = parent;
    sei.lpVerb = L"properties";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

void showFileDetailsDialog(HWND parent, const Entry& e, bool isTurkish) {
    std::wstring msg;
    if (isTurkish) {
        msg = (e.isDir ? L"Klasör Bilgileri\n\n" : L"Dosya Bilgileri\n\n");
        msg += L"Ad: " + std::wstring(e.name()) + L"\n";
        msg += L"Konum: " + e.path + L"\n";
        msg += L"Boyut: " + (e.isDir ? L"<klasör>" : formatSize(e.size)) + L"\n";
        msg += L"Değiştirilme: " + formatTime(e.mtime);
    } else {
        msg = (e.isDir ? L"Folder Information\n\n" : L"File Information\n\n");
        msg += L"Name: " + std::wstring(e.name()) + L"\n";
        msg += L"Location: " + e.path + L"\n";
        msg += L"Size: " + (e.isDir ? L"<folder>" : formatSize(e.size)) + L"\n";
        msg += L"Modified: " + formatTime(e.mtime);
    }
    MessageBoxW(parent, msg.c_str(), isTurkish ? L"Özellikler" : L"Properties", MB_ICONINFORMATION | MB_OK);
}

}  // namespace qb
