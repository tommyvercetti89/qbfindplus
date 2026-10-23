#include "actions.h"

#include <shlobj.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace qb {

namespace {

HFONT createModernFont(int pointSize = 9, int weight = FW_NORMAL, bool monospace = false) {
    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(nullptr, hdc);
    int height = -MulDiv(pointSize, dpi, 72);
    const wchar_t* face = monospace ? L"Consolas" : L"Segoe UI";
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, monospace ? (FIXED_PITCH | FF_MODERN) : (DEFAULT_PITCH | FF_SWISS),
                       face);
}

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
            size_t next = i + 1;
            while (next < src.size() && (src[next] == L' ' || src[next] == L'\t' || src[next] == L'\r' || src[next] == L'\n')) ++next;
            if (next < src.size() && ((c == L'{' && src[next] == L'}') || (c == L'[' && src[next] == L']'))) {
                out += src[next];
                i = next;
            } else {
                ++indent;
                appendIndent(indent);
            }
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

    HFONT monoFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    PreviewDlgData data;
    data.title = L"Preview - " + name;
    data.text = std::move(winText);
    data.font = monoFont ? monoFont : font;

    RECT pr;
    GetWindowRect(parent, &pr);
    int w = 680, h = 480;
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"QBFindTextPreview", data.title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE,
                               x, y, w, h, parent, nullptr, hInst, &data);
    if (!dlg) {
        if (monoFont) DeleteObject(monoFont);
        return false;
    }

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
    if (monoFont) DeleteObject(monoFont);
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
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

            // Also provide CF_UNICODETEXT for text editors
            std::wstring text;
            for (size_t i = 0; i < paths.size(); ++i) {
                if (i > 0) text += L"\r\n";
                text += paths[i];
            }
            size_t textBytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL hText = GlobalAlloc(GHND | GMEM_SHARE, textBytes);
            if (hText) {
                auto* pText = static_cast<wchar_t*>(GlobalLock(hText));
                if (pText) {
                    std::memcpy(pText, text.c_str(), textBytes);
                    GlobalUnlock(hText);
                    SetClipboardData(CF_UNICODETEXT, hText);
                } else {
                    GlobalFree(hText);
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
        msg += L"Ad: " + e.name + L"\n";
        msg += L"Konum: " + e.path() + L"\n";
        msg += L"Boyut: " + (e.isDir ? L"<klasör>" : formatSize(e.size)) + L"\n";
        msg += L"Değiştirilme: " + formatTime(e.mtime);
    } else {
        msg = (e.isDir ? L"Folder Information\n\n" : L"File Information\n\n");
        msg += L"Name: " + e.name + L"\n";
        msg += L"Location: " + e.path() + L"\n";
        msg += L"Size: " + (e.isDir ? L"<folder>" : formatSize(e.size)) + L"\n";
        msg += L"Modified: " + formatTime(e.mtime);
    }
    MessageBoxW(parent, msg.c_str(), isTurkish ? L"Özellikler" : L"Properties", MB_ICONINFORMATION | MB_OK);
}

// ------------------------------------------------------------- File Hash (SHA-256 / MD5) with Live Progress Bar
constexpr UINT WM_HASH_PROGRESS = WM_APP + 50;
constexpr UINT WM_HASH_DONE = WM_APP + 51;
constexpr UINT WM_HASH_ERROR = WM_APP + 52;

struct HashDlgData {
    std::wstring path, name, sha256, md5;
    int64_t fileSize = 0;
    bool isTurkish = true;
    bool computing = true;
    std::atomic<bool> cancel{false};
    std::thread worker;

    HWND hFile = nullptr;
    HWND hSize = nullptr;
    HWND hProgress = nullptr;
    HWND hProgressText = nullptr;
    HWND hSha = nullptr;
    HWND hMd5 = nullptr;
    HWND hInfo = nullptr;
    HWND hVerify = nullptr;
    HWND hStatus = nullptr;
    HWND btnCopySha = nullptr;
    HWND btnCopyMd5 = nullptr;
    HWND btnClose = nullptr;

    HFONT font = nullptr;
    HFONT monoFont = nullptr;
    bool done = false;
};

static void hashWorkerThread(HWND dlg, HashDlgData* d) {
    HANDLE hFile = CreateFileW(d->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        PostMessageW(dlg, WM_HASH_ERROR, 0, 0);
        return;
    }

    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            CloseHandle(hFile);
            PostMessageW(dlg, WM_HASH_ERROR, 0, 0);
            return;
        }
    }

    HCRYPTHASH hSha = 0, hMd5 = 0;
    CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hSha);
    CryptCreateHash(hProv, CALG_MD5, 0, 0, &hMd5);

    constexpr DWORD kChunk = 1024 * 1024; // 1 MB buffer
    std::vector<uint8_t> buf(kChunk);
    DWORD readBytes = 0;
    int64_t totalRead = 0;
    int lastPct = -1;

    while (ReadFile(hFile, buf.data(), kChunk, &readBytes, nullptr) && readBytes > 0) {
        if (d->cancel.load()) break;
        if (hSha) CryptHashData(hSha, buf.data(), readBytes, 0);
        if (hMd5) CryptHashData(hMd5, buf.data(), readBytes, 0);
        totalRead += readBytes;

        int pct = d->fileSize > 0 ? static_cast<int>((totalRead * 100) / d->fileSize) : 100;
        pct = std::min(100, std::max(0, pct));
        if (pct != lastPct) {
            lastPct = pct;
            PostMessageW(dlg, WM_HASH_PROGRESS, static_cast<WPARAM>(pct), 0);
        }
    }
    CloseHandle(hFile);

    if (d->cancel.load()) {
        if (hSha) CryptDestroyHash(hSha);
        if (hMd5) CryptDestroyHash(hMd5);
        CryptReleaseContext(hProv, 0);
        return;
    }

    auto toHex = [](const uint8_t* data, DWORD len) {
        static const wchar_t hex[] = L"0123456789abcdef";
        std::wstring s;
        s.reserve(len * 2);
        for (DWORD i = 0; i < len; ++i) {
            s += hex[(data[i] >> 4) & 0x0F];
            s += hex[data[i] & 0x0F];
        }
        return s;
    };

    if (hSha) {
        uint8_t hash[32];
        DWORD len = sizeof(hash);
        if (CryptGetHashParam(hSha, HP_HASHVAL, hash, &len, 0)) d->sha256 = toHex(hash, len);
        CryptDestroyHash(hSha);
    }
    if (hMd5) {
        uint8_t hash[16];
        DWORD len = sizeof(hash);
        if (CryptGetHashParam(hMd5, HP_HASHVAL, hash, &len, 0)) d->md5 = toHex(hash, len);
        CryptDestroyHash(hMd5);
    }
    CryptReleaseContext(hProv, 0);

    PostMessageW(dlg, WM_HASH_DONE, 0, 0);
}

