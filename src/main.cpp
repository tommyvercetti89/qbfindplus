// QBFind C++ - Native Win32 high-performance desktop file finder.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core.h"
#include "indexer.h"
#include "search.h"
#include "ole_dragdrop.h"
#include "actions.h"

using namespace qb;

namespace {

constexpr size_t kMaxResults = 5000;
constexpr UINT WM_SEARCH_DONE = WM_APP + 1;
constexpr UINT_PTR TIMER_DEBOUNCE = 1, TIMER_STATUS = 2;
constexpr int ID_EDIT = 100, ID_LIST = 101, ID_STATUS = 102;

enum Cmd {
    CMD_OPEN = 200,
    CMD_PREVIEW,
    CMD_FOLDER,
    CMD_COPYPATH,
    CMD_COPYNAME,
    CMD_CUT,
    CMD_DELETE,
    CMD_DESKTOP,
    CMD_PROPERTIES,
    CMD_REFRESH,
    CMD_EXCLUDES,
    CMD_DARK_MODE,
    CMD_LANG_EN,
    CMD_LANG_TR,
    CMD_CLEAR_HISTORY,
    CMD_TOGGLE_SAVE_SEARCH,
    CMD_EXIT,
    CMD_HISTORY_BASE = 300,
    CMD_SAVED_BASE = 400
};

// ------------------------------------------------------------------ Localisation
struct Strings {
    const wchar_t *title, *hint, *colName, *colPath, *colSize, *colModified;
    const wchar_t *open, *preview, *folder, *copyPath, *copyName, *cut, *del, *desktop, *properties;
    const wchar_t *refresh, *excludes, *language, *darkMode, *exit, *file, *search, *settings, *history, *saved, *saveSearch, *clearHistory;
    const wchar_t *statusMin, *statusResults, *statusIndex, *scanning, *folderDir, *emptyList;
    const wchar_t *exclTitle, *exclLabel, *ok, *cancel;
    const wchar_t *confirmDelete, *confirmDeleteTitle;
};

const Strings kEn = {
    L"QBFind", L"Search files... (ext:pdf name:report size:>10mb date:today ~fuzzy a|b regex:...)",
    L"Name", L"Folder", L"Size", L"Modified",
    L"Open\tEnter", L"Preview\tSpace", L"Show in folder", L"Copy path\tCtrl+C", L"Copy name",
    L"Cut\tCtrl+X", L"Delete\tDel", L"Copy to Desktop", L"Properties\tAlt+Enter",
    L"Refresh index\tF5", L"Excluded folders...", L"Language", L"Dark mode", L"Exit",
    L"File", L"Search", L"Settings", L"History", L"Saved Searches", L"Save current search", L"Clear history",
    L"Type at least 2 characters  |  %s indexed%s", L"%s results  |  %s indexed%s", L"%s indexed%s",
    L"  (scanning...)", L"<folder>", L"(empty)",
    L"Excluded folders", L"Semicolon separated names or globs (e.g. node_modules;.git;cache*):", L"OK", L"Cancel",
    L"Are you sure you want to send the selected file(s) to the Recycle Bin?", L"Confirm Delete"
};

const Strings kTr = {
    L"QBFind", L"Dosya ara... (ext:pdf name:rapor size:>10mb date:today ~bulanik a|b regex:...)",
    L"Ad", L"Klasör", L"Boyut", L"Değiştirilme",
    L"Aç\tEnter", L"Önizleme\tSpace", L"Klasörde göster", L"Yolu kopyala\tCtrl+C", L"Adı kopyala",
    L"Kes\tCtrl+X", L"Sil\tDel", L"Masaüstüne kopyala", L"Özellikler\tAlt+Enter",
    L"Dizini yenile\tF5", L"Hariç tutulan klasörler...", L"Dil", L"Karanlık mod", L"Çıkış",
    L"Dosya", L"Arama", L"Ayarlar", L"Geçmiş", L"Kayıtlı Aramalar", L"Geçerli aramayı kaydet", L"Geçmişi temizle",
    L"En az 2 karakter yazın  |  %s öğe dizinlendi%s", L"%s sonuç  |  %s öğe dizinlendi%s", L"%s öğe dizinlendi%s",
    L"  (taranıyor...)", L"<klasör>", L"(boş)",
    L"Hariç tutulan klasörler", L"Noktalı virgülle ayrılmış ad veya desenler (örn. node_modules;.git;cache*):", L"Tamam", L"İptal",
    L"Seçili öğe(ler) Geri Dönüşüm Kutusuna gönderilsin mi?", L"Silme Onayı"
};

struct Row {
    std::wstring path, name, dir, size, modified;
    int64_t sizeVal = 0, mtime = 0;
    bool isDir = false;
    int iconIndex = 0;
};

struct SearchJob {
    std::wstring text;
    bool has = false;
};

struct App {
    HWND hwnd = nullptr, edit = nullptr, list = nullptr, status = nullptr;
    HFONT font = nullptr;
    HINSTANCE inst = nullptr;
    Settings settings;
    const Strings* str = &kEn;
    Indexer indexer;

    std::vector<Row> rows;          // UI thread only
    std::vector<size_t> rank;       // display order -> rows index
    int sortCol = -1;
    bool sortAsc = true;
    std::wstring lastQuery;
    int64_t shownCount = -1;
    int shownScan = -1;

    // Shell Icons
    HIMAGELIST shellImageList = nullptr;
    std::mutex iconMu;
    std::unordered_map<std::wstring, int> iconCache;

    // Search worker
    std::thread searchThread, initThread;
    std::mutex sMu;
    std::condition_variable sCv;
    SearchJob job;
    bool quitting = false;
    std::atomic<bool> cancel{false};
    std::mutex rMu;
    std::vector<Row> latestRows;
    std::wstring latestQuery;
    uint64_t latestGen = 0;
    uint64_t jobGen = 0;
    uint64_t appliedGen = 0;
};

App* g = nullptr;

std::wstring fmtStatus(const wchar_t* fmt, const std::wstring& a, const std::wstring& b, const std::wstring& c) {
    std::wstring out;
    out.reserve(160);
    const std::wstring* args[3] = {&a, &b, &c};
    int k = 0;
    for (const wchar_t* p = fmt; *p; ++p) {
        if (p[0] == L'%' && p[1] == L's' && k < 3) {
            out += *args[k++];
            ++p;
        } else {
            out += *p;
        }
    }
    return out;
}

std::wstring editText() {
    int n = GetWindowTextLengthW(g->edit);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    int got = GetWindowTextW(g->edit, s.data(), n + 1);
    s.resize(got > 0 ? got : 0);
    return trim(s);
}

void setStatus(const std::wstring& s) { SetWindowTextW(g->status, s.c_str()); }

std::wstring scanSuffix() { return g->indexer.scanning() ? g->str->scanning : L""; }

void updateIndexStatus() {
    setStatus(fmtStatus(g->str->statusIndex, formatInt(g->indexer.count(), L','), scanSuffix(), L""));
}

int getIconIndex(const std::wstring& ext, bool isDir) {
    std::wstring key = isDir ? L"<dir>" : (ext.empty() ? L"<file>" : fold(ext));
    {
        std::lock_guard<std::mutex> lk(g->iconMu);
        auto it = g->iconCache.find(key);
        if (it != g->iconCache.end()) return it->second;
    }

    SHFILEINFOW sfi = {};
    DWORD flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
    if (isDir) {
        SHGetFileInfoW(L"dummy", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi), flags);
    } else {
        std::wstring fakeName = L"dummy" + (ext.empty() ? L"" : ext);
        SHGetFileInfoW(fakeName.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags);
    }

    std::lock_guard<std::mutex> lk(g->iconMu);
    g->iconCache[key] = sfi.iIcon;
    return sfi.iIcon;
}

Row toRow(const Entry& e) {
    Row r;
    r.path = e.path;
    r.name = std::wstring(e.name());
    r.dir = e.path.substr(0, e.nameOff > 0 ? e.nameOff - 1 : 0);
    if (r.dir.size() == 2 && r.dir[1] == L':') r.dir += L'\\';
    r.isDir = e.isDir;
    r.sizeVal = e.size;
    r.mtime = e.mtime;
    r.size = e.isDir ? g->str->folderDir : formatSize(e.size);
    r.modified = formatTime(e.mtime);
    r.iconIndex = getIconIndex(std::wstring(e.lowerExt()), e.isDir);
    return r;
}

// ------------------------------------------------------------------ Search Worker
void searchThreadMain() {
    for (;;) {
        std::wstring text;
        uint64_t gen;
        {
            std::unique_lock<std::mutex> lk(g->sMu);
            g->sCv.wait(lk, [] { return g->quitting || g->job.has; });
            if (g->quitting) return;
            text = std::move(g->job.text);
            g->job.has = false;
            gen = g->jobGen;
            g->cancel = false;
        }
        try {
            Query q = parseQuery(text);
            std::vector<size_t> idx;
            std::vector<Row> rows;
            bool ok = true;
            if (!q.empty()) {
                g->indexer.withEntries([&](const std::vector<Entry>& entries) {
                    ok = searchEntries(entries, q, kMaxResults, g->cancel, idx);
                    if (ok) {
                        rows.reserve(idx.size());
                        for (size_t i : idx) rows.push_back(toRow(entries[i]));
                    }
                });
            }
            if (!ok) continue;
            {
                std::lock_guard<std::mutex> lk(g->rMu);
                g->latestRows = std::move(rows);
                g->latestQuery = text;
                g->latestGen = gen;
            }
            PostMessageW(g->hwnd, WM_SEARCH_DONE, 0, 0);
        } catch (const std::exception& ex) {
            logf("search failed: %s", ex.what());
        } catch (...) {
            logf("search failed: unknown");
        }
    }
}