static LRESULT CALLBACK HashWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<HashDlgData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            d = static_cast<HashDlgData*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
            bool tr = d->isTurkish;

            // 1. File Info
            std::wstring fileTitle = (tr ? L"Dosya: " : L"File: ") + d->name;
            d->hFile = CreateWindowExW(0, L"STATIC", fileTitle.c_str(), WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                                       16, 12, 490, 20, h, nullptr, cs->hInstance, nullptr);

            std::wstring sizeTitle = (tr ? L"Boyut: " : L"Size: ") + formatSize(d->fileSize) + L" (" + formatInt(d->fileSize) + (tr ? L" bayt)" : L" bytes)");
            d->hSize = CreateWindowExW(0, L"STATIC", sizeTitle.c_str(), WS_CHILD | WS_VISIBLE,
                                       16, 34, 490, 18, h, nullptr, cs->hInstance, nullptr);

            // 2. Progress Bar & Live Status
            d->hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                          16, 56, 490, 18, h, nullptr, cs->hInstance, nullptr);
            SendMessageW(d->hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessageW(d->hProgress, PBM_SETPOS, 0, 0);

            d->hProgressText = CreateWindowExW(0, L"STATIC",
                                              tr ? L"Dosya okunuyor ve parmak izi hesaplanıyor... %0" : L"Reading file and computing hash... %0",
                                              WS_CHILD | WS_VISIBLE, 16, 78, 490, 18, h, nullptr, cs->hInstance, nullptr);

            // 3. SHA-256 Output
            CreateWindowExW(0, L"STATIC", tr ? L"SHA-256 (64 Karakter - En Güçlü Bütünlük Kodu):" : L"SHA-256 (64 Chars - Strongest Integrity Hash):",
                            WS_CHILD | WS_VISIBLE, 16, 102, 490, 16, h, nullptr, cs->hInstance, nullptr);
            d->hSha = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", tr ? L"Hesaplanıyor..." : L"Computing...",
                                      WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                                      16, 120, 490, 24, h, reinterpret_cast<HMENU>(101), cs->hInstance, nullptr);

            // 4. MD5 Output
            CreateWindowExW(0, L"STATIC", tr ? L"MD5 (32 Karakter - Hızlı Bütünlük Kodu):" : L"MD5 (32 Chars - Legacy Verification Hash):",
                            WS_CHILD | WS_VISIBLE, 16, 150, 490, 16, h, nullptr, cs->hInstance, nullptr);
            d->hMd5 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", tr ? L"Hesaplanıyor..." : L"Computing...",
                                      WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                                      16, 168, 490, 24, h, reinterpret_cast<HMENU>(102), cs->hInstance, nullptr);

            // 5. Educational banner explanation
            const wchar_t* infoMsg = tr ?
                L"Bilgi: Bütünlük, dosya içeriğinden üretilen benzersiz dijital parmak izidir. Dosyada tek bir bayt bile bozulursa veya virüs bulaşırsa bu kod tamamen değişir." :
                L"Info: Integrity is a unique cryptographic fingerprint of file contents. If even 1 byte is corrupted or modified, this hash completely changes.";
            d->hInfo = CreateWindowExW(0, L"STATIC", infoMsg, WS_CHILD | WS_VISIBLE, 16, 198, 490, 36, h, nullptr, cs->hInstance, nullptr);

            // 6. Verification comparison box
            CreateWindowExW(0, L"STATIC", tr ? L"Doğrulama (Karşılaştırılacak hash kodunu buraya yapıştırın):" : L"Verification (Paste expected hash code here):",
                            WS_CHILD | WS_VISIBLE, 16, 238, 490, 18, h, nullptr, cs->hInstance, nullptr);
            d->hVerify = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                        16, 258, 490, 24, h, reinterpret_cast<HMENU>(103), cs->hInstance, nullptr);

            d->hStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 288, 490, 22, h, nullptr, cs->hInstance, nullptr);

            // 7. Buttons
            d->btnCopySha = CreateWindowExW(0, L"BUTTON", tr ? L"SHA-256 Kopyala" : L"Copy SHA-256",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
                                            16, 320, 140, 28, h, reinterpret_cast<HMENU>(104), cs->hInstance, nullptr);
            d->btnCopyMd5 = CreateWindowExW(0, L"BUTTON", tr ? L"MD5 Kopyala" : L"Copy MD5",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
                                            164, 320, 120, 28, h, reinterpret_cast<HMENU>(105), cs->hInstance, nullptr);
            d->btnClose = CreateWindowExW(0, L"BUTTON", tr ? L"Kapat" : L"Close",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                          416, 320, 90, 28, h, reinterpret_cast<HMENU>(IDCANCEL), cs->hInstance, nullptr);

            for (HWND c : {d->hFile, d->hSize, d->hProgressText, d->hInfo, d->hStatus, d->btnCopySha, d->btnCopyMd5, d->btnClose}) {
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
            }
            if (d->monoFont) {
                SendMessageW(d->hSha, WM_SETFONT, reinterpret_cast<WPARAM>(d->monoFont), TRUE);
                SendMessageW(d->hMd5, WM_SETFONT, reinterpret_cast<WPARAM>(d->monoFont), TRUE);
                SendMessageW(d->hVerify, WM_SETFONT, reinterpret_cast<WPARAM>(d->monoFont), TRUE);
            }

            d->worker = std::thread(hashWorkerThread, h, d);
            return 0;
        }
        case WM_HASH_PROGRESS: {
            if (d) {
                int pct = static_cast<int>(w);
                SendMessageW(d->hProgress, PBM_SETPOS, pct, 0);
                wchar_t buf[128];
                wsprintfW(buf, d->isTurkish ? L"Dosya okunuyor ve parmak izi hesaplanıyor... %%%d" : L"Reading file and computing hash... %%%d", pct);
                SetWindowTextW(d->hProgressText, buf);
            }
            return 0;
        }
        case WM_HASH_DONE: {
            if (d) {
                d->computing = false;
                SendMessageW(d->hProgress, PBM_SETPOS, 100, 0);
                SetWindowTextW(d->hProgressText, d->isTurkish ? L"Parmak izi hesaplama tamamlandı. Kodlar hazır." : L"Hash computation completed. Hashes are ready.");
                SetWindowTextW(d->hSha, d->sha256.c_str());
                SetWindowTextW(d->hMd5, d->md5.c_str());
                EnableWindow(d->btnCopySha, TRUE);
                EnableWindow(d->btnCopyMd5, TRUE);

                // trigger comparison if user already pasted text
                SendMessageW(h, WM_COMMAND, MAKEWPARAM(103, EN_CHANGE), reinterpret_cast<LPARAM>(d->hVerify));
            }
            return 0;
        }
        case WM_HASH_ERROR: {
            if (d) {
                d->computing = false;
                SetWindowTextW(d->hProgressText, d->isTurkish ? L"Hata: Dosya okunamadı veya erişim engellendi." : L"Error: File cannot be read or access denied.");
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 103 && HIWORD(w) == EN_CHANGE && d) {
                wchar_t buf[256] = {};
                GetWindowTextW(d->hVerify, buf, 255);
                std::wstring v = trim(fold(buf));
                if (v.empty()) {
                    SetWindowTextW(d->hStatus, L"");
                } else if (d->computing) {
                    SetWindowTextW(d->hStatus, d->isTurkish ? L"Hesaplanıyor, lütfen bekleyin..." : L"Still computing, please wait...");
                } else if (v == d->sha256 || v == d->md5) {
                    SetWindowTextW(d->hStatus, d->isTurkish ? L"DOGRULANDI: Dosya orijinal, sağlam ve eksiksiz (Birebir eşleşti)." : L"VERIFIED: File is genuine, intact, and authentic.");
                } else {
                    SetWindowTextW(d->hStatus, d->isTurkish ? L"UYARI: Hash eşleşmedi (Dosya bozuk veya farklı)." : L"WARNING: Hash does not match (File may be corrupt or altered).");
                }
                return 0;
            }
            if ((id == 104 || id == 105) && d) {
                const std::wstring& text = (id == 104) ? d->sha256 : d->md5;
                if (!text.empty()) {
                    if (OpenClipboard(h)) {
                        EmptyClipboard();
                        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                        if (mem) {
                            void* p = GlobalLock(mem);
                            if (p) {
                                memcpy(p, text.c_str(), bytes);
                                GlobalUnlock(mem);
                                SetClipboardData(CF_UNICODETEXT, mem);
                            }
                        }
                        CloseClipboard();
                        MessageBoxW(h, d->isTurkish ? L"Hash panoya kopyalandı." : L"Hash copied to clipboard.",
                                    d->isTurkish ? L"Bilgi" : L"Info", MB_OK | MB_ICONINFORMATION);
                    }
                }
                return 0;
            }
            if (id == IDOK || id == IDCANCEL) {
                if (d) {
                    d->cancel = true;
                    if (d->worker.joinable()) d->worker.join();
                    d->done = true;
                }
                DestroyWindow(h);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (d) {
                d->cancel = true;
                if (d->worker.joinable()) d->worker.join();
                d->done = true;
            }
            DestroyWindow(h);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool showFileHashDialog(HWND parent, const std::wstring& path, bool isTurkish) {
    size_t slash = path.rfind(L'\\');
    std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    int64_t fileSize = 0;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        fileSize = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }

    HashDlgData d;
    d.path = path;
    d.name = name;
    d.fileSize = fileSize;
    d.isTurkish = isTurkish;

    d.font = createModernFont(9, FW_NORMAL);
    d.monoFont = createModernFont(10, FW_NORMAL, true);

    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = HashWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"QBFindHashDlg";
        RegisterClassW(&wc);
        reg = true;
    }

    int w = 538, h = 400;
    RECT pr;
    GetWindowRect(parent, &pr);
    int x = pr.left + (pr.right - pr.left - w) / 2;
    int y = pr.top + (pr.bottom - pr.top - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"QBFindHashDlg",
                               isTurkish ? L"Dosya Bütünlüğü (Hash) Doğrulayıcı" : L"File Integrity & Hash Verifier",
                               WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, parent, nullptr, inst, &d);
    if (!dlg) {
        if (d.font) DeleteObject(d.font);
        if (d.monoFont) DeleteObject(d.monoFont);
        return false;
    }

    EnableWindow(parent, FALSE);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    d.cancel = true;
    if (d.worker.joinable()) d.worker.join();
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    if (d.font) DeleteObject(d.font);
    if (d.monoFont) DeleteObject(d.monoFont);
    return true;
}