void requestSearch(const std::wstring& text) {
    {
        std::lock_guard<std::mutex> lk(g->sMu);
        g->job.text = text;
        g->job.has = true;
        ++g->jobGen;
        g->cancel = true;
    }
    g->sCv.notify_one();
}

void applyOrder() {
    g->rank.resize(g->rows.size());
    for (size_t i = 0; i < g->rank.size(); ++i) g->rank[i] = i;
    if (g->sortCol < 0) return;
    int col = g->sortCol;
    bool asc = g->sortAsc;
    auto lessFn = [&](size_t a, size_t b) {
        const Row &x = g->rows[a], &y = g->rows[b];
        int c = 0;
        switch (col) {
            case 0: c = _wcsicmp(x.name.c_str(), y.name.c_str()); break;
            case 1: c = _wcsicmp(x.dir.c_str(), y.dir.c_str()); break;
            case 2: c = (x.sizeVal < y.sizeVal) ? -1 : (x.sizeVal > y.sizeVal); break;
            case 3: c = (x.mtime < y.mtime) ? -1 : (x.mtime > y.mtime); break;
        }
        return asc ? c < 0 : c > 0;
    };
    std::stable_sort(g->rank.begin(), g->rank.end(), lessFn);
}

void applyResults() {
    std::vector<Row> rows;
    std::wstring q;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(g->rMu);
        rows = std::move(g->latestRows);
        g->latestRows.clear();
        q = g->latestQuery;
        gen = g->latestGen;
    }
    if (gen < g->appliedGen) return;
    {
        std::lock_guard<std::mutex> lk(g->sMu);
        if (gen != g->jobGen) return;
    }
    g->appliedGen = gen;

    std::wstring keep;
    int sel = ListView_GetNextItem(g->list, -1, LVNI_SELECTED);
    if (sel >= 0 && static_cast<size_t>(sel) < g->rank.size()) keep = g->rows[g->rank[sel]].path;

    g->rows = std::move(rows);
    applyOrder();
    ListView_SetItemCountEx(g->list, static_cast<int>(g->rows.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    ListView_SetItemState(g->list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (!keep.empty()) {
        for (size_t i = 0; i < g->rank.size(); ++i) {
            if (g->rows[g->rank[i]].path == keep) {
                ListView_SetItemState(g->list, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(g->list, static_cast<int>(i), FALSE);
                break;
            }
        }
    }
    InvalidateRect(g->list, nullptr, TRUE);
    setStatus(fmtStatus(g->str->statusResults, formatInt(static_cast<int64_t>(g->rows.size()), L','),
                        formatInt(g->indexer.count(), L','), scanSuffix()));
}

void createMenuBar();

void runSearch() {
    std::wstring q = editText();
    g->lastQuery = q;
    if (q.size() < 2) {
        {
            std::lock_guard<std::mutex> lk(g->sMu);
            g->job.has = false;
            ++g->jobGen;
            g->cancel = true;
        }
        g->rows.clear();
        g->rank.clear();
        ListView_SetItemCountEx(g->list, 0, 0);
        InvalidateRect(g->list, nullptr, TRUE);
        setStatus(fmtStatus(g->str->statusMin, formatInt(g->indexer.count(), L','), scanSuffix(), L""));
        return;
    }
    addHistory(q);
    requestSearch(q);
}

// ------------------------------------------------------------------ Actions Helpers
std::vector<const Row*> selectedRows() {
    std::vector<const Row*> out;
    int i = -1;
    while ((i = ListView_GetNextItem(g->list, i, LVNI_SELECTED)) >= 0)
        if (static_cast<size_t>(i) < g->rank.size()) out.push_back(&g->rows[g->rank[i]]);
    return out;
}

std::vector<std::wstring> selectedPaths() {
    std::vector<std::wstring> out;
    for (const auto* r : selectedRows()) out.push_back(r->path);
    return out;
}

void copyToClipboard(const std::wstring& text) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(g->hwnd)) {
            EmptyClipboard();
            size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (mem) {
                void* p = GlobalLock(mem);
                if (p) {
                    memcpy(p, text.c_str(), bytes);
                    GlobalUnlock(mem);
                    if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
                } else {
                    GlobalFree(mem);
                }
            }
            CloseClipboard();
            return;
        }
        Sleep(20);
    }
}