// ------------------------------------------------------------- About Dialog
struct AboutDlgData {
    bool isTurkish = true;
    HFONT fontTitle = nullptr;
    HFONT fontSub = nullptr;
    HFONT fontDev = nullptr;
    HFONT fontRegular = nullptr;
    bool done = false;
    HWND hTabs = nullptr;
    HWND hPanel0 = nullptr;
    HWND hListShortcuts = nullptr;
    HWND hListSyntax = nullptr;
    HWND btnOk = nullptr;
};

static LRESULT CALLBACK AboutWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<AboutDlgData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            d = static_cast<AboutDlgData*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
            bool tr = d->isTurkish;

            HWND hTitle = CreateWindowExW(0, L"STATIC", L"QBFind Plus",
                                         WS_CHILD | WS_VISIBLE, 20, 14, 550, 26, h, nullptr, cs->hInstance, nullptr);
            HWND hSub = CreateWindowExW(0, L"STATIC",
                                        tr ? L"Sürüm 2.0 (x64 Native Win32 / C++20 Motoru)" :
                                             L"Version 2.0 (x64 Native Win32 / C++20 Engine)",
                                        WS_CHILD | WS_VISIBLE, 20, 40, 550, 18, h, nullptr, cs->hInstance, nullptr);
            HWND hDev = CreateWindowExW(0, L"STATIC",
                                        tr ? L"Geliştirici: tommyvercetti89 tarafında Google Antigravity ile geliştirildi." :
                                             L"Developer: Developed by tommyvercetti89 with Google Antigravity.",
                                        WS_CHILD | WS_VISIBLE, 20, 58, 550, 18, h, nullptr, cs->hInstance, nullptr);

            SendMessageW(hTitle, WM_SETFONT, reinterpret_cast<WPARAM>(d->fontTitle), TRUE);
            SendMessageW(hSub, WM_SETFONT, reinterpret_cast<WPARAM>(d->fontSub), TRUE);
            SendMessageW(hDev, WM_SETFONT, reinterpret_cast<WPARAM>(d->fontDev), TRUE);

            // Tab Control
            d->hTabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                       20, 84, 552, 320, h, reinterpret_cast<HMENU>(300), cs->hInstance, nullptr);
            SendMessageW(d->hTabs, WM_SETFONT, reinterpret_cast<WPARAM>(d->fontRegular), TRUE);

            const wchar_t* tabsTr[] = { L"Genel & Mimari", L"Klavye Kısayolları", L"Arama Sözdizimi" };
            const wchar_t* tabsEn[] = { L"General & Architecture", L"Keyboard Shortcuts", L"Search Syntax" };
            const wchar_t** tabs = tr ? tabsTr : tabsEn;
            for (int i = 0; i < 3; ++i) {
                TCITEMW tie = {};
                tie.mask = TCIF_TEXT;
                tie.pszText = const_cast<wchar_t*>(tabs[i]);
                TabCtrl_InsertItem(d->hTabs, i, &tie);
            }

            // Tab 0 View: Architecture Details
            d->hPanel0 = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                         24, 114, 544, 284, h, reinterpret_cast<HMENU>(301), cs->hInstance, nullptr);
            ListView_SetExtendedListViewStyle(d->hPanel0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
            SetWindowTheme(d->hPanel0, L"Explorer", nullptr);
            SendMessageW(d->hPanel0, WM_SETFONT, reinterpret_cast<WPARAM>(d->fontRegular), TRUE);

            LVCOLUMNW col0a = {LVCF_TEXT | LVCF_WIDTH, 0, 155, const_cast<wchar_t*>(tr ? L"Bileşen / Özellik" : L"Component / Feature")};
            LVCOLUMNW col0b = {LVCF_TEXT | LVCF_WIDTH, 0, 365, const_cast<wchar_t*>(tr ? L"Teknik Açıklama" : L"Technical Description")};
            ListView_InsertColumn(d->hPanel0, 0, &col0a);
            ListView_InsertColumn(d->hPanel0, 1, &col0b);

            struct InfoItem { const wchar_t *k, *v; };
            InfoItem infoTr[] = {
                { L"Çekirdek & API", L"Saf ISO C++20 ve doğrudan Win32 API. Sıfır harici bağımlılık, anında açılış." },
                { L"Bellek (DirPool)", L"Dize havuzlama ve sanallaştırma ile 1.8M dosyada ~40 MB RAM tüketimi." },
                { L"Canlı İndeksleme", L"NTFS USN Değişiklik Günlüğü ve dosya bildirimleri ile anlık otomatik takip." },
                { L"Bütünlük Motoru", L"Donanım hızlandırmalı Windows CNG ile SHA-256 ve MD5 akış doğrulaması." },
                { L"Sistem Entegrasyonu", L"OLE Sürükle-Bırak, Geri Dönüşüm Kutusu, yerel Windows dosya simgeleri." },
                { L"Geliştirici", L"tommyvercetti89 tarafında Google Antigravity ile geliştirildi." },
                { L"Lisans", L"Açık Kaynak (MIT Lisansı). Ticari ve kişisel kullanım için tamamen ücretsizdir." }
            };
            InfoItem infoEn[] = {
                { L"Core & Native API", L"Pure ISO C++20 and Win32 direct API. Zero external bloat, instant startup." },
                { L"DirPool Virtualization", L"Interned string pools keeping 1.8M indexed files in ~40 MB RAM." },
                { L"Real-Time Indexer", L"Instant file synchronization via NTFS USN Journal and file change watchers." },
                { L"Integrity Engine", L"Hardware-accelerated Windows CNG stream verification (SHA-256 / MD5)." },
                { L"Shell Integration", L"OLE Drag & Drop, Recycle Bin shell operations, native Windows file icons." },
                { L"Developer", L"Developed by tommyvercetti89 with Google Antigravity." },
                { L"License", L"Open Source (MIT License). Free for personal and commercial use." }
            };
            InfoItem* pInfo = tr ? infoTr : infoEn;
            int infoCount = tr ? (sizeof(infoTr) / sizeof(infoTr[0])) : (sizeof(infoEn) / sizeof(infoEn[0]));
            for (int i = 0; i < infoCount; ++i) {
                LVITEMW it = {};
                it.mask = LVIF_TEXT;
                it.iItem = i;
                it.pszText = const_cast<wchar_t*>(pInfo[i].k);
                ListView_InsertItem(d->hPanel0, &it);
                ListView_SetItemText(d->hPanel0, i, 1, const_cast<wchar_t*>(pInfo[i].v));
            }

            // Tab 1 View: Shortcuts List
            d->hListShortcuts = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                                WS_CHILD | LVS_REPORT | LVS_SINGLESEL,
                                                24, 114, 544, 284, h, reinterpret_cast<HMENU>(302), cs->hInstance, nullptr);
            ListView_SetExtendedListViewStyle(d->hListShortcuts, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
            SetWindowTheme(d->hListShortcuts, L"Explorer", nullptr);
            SendMessageW(d->hListShortcuts, WM_SETFONT, reinterpret_cast<WPARAM>(d->fontRegular), TRUE);

            LVCOLUMNW col1a = {LVCF_TEXT | LVCF_WIDTH, 0, 150, const_cast<wchar_t*>(tr ? L"Klavye Kısayolu" : L"Shortcut")};
            LVCOLUMNW col1b = {LVCF_TEXT | LVCF_WIDTH, 0, 370, const_cast<wchar_t*>(tr ? L"İşlev / Eylem" : L"Function / Action")};
            ListView_InsertColumn(d->hListShortcuts, 0, &col1a);
            ListView_InsertColumn(d->hListShortcuts, 1, &col1b);

            struct ShortcutItem { const wchar_t *k, *v; };
            ShortcutItem scTr[] = {
                { L"Enter", L"Seçili dosyayı veya klasörü açar" },
                { L"Space (Boşluk)", L"Hızlı yerel dosya önizleme penceresini açar" },
                { L"Ctrl + C", L"Dosyayı panoya kopyalar (Explorer klasörlerine yapıştırılabilir)" },
                { L"Ctrl + Shift + C", L"Dosyanın tam disk yolunu metin olarak panoya kopyalar" },
                { L"Ctrl + X", L"Dosyayı keser" },
                { L"Del (Delete)", L"Seçili dosyaları Geri Dönüşüm Kutusuna siler (Geri alınabilir)" },
                { L"Alt + Enter", L"Dosya veya klasör özellikler penceresini açar" },
                { L"Ctrl + A", L"Arama listesindeki tüm dosyaları seçer" },
                { L"Ctrl + R", L"Toplu Dosya Adı Değiştirici aracını açar" },
                { L"Ctrl + H", L"Dosya Bütünlüğü (SHA-256 / MD5) & Canlı Hash Doğrulayıcı" },
                { L"Ctrl + K", L"Gerçek Klasör Boyutu Hesaplayıcı aracını açar" },
                { L"Ctrl + F / F3", L"Arama metin kutusuna odaklanır" },
                { L"Ctrl + S", L"Geçerli arama sözdizimini Kayıtlı Aramalara ekler" },
                { L"F5", L"Disk indeksini baştan tarar ve günceller" },
                { L"F1", L"Bu Hakkında ve yardım penceresini açar" }
            };
            ShortcutItem scEn[] = {
                { L"Enter", L"Open selected file or folder" },
                { L"Space", L"Quick native file preview" },
                { L"Ctrl + C", L"Copy file object to clipboard (Paste directly in Explorer)" },
                { L"Ctrl + Shift + C", L"Copy full path as text to clipboard" },
                { L"Ctrl + X", L"Cut file object" },
                { L"Del", L"Send selected file(s) safely to Recycle Bin" },
                { L"Alt + Enter", L"Show file or folder properties" },
                { L"Ctrl + A", L"Select all items in search results list" },
                { L"Ctrl + R", L"Open Batch File Renamer tool" },
                { L"Ctrl + H", L"Open File Integrity & Live Hash Verifier (SHA-256 / MD5)" },
                { L"Ctrl + K", L"Open Real Folder Size Calculator" },
                { L"Ctrl + F / F3", L"Focus search input box" },
                { L"Ctrl + S", L"Save current query to Saved Searches" },
                { L"F5", L"Rescan and refresh drive indexes" },
                { L"F1", L"Open this About and Shortcuts dialog" }
            };
            ShortcutItem* pSc = tr ? scTr : scEn;
            int scCount = tr ? (sizeof(scTr) / sizeof(scTr[0])) : (sizeof(scEn) / sizeof(scEn[0]));
            for (int i = 0; i < scCount; ++i) {
                LVITEMW it = {};
                it.mask = LVIF_TEXT;
                it.iItem = i;
                it.pszText = const_cast<wchar_t*>(pSc[i].k);
                ListView_InsertItem(d->hListShortcuts, &it);
                ListView_SetItemText(d->hListShortcuts, i, 1, const_cast<wchar_t*>(pSc[i].v));
            }

            // Tab 2 View: Search Syntax List
            d->hListSyntax = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                             WS_CHILD | LVS_REPORT | LVS_SINGLESEL,
                                             24, 114, 544, 284, h, reinterpret_cast<HMENU>(303), cs->hInstance, nullptr);
            ListView_SetExtendedListViewStyle(d->hListSyntax, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
            SetWindowTheme(d->hListSyntax, L"Explorer", nullptr);
            SendMessageW(d->hListSyntax, WM_SETFONT, reinterpret_cast<WPARAM>(d->fontRegular), TRUE);

            LVCOLUMNW col2a = {LVCF_TEXT | LVCF_WIDTH, 0, 140, const_cast<wchar_t*>(tr ? L"Operatör / Filtre" : L"Operator / Filter")};
            LVCOLUMNW col2b = {LVCF_TEXT | LVCF_WIDTH, 0, 215, const_cast<wchar_t*>(tr ? L"Açıklama" : L"Description")};
            LVCOLUMNW col2c = {LVCF_TEXT | LVCF_WIDTH, 0, 165, const_cast<wchar_t*>(tr ? L"Örnek Kullanım" : L"Example")};
            ListView_InsertColumn(d->hListSyntax, 0, &col2a);
            ListView_InsertColumn(d->hListSyntax, 1, &col2b);
            ListView_InsertColumn(d->hListSyntax, 2, &col2c);

            struct SyntaxItem { const wchar_t *op, *desc, *ex; };
            SyntaxItem synTr[] = {
                { L"boyut: veya size:", L"Dosya boyutuna göre filtreleme", L"boyut:>1gb | size:10mb..100mb" },
                { L"ext: veya *.uzantı", L"Dosya uzantısına göre filtreleme", L"ext:pdf,docx,xlsx | *.png" },
                { L"tarih: veya date:", L"Değiştirilme tarihine göre", L"tarih:today | date:week" },
                { L"ad: veya name:", L"Yalnızca dosya adında arama", L"ad:rapor | name:setup" },
                { L"klasor: veya folder:", L"Belirli bir klasör yolunda kısıtlama", L"klasor:indirilenler" },
                { L"~kelime", L"Bulanık (Fuzzy) yazım toleranslı arama", L"~belge | ~rapor" },
                { L"a | b", L"VEYA mantıksal arama operatörü", L"rapor | sunum" },
                { L"regex:...", L"Tam düzenli ifade (RegEx) motoru", L"regex:^[0-9]+.*\\.txt$" }
            };
            SyntaxItem synEn[] = {
                { L"size: or boyut:", L"Filter by file size range", L"size:>1gb | size:10mb..100mb" },
                { L"ext: or *.extension", L"Filter by file extension", L"ext:pdf,docx,xlsx | *.png" },
                { L"date: or tarih:", L"Filter by modified date", L"date:today | date:week" },
                { L"name: or ad:", L"Restrict search to file name only", L"name:setup | ad:report" },
                { L"folder: or klasor:", L"Restrict search to specific folder", L"folder:downloads" },
                { L"~word", L"Typo-tolerant fuzzy search", L"~document | ~report" },
                { L"a | b", L"OR boolean matching operator", L"report | presentation" },
                { L"regex:...", L"Full regular expression engine", L"regex:^[0-9]+.*\\.txt$" }
            };
            SyntaxItem* pSyn = tr ? synTr : synEn;
            int synCount = tr ? (sizeof(synTr) / sizeof(synTr[0])) : (sizeof(synEn) / sizeof(synEn[0]));
            for (int i = 0; i < synCount; ++i) {
                LVITEMW it = {};
                it.mask = LVIF_TEXT;
                it.iItem = i;
                it.pszText = const_cast<wchar_t*>(pSyn[i].op);
                ListView_InsertItem(d->hListSyntax, &it);
                ListView_SetItemText(d->hListSyntax, i, 1, const_cast<wchar_t*>(pSyn[i].desc));
                ListView_SetItemText(d->hListSyntax, i, 2, const_cast<wchar_t*>(pSyn[i].ex));
            }

            // Close button
            d->btnOk = CreateWindowExW(0, L"BUTTON", tr ? L"Kapat" : L"Close",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       472, 412, 100, 30, h, reinterpret_cast<HMENU>(IDOK), cs->hInstance, nullptr);
            SendMessageW(d->btnOk, WM_SETFONT, reinterpret_cast<WPARAM>(d->fontRegular), TRUE);
            SetFocus(d->btnOk);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(l);
            if (nm && nm->code == TCN_SELCHANGE && d && nm->hwndFrom == d->hTabs) {
                int cur = TabCtrl_GetCurSel(d->hTabs);
                ShowWindow(d->hPanel0, cur == 0 ? SW_SHOW : SW_HIDE);
                ShowWindow(d->hListShortcuts, cur == 1 ? SW_SHOW : SW_HIDE);
                ShowWindow(d->hListSyntax, cur == 2 ? SW_SHOW : SW_HIDE);
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
                if (d) d->done = true;
                DestroyWindow(h);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (d) d->done = true;
            DestroyWindow(h);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void showAboutDialog(HWND parent, bool isTurkish) {
    AboutDlgData d;
    d.isTurkish = isTurkish;
    d.fontTitle = createModernFont(14, FW_BOLD);
    d.fontSub = createModernFont(9, FW_NORMAL);
    d.fontDev = createModernFont(9, FW_SEMIBOLD);
    d.fontRegular = createModernFont(9, FW_NORMAL);

    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = AboutWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"QBFindAboutDlg";
        RegisterClassW(&wc);
        reg = true;
    }

    int w = 608, h = 495;
    RECT pr;
    GetWindowRect(parent, &pr);
    int x = pr.left + (pr.right - pr.left - w) / 2;
    int y = pr.top + (pr.bottom - pr.top - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"QBFindAboutDlg",
                               isTurkish ? L"QBFind Plus Hakkında" : L"About QBFind Plus",
                               WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, parent, nullptr, inst, &d);
    if (!dlg) {
        if (d.fontTitle) DeleteObject(d.fontTitle);
        if (d.fontSub) DeleteObject(d.fontSub);
        if (d.fontDev) DeleteObject(d.fontDev);
        if (d.fontRegular) DeleteObject(d.fontRegular);
        return;
    }

    EnableWindow(parent, FALSE);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    if (d.fontTitle) DeleteObject(d.fontTitle);
    if (d.fontSub) DeleteObject(d.fontSub);
    if (d.fontDev) DeleteObject(d.fontDev);
    if (d.fontRegular) DeleteObject(d.fontRegular);
}

// ------------------------------------------------------------- Custom Size Filter Dialog
struct CustomSizeDlgData {
    std::wstring result;
    bool isTurkish;
    HFONT font;
    bool accepted = false;
    bool done = false;
    HWND hLabel = nullptr;
    HWND hEdit = nullptr;
    HWND btnOk = nullptr;
    HWND btnCancel = nullptr;
};

static LRESULT CALLBACK CustomSizeWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<CustomSizeDlgData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            d = static_cast<CustomSizeDlgData*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
            bool tr = d->isTurkish;

            d->hLabel = CreateWindowExW(0, L"STATIC",
                                        tr ? L"Dosya boyut filtresi girin (Örnek: >1gb, 100mb-1gb, <10mb, >=500mb):" :
                                             L"Enter file size filter (e.g. >1gb, 100mb-1gb, <10mb, >=500mb):",
                                        WS_CHILD | WS_VISIBLE, 18, 14, 430, 38, h, nullptr, cs->hInstance, nullptr);

            d->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"> 500mb",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                       18, 56, 430, 26, h, reinterpret_cast<HMENU>(101), cs->hInstance, nullptr);

            d->btnOk = CreateWindowExW(0, L"BUTTON", tr ? L"Uygula" : L"Apply",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       242, 98, 100, 28, h, reinterpret_cast<HMENU>(IDOK), cs->hInstance, nullptr);
            d->btnCancel = CreateWindowExW(0, L"BUTTON", tr ? L"İptal" : L"Cancel",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           348, 98, 100, 28, h, reinterpret_cast<HMENU>(IDCANCEL), cs->hInstance, nullptr);

            for (HWND c : {d->hLabel, d->hEdit, d->btnOk, d->btnCancel}) {
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
            }
            SetFocus(d->hEdit);
            SendMessageW(d->hEdit, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == IDOK && d) {
                wchar_t buf[128] = {};
                GetWindowTextW(d->hEdit, buf, 127);
                std::wstring val = trim(buf);
                if (!val.empty()) {
                    if (!val.starts_with(L"size:") && !val.starts_with(L"boyut:")) {
                        val = L"size:" + val;
                    }
                    d->result = val;
                    d->accepted = true;
                }
                d->done = true;
                DestroyWindow(h);
                return 0;
            }
            if (id == IDCANCEL && d) {
                d->done = true;
                DestroyWindow(h);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (d) d->done = true;
            DestroyWindow(h);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool showCustomSizeDialog(HWND parent, std::wstring& outSizeFilter, bool isTurkish) {
    CustomSizeDlgData d;
    d.isTurkish = isTurkish;
    d.font = createModernFont(9, FW_NORMAL);

    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = CustomSizeWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"QBFindCustomSizeDlg";
        RegisterClassW(&wc);
        reg = true;
    }

    int w = 480, h = 175;
    RECT pr;
    GetWindowRect(parent, &pr);
    int x = pr.left + (pr.right - pr.left - w) / 2;
    int y = pr.top + (pr.bottom - pr.top - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"QBFindCustomSizeDlg",
                               isTurkish ? L"Özel Boyut Filtresi" : L"Custom Size Filter",
                               WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, parent, nullptr, inst, &d);
    if (!dlg) {
        if (d.font) DeleteObject(d.font);
        return false;
    }

    EnableWindow(parent, FALSE);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    if (d.font) DeleteObject(d.font);
    if (d.accepted) {
        outSizeFilter = d.result;
        return true;
    }
    return false;
}