void openRow(const Row& r) {
    DWORD a = GetFileAttributesW(r.path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g->hwnd, (r.path + L"\n\nNot found / Bulunamadı").c_str(), g->str->title, MB_ICONWARNING);
        return;
    }
    std::wstring dir = r.isDir ? r.path : r.dir;
    ShellExecuteW(g->hwnd, L"open", r.path.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
}

void showInFolder(const Row& r) {
    std::wstring args = L"/select,\"" + r.path + L"\"";
    ShellExecuteW(g->hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void openSelected() {
    auto sel = selectedRows();
    if (sel.empty() && !g->rank.empty()) {
        openRow(g->rows[g->rank[0]]);
        return;
    }
    size_t n = 0;
    for (auto* r : sel) {
        if (++n > 10) break;
        openRow(*r);
    }
}

void previewSelected() {
    auto sel = selectedRows();
    if (sel.empty()) return;
    const Row* r = sel[0];
    if (!previewFile(g->hwnd, r->path, r->name, g->font)) {
        // Fallback: system preview or properties
        ShellExecuteW(g->hwnd, L"preview", r->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void copySelected(bool nameOnly) {
    std::wstring text;
    for (auto* r : selectedRows()) {
        if (!text.empty()) text += L"\r\n";
        text += nameOnly ? r->name : r->path;
    }
    if (!text.empty()) copyToClipboard(text);
}

void cutSelected() {
    auto paths = selectedPaths();
    if (!paths.empty()) putFilesOnClipboard(g->hwnd, paths, true);
}

void deleteSelected() {
    auto paths = selectedPaths();
    if (paths.empty()) return;
    if (MessageBoxW(g->hwnd, g->str->confirmDelete, g->str->confirmDeleteTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) {
        if (deleteFilesToRecycleBin(g->hwnd, paths)) {
            runSearch();
        }
    }
}

void copyToDesktopSelected() {
    auto paths = selectedPaths();
    if (!paths.empty()) copyFilesToDesktop(g->hwnd, paths);
}

void propertiesSelected() {
    auto sel = selectedRows();
    if (sel.empty()) return;
    showFileProperties(g->hwnd, sel[0]->path);
}

// ------------------------------------------------------------------ Dark Mode
void applyDarkMode() {
    BOOL dark = g->settings.darkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(g->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    SetWindowTheme(g->list, dark ? L"Explorer" : nullptr, nullptr);
    InvalidateRect(g->hwnd, nullptr, TRUE);
}

// ------------------------------------------------------------------ Menus & Settings
void setLanguage(const std::wstring& lang);

HMENU buildMenuBar() {
    const Strings& s = *g->str;
    HMENU bar = CreateMenu();

    // File Menu
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, CMD_OPEN, s.open);
    AppendMenuW(file, MF_STRING, CMD_PREVIEW, s.preview);
    AppendMenuW(file, MF_STRING, CMD_FOLDER, s.folder);
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, CMD_COPYPATH, s.copyPath);
    AppendMenuW(file, MF_STRING, CMD_COPYNAME, s.copyName);
    AppendMenuW(file, MF_STRING, CMD_CUT, s.cut);
    AppendMenuW(file, MF_STRING, CMD_DESKTOP, s.desktop);
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, CMD_DELETE, s.del);
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, CMD_PROPERTIES, s.properties);
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, CMD_REFRESH, s.refresh);
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, CMD_EXIT, s.exit);

    // Search Menu (History & Saved Searches)
    HMENU searchMenu = CreatePopupMenu();
    HMENU historyMenu = CreatePopupMenu();
    auto history = loadHistory();
    if (history.empty()) {
        AppendMenuW(historyMenu, MF_STRING | MF_GRAYED, 0, s.emptyList);
    } else {
        for (size_t i = 0; i < history.size() && i < 20; ++i) {
            AppendMenuW(historyMenu, MF_STRING, CMD_HISTORY_BASE + static_cast<UINT>(i), history[i].c_str());
        }
    }
    AppendMenuW(searchMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(historyMenu), s.history);

    HMENU savedMenu = CreatePopupMenu();
    auto saved = loadSavedSearches();
    if (saved.empty()) {
        AppendMenuW(savedMenu, MF_STRING | MF_GRAYED, 0, s.emptyList);
    } else {
        for (size_t i = 0; i < saved.size() && i < 20; ++i) {
            AppendMenuW(savedMenu, MF_STRING, CMD_SAVED_BASE + static_cast<UINT>(i), saved[i].c_str());
        }
    }
    AppendMenuW(searchMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(savedMenu), s.saved);
    AppendMenuW(searchMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(searchMenu, MF_STRING, CMD_TOGGLE_SAVE_SEARCH, s.saveSearch);
    AppendMenuW(searchMenu, MF_STRING, CMD_CLEAR_HISTORY, s.clearHistory);

    // Settings Menu
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING | (g->settings.darkMode ? MF_CHECKED : 0), CMD_DARK_MODE, s.darkMode);
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, CMD_EXCLUDES, s.excludes);
    HMENU lang = CreatePopupMenu();
    AppendMenuW(lang, MF_STRING | (g->str == &kEn ? MF_CHECKED : 0), CMD_LANG_EN, L"English");
    AppendMenuW(lang, MF_STRING | (g->str == &kTr ? MF_CHECKED : 0), CMD_LANG_TR, L"Türkçe");
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(lang), s.language);

    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), s.file);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(searchMenu), s.search);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), s.settings);
    return bar;
}