void showFolderSizeDialog(HWND parent, const std::wstring& folderPath, int64_t totalBytes, int64_t fileCount, int64_t subDirCount, bool isTurkish) {
    std::wstring msg;
    if (isTurkish) {
        msg = L"Klasör:\n" + folderPath + L"\n\n";
        msg += L"Toplam Boyut: " + formatSize(totalBytes) + L" (" + formatInt(totalBytes) + L" bayt)\n";
        msg += L"Dosya Sayısı: " + formatInt(fileCount) + L" dosya\n";
        msg += L"Alt Klasör Sayısı: " + formatInt(subDirCount) + L" klasör";
    } else {
        msg = L"Folder:\n" + folderPath + L"\n\n";
        msg += L"Total Size: " + formatSize(totalBytes) + L" (" + formatInt(totalBytes) + L" bytes)\n";
        msg += L"File Count: " + formatInt(fileCount) + L" files\n";
        msg += L"Subfolder Count: " + formatInt(subDirCount) + L" folders";
    }
    MessageBoxW(parent, msg.c_str(), isTurkish ? L"Gerçek Klasör Boyutu İstatistikleri" : L"Folder Size Statistics", MB_ICONINFORMATION | MB_OK);
}

// ------------------------------------------------------------- Batch Renamer
struct BatchItem {
    std::wstring oldPath, dir, oldName, newName;
};