void createMenuBar() {
    HMENU old = GetMenu(g->hwnd);
    SetMenu(g->hwnd, buildMenuBar());
    if (old) DestroyMenu(old);
    DrawMenuBar(g->hwnd);
}

void applyLanguageToUi() {
    const Strings& s = *g->str;
    SetWindowTextW(g->hwnd, s.title);
    SendMessageW(g->edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(s.hint));
    const wchar_t* cols[] = {s.colName, s.colPath, s.colSize, s.colModified};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        ListView_SetColumn(g->list, i, &c);
    }
    createMenuBar();
}

void setLanguage(const std::wstring& lang) {
    g->str = (lang == L"tr") ? &kTr : &kEn;
    g->settings.lang = lang;
    saveSettings(g->settings);
    applyLanguageToUi();
    g->shownCount = -1;
    runSearch();
    if (g->lastQuery.size() < 2) updateIndexStatus();
}

// ------------------------------------------------------------------ Exclusions Dialog
struct ExclDlg {
    HWND edit = nullptr;
    bool accepted = false;
    bool done = false;
    std::wstring result;
};

LRESULT CALLBACK ExclProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<ExclDlg*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            d = static_cast<ExclDlg*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
            const Strings& s = *g->str;
            HWND lbl = CreateWindowExW(0, L"STATIC", s.exclLabel, WS_CHILD | WS_VISIBLE, 12, 12, 456, 36, h, nullptr, g->inst, nullptr);
            d->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", d->result.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      12, 52, 456, 24, h, reinterpret_cast<HMENU>(10), g->inst, nullptr);
            HWND ok = CreateWindowExW(0, L"BUTTON", s.ok, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 288, 92, 84, 26, h,
                                      reinterpret_cast<HMENU>(IDOK), g->inst, nullptr);
            HWND cn = CreateWindowExW(0, L"BUTTON", s.cancel, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 384, 92, 84, 26, h,
                                      reinterpret_cast<HMENU>(IDCANCEL), g->inst, nullptr);
            for (HWND c : {lbl, d->edit, ok, cn}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g->font), TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDOK && d) {
                int n = GetWindowTextLengthW(d->edit);
                std::wstring s(static_cast<size_t>(n) + 1, L'\0');
                GetWindowTextW(d->edit, s.data(), n + 1);
                s.resize(n > 0 ? n : 0);
                d->result = s;
                d->accepted = true;
                d->done = true;
                DestroyWindow(h);
            } else if (LOWORD(w) == IDCANCEL && d) {
                d->done = true;
                DestroyWindow(h);
            }
            return 0;
        case WM_CLOSE:
            if (d) d->done = true;
            DestroyWindow(h);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void editExcludes() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ExclProc;
        wc.hInstance = g->inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"QBFindExcludes";
        RegisterClassW(&wc);
        registered = true;
    }
    ExclDlg d;
    for (size_t i = 0; i < g->settings.excludes.size(); ++i) d.result += (i ? L";" : L"") + g->settings.excludes[i];
    RECT r{0, 0, 480, 130};
    AdjustWindowRectEx(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    RECT pr;
    GetWindowRect(g->hwnd, &pr);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = pr.left + ((pr.right - pr.left) - w) / 2, y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"QBFindExcludes", g->str->exclTitle, WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, w, h,
                               g->hwnd, nullptr, g->inst, &d);
    if (!dlg) return;
    EnableWindow(g->hwnd, FALSE);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g->hwnd, TRUE);
    SetForegroundWindow(g->hwnd);
    if (!d.accepted) return;

    std::vector<std::wstring> ex;
    for (auto& part : split(d.result, L';')) {
        std::wstring p = fold(trim(part));
        if (!p.empty()) ex.push_back(p);
    }
    if (ex == g->settings.excludes) return;
    g->settings.excludes = ex;
    saveSettings(g->settings);
    g->indexer.stop();
    g->indexer.setExcludes(ex);
    g->indexer.rescan();
}

// ------------------------------------------------------------------ Layout & Subclassing
void layout() {
    RECT rc;
    GetClientRect(g->hwnd, &rc);
    int pad = MulDiv(8, GetDpiForWindow(g->hwnd), 96);
    TEXTMETRICW tm;
    HDC dc = GetDC(g->hwnd);
    HGDIOBJ old = SelectObject(dc, g->font);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g->hwnd, dc);
    int editH = tm.tmHeight + pad + 4, statusH = tm.tmHeight + pad;
    MoveWindow(g->edit, pad, pad, rc.right - 2 * pad, editH, TRUE);
    int listTop = pad * 2 + editH;
    MoveWindow(g->list, 0, listTop, rc.right, std::max<int>(0, rc.bottom - listTop - statusH), TRUE);
    MoveWindow(g->status, pad, rc.bottom - statusH, rc.right - 2 * pad, statusH, TRUE);

    int total = rc.right - GetSystemMetrics(SM_CXVSCROLL);
    int wName = total * 28 / 100, wSize = total * 11 / 100, wMod = total * 16 / 100;
    ListView_SetColumnWidth(g->list, 0, wName);
    ListView_SetColumnWidth(g->list, 2, wSize);
    ListView_SetColumnWidth(g->list, 3, wMod);
    ListView_SetColumnWidth(g->list, 1, std::max(50, total - wName - wSize - wMod));
}

LRESULT CALLBACK EditSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN) {
        if (w == VK_DOWN && !g->rank.empty()) {
            SetFocus(g->list);
            ListView_SetItemState(g->list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            return 0;
        }
        if (w == VK_RETURN) { openSelected(); return 0; }
        if (w == VK_ESCAPE) { SetWindowTextW(h, L""); return 0; }
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
    if (m == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK ListSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (w == VK_RETURN) {
            if (alt) propertiesSelected();
            else openSelected();
            return 0;
        }
        if (w == VK_SPACE) { previewSelected(); return 0; }
        if (w == VK_DELETE) { deleteSelected(); return 0; }
        if (w == VK_ESCAPE) { SetFocus(g->edit); return 0; }
        if (ctrl && w == 'C') { copySelected(false); return 0; }
        if (ctrl && w == 'X') { cutSelected(); return 0; }
        if (ctrl && w == 'F') { SetFocus(g->edit); SendMessageW(g->edit, EM_SETSEL, 0, -1); return 0; }
    }
    return DefSubclassProc(h, m, w, l);
}

void showContextMenu(POINT pt) {
    auto sel = selectedRows();
    if (sel.empty()) return;
    const Strings& s = *g->str;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_OPEN, s.open);
    AppendMenuW(menu, MF_STRING, CMD_PREVIEW, s.preview);
    AppendMenuW(menu, MF_STRING, CMD_FOLDER, s.folder);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_COPYPATH, s.copyPath);
    AppendMenuW(menu, MF_STRING, CMD_COPYNAME, s.copyName);
    AppendMenuW(menu, MF_STRING, CMD_CUT, s.cut);
    AppendMenuW(menu, MF_STRING, CMD_DESKTOP, s.desktop);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_DELETE, s.del);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_PROPERTIES, s.properties);
    SetForegroundWindow(g->hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g->hwnd, nullptr);
    DestroyMenu(menu);
}

void onTimerStatus() {
    int64_t count = g->indexer.count();
    int scan = g->indexer.scanning() ? 1 : 0;
    if (count == g->shownCount && scan == g->shownScan) return;
    g->shownCount = count;
    g->shownScan = scan;
    if (g->lastQuery.size() >= 2) runSearch();
    else updateIndexStatus();
}