struct BatchDlgData {
    std::vector<BatchItem> items;
    bool isTurkish;
    HWND hPrefix, hSuffix, hFind, hReplace, hList;
    HFONT font;
    bool accepted = false;
    bool done = false;
};

static void updateBatchPreview(BatchDlgData* d) {
    if (!d || !d->hList) return;
    wchar_t pfx[128] = {}, sfx[128] = {}, fnd[128] = {}, rep[128] = {};
    GetWindowTextW(d->hPrefix, pfx, 127);
    GetWindowTextW(d->hSuffix, sfx, 127);
    GetWindowTextW(d->hFind, fnd, 127);
    GetWindowTextW(d->hReplace, rep, 127);

    for (size_t i = 0; i < d->items.size(); ++i) {
        auto& it = d->items[i];
        std::wstring name = it.oldName;
        // Find & Replace
        if (fnd[0]) {
            size_t pos = 0;
            while ((pos = name.find(fnd, pos)) != std::wstring::npos) {
                name.replace(pos, wcslen(fnd), rep);
                pos += wcslen(rep);
            }
        }
        // Split name & ext
        size_t dot = name.rfind(L'.');
        std::wstring stem = (dot == std::wstring::npos) ? name : name.substr(0, dot);
        std::wstring ext = (dot == std::wstring::npos) ? L"" : name.substr(dot);
        // Prefix & Suffix
        name = std::wstring(pfx) + stem + std::wstring(sfx) + ext;
        it.newName = name;

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = static_cast<int>(i);
        lvi.iSubItem = 1;
        lvi.pszText = const_cast<wchar_t*>(it.newName.c_str());
        ListView_SetItem(d->hList, &lvi);
    }
}