LRESULT onNotify(NMHDR* nh, LPARAM lParam) {
    if (nh->hwndFrom != g->list) return 0;
    switch (nh->code) {
        case LVN_GETDISPINFOW: {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(nh);
            size_t i = static_cast<size_t>(di->item.iItem);
            if (i >= g->rank.size()) break;
            const Row& r = g->rows[g->rank[i]];
            if (di->item.mask & LVIF_TEXT) {
                const std::wstring* s = &r.name;
                switch (di->item.iSubItem) {
                    case 1: s = &r.dir; break;
                    case 2: s = &r.size; break;
                    case 3: s = &r.modified; break;
                }
                di->item.pszText = const_cast<wchar_t*>(s->c_str());
            }
            if (di->item.mask & LVIF_IMAGE) {
                di->item.iImage = r.iconIndex;
            }
            break;
        }
        case LVN_BEGINDRAG: {
            auto paths = selectedPaths();
            if (paths.empty()) {
                auto* nm = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (nm->iItem >= 0 && static_cast<size_t>(nm->iItem) < g->rank.size()) {
                    paths.push_back(g->rows[g->rank[nm->iItem]].path);
                }
            }
            if (!paths.empty()) {
                doDragDropFiles(g->hwnd, paths);
            }
            break;
        }
        case LVN_COLUMNCLICK: {
            int col = reinterpret_cast<NMLISTVIEW*>(nh)->iSubItem;
            if (g->sortCol == col) g->sortAsc = !g->sortAsc;
            else { g->sortCol = col; g->sortAsc = true; }
            applyOrder();
            ListView_SetItemState(g->list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            InvalidateRect(g->list, nullptr, TRUE);
            break;
        }
        case NM_DBLCLK: {
            auto* a = reinterpret_cast<NMITEMACTIVATE*>(nh);
            if (a->iItem >= 0 && static_cast<size_t>(a->iItem) < g->rank.size()) openRow(g->rows[g->rank[a->iItem]]);
            break;
        }
        case NM_RCLICK: {
            auto* a = reinterpret_cast<NMITEMACTIVATE*>(nh);
            if (a->iItem >= 0 && !(ListView_GetItemState(g->list, a->iItem, LVIS_SELECTED) & LVIS_SELECTED)) {
                ListView_SetItemState(g->list, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(g->list, a->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
            POINT pt;
            GetCursorPos(&pt);
            showContextMenu(pt);
            break;
        }
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    try {
        switch (m) {
            case WM_CREATE: {
                g->hwnd = h;
                NONCLIENTMETRICSW ncm = {};
                ncm.cbSize = sizeof(ncm);
                SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, GetDpiForWindow(h));
                g->font = CreateFontIndirectW(&ncm.lfMessageFont);
                g->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, h,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT)), g->inst, nullptr);
                g->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS, 0, 0, 10, 10, h,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), g->inst, nullptr);
                g->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE, 0, 0, 10, 10, h,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), g->inst, nullptr);
                for (HWND c : {g->edit, g->list, g->status}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g->font), TRUE);
                ListView_SetExtendedListViewStyle(g->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP | LVS_EX_SUBITEMIMAGES);

                // Attach System Shell Image List (Native Windows Icons)
                SHFILEINFOW sfi = {};
                auto himl = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"dummy", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                                                                        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
                if (himl) {
                    g->shellImageList = himl;
                    ListView_SetImageList(g->list, himl, LVSIL_SMALL);
                }

                for (int i = 0; i < 4; ++i) {
                    LVCOLUMNW c{};
                    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                    c.fmt = (i == 2) ? LVCFMT_RIGHT : LVCFMT_LEFT;
                    c.cx = 100;
                    c.pszText = const_cast<wchar_t*>(L"");
                    ListView_InsertColumn(g->list, i, &c);
                }
                SetWindowSubclass(g->edit, EditSub, 1, 0);
                SetWindowSubclass(g->list, ListSub, 1, 0);
                applyLanguageToUi();
                applyDarkMode();
                layout();
                SetTimer(h, TIMER_STATUS, 400, nullptr);
                SetFocus(g->edit);

                g->indexer.setExcludes(g->settings.excludes);
                g->searchThread = std::thread(searchThreadMain);
                g->initThread = std::thread([] { g->indexer.init(); });
                return 0;
            }
            case WM_SIZE:
                if (g && g->list && w != SIZE_MINIMIZED) layout();
                return 0;
            case WM_DPICHANGED: {
                auto* r = reinterpret_cast<RECT*>(l);
                SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                NONCLIENTMETRICSW ncm = {};
                ncm.cbSize = sizeof(ncm);
                SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, HIWORD(w));
                HFONT nf = CreateFontIndirectW(&ncm.lfMessageFont);
                for (HWND c : {g->edit, g->list, g->status}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(nf), TRUE);
                DeleteObject(g->font);
                g->font = nf;
                layout();
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(l);
                mm->ptMinTrackSize = {480, 320};
                return 0;
            }
            case WM_SETFOCUS:
                SetFocus(g->edit);
                return 0;
            case WM_TIMER:
                if (w == TIMER_DEBOUNCE) {
                    KillTimer(h, TIMER_DEBOUNCE);
                    runSearch();
                } else if (w == TIMER_STATUS) {
                    onTimerStatus();
                }
                return 0;
            case WM_SEARCH_DONE:
                applyResults();
                return 0;
            case WM_NOTIFY:
                return onNotify(reinterpret_cast<NMHDR*>(l), l);
            case WM_CONTEXTMENU:
                if (reinterpret_cast<HWND>(w) == g->list && l != -1) {
                    return 0;
                }
                if (l == -1 && GetFocus() == g->list) {
                    RECT rc;
                    GetWindowRect(g->list, &rc);
                    showContextMenu({rc.left + 40, rc.top + 40});
                    return 0;
                }
                break;
            case WM_COMMAND: {
                int id = LOWORD(w);
                if (id == ID_EDIT && HIWORD(w) == EN_CHANGE) {
                    SetTimer(h, TIMER_DEBOUNCE, 150, nullptr);
                    return 0;
                }
                if (id >= CMD_HISTORY_BASE && id < CMD_HISTORY_BASE + 30) {
                    auto history = loadHistory();
                    size_t idx = id - CMD_HISTORY_BASE;
                    if (idx < history.size()) {
                        SetWindowTextW(g->edit, history[idx].c_str());
                        SendMessageW(g->edit, EM_SETSEL, history[idx].size(), history[idx].size());
                        runSearch();
                    }
                    return 0;
                }
                if (id >= CMD_SAVED_BASE && id < CMD_SAVED_BASE + 30) {
                    auto saved = loadSavedSearches();
                    size_t idx = id - CMD_SAVED_BASE;
                    if (idx < saved.size()) {
                        SetWindowTextW(g->edit, saved[idx].c_str());
                        SendMessageW(g->edit, EM_SETSEL, saved[idx].size(), saved[idx].size());
                        runSearch();
                    }
                    return 0;
                }

                switch (id) {
                    case CMD_OPEN: openSelected(); break;
                    case CMD_PREVIEW: previewSelected(); break;
                    case CMD_FOLDER: { auto s = selectedRows(); if (!s.empty()) showInFolder(*s[0]); break; }
                    case CMD_COPYPATH: copySelected(false); break;
                    case CMD_COPYNAME: copySelected(true); break;
                    case CMD_CUT: cutSelected(); break;
                    case CMD_DELETE: deleteSelected(); break;
                    case CMD_DESKTOP: copyToDesktopSelected(); break;
                    case CMD_PROPERTIES: propertiesSelected(); break;
                    case CMD_REFRESH: g->indexer.rescan(); break;
                    case CMD_EXCLUDES: editExcludes(); break;
                    case CMD_DARK_MODE:
                        g->settings.darkMode = !g->settings.darkMode;
                        saveSettings(g->settings);
                        applyDarkMode();
                        createMenuBar();
                        break;
                    case CMD_LANG_EN: setLanguage(L"en"); break;
                    case CMD_LANG_TR: setLanguage(L"tr"); break;
                    case CMD_TOGGLE_SAVE_SEARCH:
                        toggleSavedSearch(editText());
                        createMenuBar();
                        break;
                    case CMD_CLEAR_HISTORY:
                        clearHistory();
                        createMenuBar();
                        break;
                    case CMD_EXIT: DestroyWindow(h); break;
                }
                return 0;
            }
            case WM_KEYDOWN:
                if (w == VK_F5) { g->indexer.rescan(); return 0; }
                break;
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            case WM_DESTROY:
                KillTimer(h, TIMER_STATUS);
                PostQuitMessage(0);
                return 0;
        }
    } catch (const std::exception& ex) {
        logf("wndproc exception (msg %u): %s", m, ex.what());
        return 0;
    } catch (...) {
        logf("wndproc unknown exception (msg %u)", m);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // 1. Crash reporter initialization
    initCrashReporter();

    // 2. Single instance check
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\QBFindCpp.SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"QBFindCppMain", nullptr)) {
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        CloseHandle(single);
        return 0;
    }

    // 3. COM / OLE Initialization for Drag & Drop and Shell operations
    OleInitialize(nullptr);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    auto app = std::make_unique<App>();
    g = app.get();
    g->inst = inst;
    g->settings = loadSettings();
    std::wstring lang = g->settings.lang;
    if (lang.empty()) lang = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH) ? L"tr" : L"en";
    g->str = (lang == L"tr") ? &kTr : &kEn;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"QBFindCppMain";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, g->str->title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 640, nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5) {
            g->indexer.rescan();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Clean orderly shutdown: stop producers before the App is destroyed
    {
        std::lock_guard<std::mutex> lk(g->sMu);
        g->quitting = true;
        g->cancel = true;
    }
    g->sCv.notify_all();
    if (g->searchThread.joinable()) g->searchThread.join();
    if (g->initThread.joinable()) g->initThread.join();
    g->indexer.stop();
    if (g->font) DeleteObject(g->font);

    OleUninitialize();
    if (single) CloseHandle(single);
    return static_cast<int>(msg.wParam);
}