static LRESULT CALLBACK BatchWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<BatchDlgData*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            d = static_cast<BatchDlgData*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
            bool tr = d->isTurkish;

            CreateWindowExW(0, L"STATIC", tr ? L"Önek Ekle:" : L"Add Prefix:", WS_CHILD | WS_VISIBLE, 16, 12, 100, 16, h, nullptr, cs->hInstance, nullptr);
            d->hPrefix = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                         16, 30, 240, 24, h, reinterpret_cast<HMENU>(201), cs->hInstance, nullptr);

            CreateWindowExW(0, L"STATIC", tr ? L"Sonek Ekle:" : L"Add Suffix:", WS_CHILD | WS_VISIBLE, 276, 12, 100, 16, h, nullptr, cs->hInstance, nullptr);
            d->hSuffix = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                         276, 30, 240, 24, h, reinterpret_cast<HMENU>(202), cs->hInstance, nullptr);

            CreateWindowExW(0, L"STATIC", tr ? L"Aranan Metin:" : L"Find Text:", WS_CHILD | WS_VISIBLE, 16, 62, 100, 16, h, nullptr, cs->hInstance, nullptr);
            d->hFind = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                       16, 80, 240, 24, h, reinterpret_cast<HMENU>(203), cs->hInstance, nullptr);

            CreateWindowExW(0, L"STATIC", tr ? L"Yeni Metin:" : L"Replace With:", WS_CHILD | WS_VISIBLE, 276, 62, 100, 16, h, nullptr, cs->hInstance, nullptr);
            d->hReplace = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                          276, 80, 240, 24, h, reinterpret_cast<HMENU>(204), cs->hInstance, nullptr);

            // Preview List
            d->hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                       16, 116, 500, 220, h, nullptr, cs->hInstance, nullptr);
            ListView_SetExtendedListViewStyle(d->hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            LVCOLUMNW col1 = {}, col2 = {};
            col1.mask = LVCF_TEXT | LVCF_WIDTH;
            col1.cx = 240;
            col1.pszText = const_cast<wchar_t*>(tr ? L"Geçerli Ad" : L"Current Name");
            ListView_InsertColumn(d->hList, 0, &col1);

            col2.mask = LVCF_TEXT | LVCF_WIDTH;
            col2.cx = 240;
            col2.pszText = const_cast<wchar_t*>(tr ? L"Yeni Ad (Önizleme)" : L"New Name (Preview)");
            ListView_InsertColumn(d->hList, 1, &col2);

            for (size_t i = 0; i < d->items.size(); ++i) {
                LVITEMW itm = {};
                itm.mask = LVIF_TEXT;
                itm.iItem = static_cast<int>(i);
                itm.pszText = const_cast<wchar_t*>(d->items[i].oldName.c_str());
                ListView_InsertItem(d->hList, &itm);
                ListView_SetItemText(d->hList, static_cast<int>(i), 1, const_cast<wchar_t*>(d->items[i].oldName.c_str()));
            }

            CreateWindowExW(0, L"BUTTON", tr ? L"Yeniden Adlandır" : L"Rename", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                            316, 350, 120, 28, h, reinterpret_cast<HMENU>(IDOK), cs->hInstance, nullptr);
            CreateWindowExW(0, L"BUTTON", tr ? L"İptal" : L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                            446, 350, 70, 28, h, reinterpret_cast<HMENU>(IDCANCEL), cs->hInstance, nullptr);

            SetWindowTheme(d->hList, L"Explorer", nullptr);

            EnumChildWindows(h, [](HWND child, LPARAM lParam) -> BOOL {
                SendMessageW(child, WM_SETFONT, lParam, TRUE);
                return TRUE;
            }, reinterpret_cast<LPARAM>(d->font));
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if ((id >= 201 && id <= 204) && HIWORD(w) == EN_CHANGE) {
                updateBatchPreview(d);
                return 0;
            }
            if (id == IDOK && d) {
                d->accepted = true;
                d->done = true;
                DestroyWindow(h);
                return 0;
            }
            if (id == IDCANCEL && d) {
                d->done = true;
                DestroyWindow(h);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (d) d->done = true;
            DestroyWindow(h);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool showBatchRenameDialog(HWND parent, const std::vector<std::wstring>& paths, bool isTurkish) {
    if (paths.empty()) return false;
    BatchDlgData d;
    d.isTurkish = isTurkish;
    for (const auto& p : paths) {
        size_t slash = p.rfind(L'\\');
        BatchItem it;
        it.oldPath = p;
        if (slash != std::wstring::npos) {
            it.dir = p.substr(0, slash);
            it.oldName = p.substr(slash + 1);
        } else {
            it.dir = L"";
            it.oldName = p;
        }
        it.newName = it.oldName;
        d.items.push_back(it);
    }

    d.font = createModernFont(9, FW_NORMAL);

    static bool reg = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = BatchWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"QBFindBatchRenameDlg";
        RegisterClassW(&wc);
        reg = true;
    }

    int w = 548, h = 428;
    RECT pr;
    GetWindowRect(parent, &pr);
    int x = pr.left + (pr.right - pr.left - w) / 2;
    int y = pr.top + (pr.bottom - pr.top - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"QBFindBatchRenameDlg",
                               isTurkish ? L"Toplu Dosya Adı Değiştirici" : L"Batch File Renamer",
                               WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h, parent, nullptr, inst, &d);
    if (!dlg) {
        DeleteObject(d.font);
        return false;
    }

    EnableWindow(parent, FALSE);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    DeleteObject(d.font);

    if (d.accepted) {
        int count = 0;
        for (const auto& it : d.items) {
            if (it.oldName != it.newName && !it.newName.empty()) {
                std::wstring target = it.dir.empty() ? it.newName : (it.dir + L"\\" + it.newName);
                if (MoveFileW(it.oldPath.c_str(), target.c_str())) {
                    ++count;
                }
            }
        }
        return count > 0;
    }
    return false;
}

}  // namespace qb
