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
#include <psapi.h>

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
constexpr UINT WM_INDEX_CHANGED = WM_APP + 2;
constexpr UINT_PTR TIMER_DEBOUNCE = 1, TIMER_STATUS = 2;
constexpr int ID_EDIT = 100, ID_LIST = 101, ID_STATUS = 102;

enum Cmd {
    CMD_OPEN = 200,
    CMD_PREVIEW,
    CMD_FOLDER,
    CMD_COPYFILE = 203,
    CMD_COPYPATH = 204,
    CMD_COPYNAME = 205,
    CMD_CUT = 206,
    CMD_DELETE = 207,
    CMD_DESKTOP = 208,
    CMD_PROPERTIES = 209,
    CMD_REFRESH = 210,
    CMD_EXCLUDES = 211,
    CMD_DARK_MODE = 212,
    CMD_LANG_EN = 213,
    CMD_LANG_TR = 214,
    CMD_CLEAR_HISTORY = 215,
    CMD_TOGGLE_SAVE_SEARCH = 216,
    CMD_EXIT = 217,
    CMD_TOGGLE_TOOLBAR = 220,
    CMD_TOGGLE_TABS = 221,
    CMD_TRIM_RAM = 222,
    CMD_TOGGLE_SCOPE_POS = 223,
    CMD_TOGGLE_BREADCRUMB = 224,
    CMD_BATCH_RENAME = 225,
    CMD_FILE_HASH = 226,
    CMD_FOLDER_SIZE = 227,
    CMD_ABOUT = 230,
    CMD_SIZE_ALL = 231,
    CMD_SIZE_GIGANTIC = 232,
    CMD_SIZE_HUGE = 233,
    CMD_SIZE_LARGE = 234,
    CMD_SIZE_MEDIUM = 235,
    CMD_SIZE_SMALL = 236,
    CMD_SIZE_TINY = 237,
    CMD_SIZE_CUSTOM = 238,
    CMD_SELECT_ALL = 240,
    CMD_INVERT_SELECT = 241,
    CMD_HISTORY_BASE = 300,
    CMD_SAVED_BASE = 400,
    ID_BTN_COPY = 501,
    ID_BTN_OPEN = 502,
    ID_BTN_FOLDER = 503,
    ID_BTN_PREVIEW = 504,
    ID_BTN_DELETE = 505,
    ID_BTN_DETAILS = 506,
    ID_BTN_REFRESH = 507,
    ID_TABS = 510,
    ID_COMBO_SCOPE = 511,
    ID_BTN_UP = 512,
    ID_BREADCRUMB = 513
};

// ------------------------------------------------------------------ Localisation
struct Strings {
    const wchar_t *title, *hint, *colName, *colPath, *colSize, *colModified;
    const wchar_t *open, *preview, *folder, *copyFile, *copyPath, *copyName, *cut, *del, *desktop, *properties;
    const wchar_t *selectAll, *invertSelect;
    const wchar_t *refresh, *excludes, *language, *darkMode, *toolbar, *categoryTabs, *exit, *file, *search, *settings, *history, *saved, *saveSearch, *clearHistory;
    const wchar_t *statusMin, *statusResults, *statusIndex, *scanning, *folderDir, *emptyList;
    const wchar_t *exclTitle, *exclLabel, *ok, *cancel;
    const wchar_t *confirmDelete, *confirmDeleteTitle;
    const wchar_t *btnCopy, *btnOpen, *btnFolder, *btnPreview, *btnDelete, *btnDetails, *btnRefresh;
    const wchar_t *copiedSingle, *copiedMulti;
    const wchar_t *tools, *batchRename, *fileHash, *folderSize, *trimRam, *ramTrimmed;
    const wchar_t *scopeRight, *breadcrumbBar;
    const wchar_t *view, *help, *about;
    const wchar_t *filterBySize, *sizeAll, *sizeGigantic, *sizeHuge, *sizeLarge, *sizeMedium, *sizeSmall, *sizeTiny, *sizeCustom;
};

const Strings kEn = {
    L"QBFind", L"Search files... (ext:pdf name:report size:>10mb date:today ~fuzzy a|b regex:...)",
    L"Name", L"Folder", L"Size", L"Modified",
    L"Open\tEnter", L"Preview\tSpace", L"Show in folder", L"Copy file\tCtrl+C", L"Copy path\tCtrl+Shift+C", L"Copy name",
    L"Cut\tCtrl+X", L"Delete\tDel", L"Copy to Desktop", L"Properties\tAlt+Enter",
    L"Select all\tCtrl+A", L"Invert selection",
    L"Refresh index\tF5", L"Excluded folders...", L"Language", L"Dark mode", L"Toolbar", L"Category Tabs", L"Exit",
    L"File", L"Search", L"Settings", L"History", L"Saved Searches", L"Save current search", L"Clear history",
    L"Type at least 2 characters  |  %s indexed%s", L"%s results  |  %s indexed%s", L"%s indexed%s",
    L"  (scanning...)", L"<folder>", L"(empty)",
    L"Excluded folders", L"Semicolon separated names or globs (e.g. node_modules;.git;cache*):", L"OK", L"Cancel",
    L"Are you sure you want to send the selected file(s) to the Recycle Bin?", L"Confirm Delete",
    L"Copy", L"Open", L"Folder", L"Preview", L"Delete", L"Properties", L"Refresh",
    L"File copied to clipboard! Paste anywhere with Ctrl+V.",
    L"%s files copied to clipboard! Paste anywhere with Ctrl+V.",
    L"Tools", L"Batch Rename...\tCtrl+R", L"File Integrity (SHA-256 / MD5)...\tCtrl+H", L"Calculate Folder Size...\tCtrl+K",
    L"Trim Memory (Purge RAM)", L"Memory trimmed! Working set released.",
    L"Scope Selector on Right", L"Address Breadcrumb Bar",
    L"View", L"Help", L"About QBFind Plus...\tF1",
    L"Filter by Size", L"All (No Size Filter)", L"Gigantic (> 1 GB)", L"Huge (100 MB - 1 GB)", L"Large (16 MB - 100 MB)",
    L"Medium (1 MB - 16 MB)", L"Small (< 1 MB)", L"Tiny (< 100 KB)", L"Custom Size..."
};

const Strings kTr = {
    L"QBFind", L"Dosya ara... (ext:pdf name:rapor size:>10mb date:today ~bulanik a|b regex:...)",
    L"Ad", L"Klasör", L"Boyut", L"Değiştirilme",
    L"Aç\tEnter", L"Önizleme\tSpace", L"Klasörde göster", L"Dosyayı kopyala\tCtrl+C", L"Yolu kopyala\tCtrl+Shift+C", L"Adı kopyala",
    L"Kes\tCtrl+X", L"Sil\tDel", L"Masaüstüne kopyala", L"Özellikler\tAlt+Enter",
    L"Tümünü seç\tCtrl+A", L"Seçimi tersine çevir",
    L"Dizini yenile\tF5", L"Hariç tutulan klasörler...", L"Dil", L"Karanlık mod", L"Araç Çubuğu", L"Kategori Sekmeleri", L"Çıkış",
    L"Dosya", L"Arama", L"Ayarlar", L"Geçmiş", L"Kayıtlı Aramalar", L"Geçerli aramayı kaydet", L"Geçmişi temizle",
    L"En az 2 karakter yazın  |  %s öğe dizinlendi%s", L"%s sonuç  |  %s öğe dizinlendi%s", L"%s öğe dizinlendi%s",
    L"  (taranıyor...)", L"<klasör>", L"(boş)",
    L"Hariç tutulan klasörler", L"Noktalı virgülle ayrılmış ad veya desenler (örn. node_modules;.git;cache*):", L"Tamam", L"İptal",
    L"Seçili öğe(ler) Geri Dönüşüm Kutusuna gönderilsin mi?", L"Silme Onayı",
    L"Kopyala", L"Aç", L"Klasör", L"Önizle", L"Sil", L"Özellikler", L"Yenile",
    L"Dosya panoya kopyalandı! İstediğiniz yere Ctrl+V ile yapıştırabilirsiniz.",
    L"%s dosya panoya kopyalandı! İstediğiniz yere Ctrl+V ile yapıştırabilirsiniz.",
    L"Araçlar", L"Toplu Yeniden Adlandır...\tCtrl+R", L"Dosya Bütünlüğü (SHA-256 / MD5)...\tCtrl+H", L"Klasör Boyutunu Hesapla...\tCtrl+K",
    L"Belleği Temizle (RAM'i Boşalt)", L"Bellek temizlendi! Boştaki RAM serbest bırakıldı.",
    L"Konum Seçiciyi Sağa Al", L"Adres Yolu Çubuğu",
    L"Görünüm", L"Yardım", L"QBFind Plus Hakkında...\tF1",
    L"Boyuta Göre Filtrele", L"Tümü (Filtresiz)", L"Devasa (> 1 GB)", L"Çok Büyük (100 MB - 1 GB)", L"Büyük (16 MB - 100 MB)",
    L"Orta (1 MB - 16 MB)", L"Küçük (< 1 MB)", L"Çok Küçük (< 100 KB)", L"Özel Boyut..."
};

struct Row {
    std::wstring path, name, dir, size, modified;
    int64_t sizeVal = 0, mtime = 0;
    bool isDir = false;
    int iconIndex = 0;
};

struct SearchJob {
    std::wstring text;
    FileCategory category = CAT_ALL;
    ScopeType scope = SCOPE_ALL;
    std::wstring browseDir;
    bool browseDirectOnly = false;
    bool has = false;
};

struct App {
    HWND hwnd = nullptr, edit = nullptr, list = nullptr, status = nullptr;
    HWND btnCopy = nullptr, btnOpen = nullptr, btnFolder = nullptr, btnPreview = nullptr;
    HWND btnDelete = nullptr, btnDetails = nullptr, btnRefresh = nullptr;
    HWND tabs = nullptr, comboScope = nullptr, btnUp = nullptr;
    HWND breadcrumb = nullptr;
    FileCategory category = CAT_ALL;
    ScopeType scope = SCOPE_ALL;
    std::wstring currentBrowseDir;

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

    // Dark mode resources
    HBRUSH darkEditBrush = nullptr;
    HBRUSH darkWndBrush = nullptr;
    COLORREF darkEditBg = RGB(36, 36, 36);
    COLORREF darkEditText = RGB(240, 240, 240);
    COLORREF darkWndBg = RGB(24, 24, 24);
    COLORREF darkStatusText = RGB(190, 190, 190);
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

size_t getRamUsageMb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024 * 1024);
    }
    return 0;
}

void updateIndexStatus() {
    std::wstring ramStr = L"  |  RAM: " + std::to_wstring(getRamUsageMb()) + L" MB";
    setStatus(fmtStatus(g->str->statusIndex, formatInt(g->indexer.count(), L','), scanSuffix(), L"") + ramStr);
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
    r.path = e.path();
    r.name = e.name;
    r.dir = std::wstring(e.dir());
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
        std::wstring text, browseDir;
        FileCategory cat = CAT_ALL;
        ScopeType scope = SCOPE_ALL;
        bool browseDirectOnly = false;
        uint64_t gen;
        {
            std::unique_lock<std::mutex> lk(g->sMu);
            g->sCv.wait(lk, [] { return g->quitting || g->job.has; });
            if (g->quitting) return;
            text = std::move(g->job.text);
            cat = g->job.category;
            scope = g->job.scope;
            browseDir = std::move(g->job.browseDir);
            browseDirectOnly = g->job.browseDirectOnly;
            g->job.has = false;
            gen = g->jobGen;
            g->cancel = false;
        }
        try {
            Query q = parseQuery(text);
            q.category = cat;
            if (q.scope == SCOPE_ALL) q.scope = scope;
            q.browseDir = std::move(browseDir);
            q.browseDirectOnly = browseDirectOnly;
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

void requestSearch(const std::wstring& text, FileCategory cat, ScopeType sc = SCOPE_ALL,
                   const std::wstring& bDir = L"", bool bDirect = false) {
    {
        std::lock_guard<std::mutex> lk(g->sMu);
        g->job.text = text;
        g->job.category = cat;
        g->job.scope = sc;
        g->job.browseDir = bDir;
        g->job.browseDirectOnly = bDirect;
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
    std::wstring ramStr = L"  |  RAM: " + std::to_wstring(getRamUsageMb()) + L" MB";
    setStatus(fmtStatus(g->str->statusResults, formatInt(static_cast<int64_t>(g->rows.size()), L','),
                        formatInt(g->indexer.count(), L','), scanSuffix()) + ramStr);
}

void createMenuBar();
void layout();
void updateBreadcrumb();

void runSearch() {
    std::wstring q = editText();
    g->lastQuery = q;
    bool isEmpty = q.empty();
    bool isAll = (g->scope == SCOPE_ALL && g->category == CAT_ALL && g->currentBrowseDir.empty());

    if (isEmpty && isAll) {
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
        std::wstring ramStr = L"  |  RAM: " + std::to_wstring(getRamUsageMb()) + L" MB";
        setStatus(fmtStatus(g->str->statusMin, formatInt(g->indexer.count(), L','), scanSuffix(), L"") + ramStr);
        return;
    }
    if (q.size() >= 2) addHistory(q);
    bool browseDirect = isEmpty && !g->currentBrowseDir.empty();
    requestSearch(q, g->category, g->scope, g->currentBrowseDir, browseDirect);
}

void navigateUp() {
    if (g->currentBrowseDir.empty()) return;
    std::wstring p = g->currentBrowseDir;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t slash = p.rfind(L'\\');
    if (slash == std::wstring::npos) slash = p.rfind(L'/');
    if (slash != std::wstring::npos) {
        if (slash <= 2 && p[1] == L':') {
            g->currentBrowseDir = p.substr(0, 3);
        } else {
            g->currentBrowseDir = p.substr(0, slash);
        }
    } else {
        g->currentBrowseDir.clear();
        g->scope = SCOPE_ALL;
        SendMessageW(g->comboScope, CB_SETCURSEL, static_cast<WPARAM>(SCOPE_ALL), 0);
    }
    updateBreadcrumb();
    layout();
    runSearch();
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

void selectAll() {
    if (!g || !g->list) return;
    ListView_SetItemState(g->list, -1, LVIS_SELECTED, LVIS_SELECTED);
}

void invertSelection() {
    if (!g || !g->list) return;
    int count = ListView_GetItemCount(g->list);
    for (int i = 0; i < count; ++i) {
        UINT state = ListView_GetItemState(g->list, i, LVIS_SELECTED);
        ListView_SetItemState(g->list, i, (state & LVIS_SELECTED) ? 0 : LVIS_SELECTED, LVIS_SELECTED);
    }
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
    std::wstring p = r.path;
    if (p.size() > 3 && p.back() == L'\\') p.pop_back();
    std::wstring args = L"/select,\"" + p + L"\"";
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

void oneClickCopy() {
    auto paths = selectedPaths();
    if (paths.empty()) {
        int sel = ListView_GetNextItem(g->list, -1, LVNI_FOCUSED);
        if (sel >= 0 && static_cast<size_t>(sel) < g->rank.size()) {
            paths.push_back(g->rows[g->rank[sel]].path);
        } else if (!g->rank.empty()) {
            paths.push_back(g->rows[g->rank[0]].path);
        }
    }
    if (paths.empty()) return;
    if (putFilesOnClipboard(g->hwnd, paths, false)) {
        if (paths.size() == 1) {
            setStatus(g->str->copiedSingle);
        } else {
            wchar_t buf[256];
            wsprintfW(buf, g->str->copiedMulti, formatInt(static_cast<int64_t>(paths.size()), L',').c_str());
            setStatus(buf);
        }
    }
}

void copyPathSelected() {
    std::wstring text;
    for (auto* r : selectedRows()) {
        if (!text.empty()) text += L"\r\n";
        text += r->path;
    }
    if (text.empty()) {
        int sel = ListView_GetNextItem(g->list, -1, LVNI_FOCUSED);
        if (sel >= 0 && static_cast<size_t>(sel) < g->rank.size()) {
            text = g->rows[g->rank[sel]].path;
        } else if (!g->rank.empty()) {
            text = g->rows[g->rank[0]].path;
        }
    }
    if (!text.empty()) {
        copyToClipboard(text);
        setStatus(g->str == &kTr ? L"Dosya yolu panoya kopyalandı" : L"Path copied to clipboard");
    }
}

void copyNameSelected() {
    std::wstring text;
    for (auto* r : selectedRows()) {
        if (!text.empty()) text += L"\r\n";
        text += r->name;
    }
    if (text.empty()) {
        int sel = ListView_GetNextItem(g->list, -1, LVNI_FOCUSED);
        if (sel >= 0 && static_cast<size_t>(sel) < g->rank.size()) {
            text = g->rows[g->rank[sel]].name;
        } else if (!g->rank.empty()) {
            text = g->rows[g->rank[0]].name;
        }
    }
    if (!text.empty()) {
        copyToClipboard(text);
        setStatus(g->str == &kTr ? L"Dosya adı panoya kopyalandı" : L"Name copied to clipboard");
    }
}

void applySizeFilter(const std::wstring& sizeFilter) {
    std::wstring text = editText();
    std::vector<std::wstring> tokens = split(text, L' ');
    std::wstring newText;
    for (const auto& t : tokens) {
        if (t.starts_with(L"size:") || t.starts_with(L"boyut:")) continue;
        if (!newText.empty()) newText += L" ";
        newText += t;
    }
    if (!sizeFilter.empty()) {
        if (!newText.empty()) newText += L" ";
        newText += L"size:" + sizeFilter;
    }
    SetWindowTextW(g->edit, newText.c_str());
    SendMessageW(g->edit, EM_SETSEL, newText.size(), newText.size());
    runSearch();
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
    if (sel.empty()) {
        if (!g->rank.empty()) sel.push_back(&g->rows[g->rank[0]]);
        else return;
    }
    showFileProperties(g->hwnd, sel[0]->path);
}

void batchRenameSelected() {
    auto paths = selectedPaths();
    if (paths.empty()) {
        int sel = ListView_GetNextItem(g->list, -1, LVNI_FOCUSED);
        if (sel >= 0 && static_cast<size_t>(sel) < g->rank.size()) {
            paths.push_back(g->rows[g->rank[sel]].path);
        } else if (!g->rank.empty()) {
            paths.push_back(g->rows[g->rank[0]].path);
        }
    }
    if (paths.empty()) {
        MessageBoxW(g->hwnd,
                    g->str == &kTr ? L"Lütfen yeniden adlandırmak için en az bir dosya seçin." : L"Please select at least one file to rename.",
                    g->str->title, MB_ICONINFORMATION);
        return;
    }
    if (paths.size() <= 1 && g->rank.size() > 1) {
        wchar_t prompt[512];
        if (g->str == &kTr) {
            swprintf_s(prompt, L"Mevcut arama listesindeki tüm dosyalar (%zu adet) toplu adlandırmaya dahil edilsin mi?\n\n- Evet: Listedeki tüm dosyaları (%zu adet) toplu adlandır\n- Hayır: Yalnızca seçili tek dosyayı adlandır\n- İptal: İşlemi iptal et",
                       g->rank.size(), g->rank.size());
        } else {
            swprintf_s(prompt, L"Include all files in the current search results (%zu items) for batch rename?\n\n- Yes: Batch rename all %zu files\n- No: Rename only the single selected file\n- Cancel: Abort",
                       g->rank.size(), g->rank.size());
        }
        int choice = MessageBoxW(g->hwnd, prompt, g->str->title, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL) return;
        if (choice == IDYES) {
            paths.clear();
            for (size_t idx : g->rank) {
                paths.push_back(g->rows[idx].path);
            }
        }
    }
    if (showBatchRenameDialog(g->hwnd, paths, g->str == &kTr)) {
        g->indexer.rescan();
        runSearch();
    }
}

void fileHashSelected() {
    auto sel = selectedRows();
    if (sel.empty()) {
        int focused = ListView_GetNextItem(g->list, -1, LVNI_FOCUSED);
        if (focused >= 0 && static_cast<size_t>(focused) < g->rank.size()) {
            sel.push_back(&g->rows[g->rank[focused]]);
        } else if (!g->rank.empty()) {
            sel.push_back(&g->rows[g->rank[0]]);
        }
    }
    if (sel.empty()) {
        MessageBoxW(g->hwnd,
                    g->str == &kTr ? L"Lütfen bütünlüğünü doğrulamak için bir dosya seçin." : L"Please select a file to verify hash.",
                    g->str->title, MB_ICONINFORMATION);
        return;
    }
    showFileHashDialog(g->hwnd, sel[0]->path, g->str == &kTr);
}

void calculateAndShowFolderSize(HWND parent, const std::wstring& folderPath) {
    int64_t totalBytes = 0, fileCount = 0, dirCount = 0;
    std::wstring folded = fold(folderPath);
    while (folded.size() > 3 && (folded.back() == L'\\' || folded.back() == L'/')) folded.pop_back();
    std::wstring prefix = folded + L'\\';

    g->indexer.withEntries([&](const std::vector<Entry>& entries) {
        for (const auto& e : entries) {
            std::wstring_view edir = e.lowerDir();
            if (edir == folded || edir.starts_with(prefix)) {
                if (e.isDir) ++dirCount;
                else {
                    ++fileCount;
                    totalBytes += e.size;
                }
            }
        }
    });
    showFolderSizeDialog(parent, folderPath, totalBytes, fileCount, dirCount, g->str == &kTr);
}

void folderSizeSelected() {
    auto sel = selectedRows();
    if (sel.empty()) {
        int focused = ListView_GetNextItem(g->list, -1, LVNI_FOCUSED);
        if (focused >= 0 && static_cast<size_t>(focused) < g->rank.size()) {
            sel.push_back(&g->rows[g->rank[focused]]);
        }
    }
    if (sel.empty()) {
        if (!g->currentBrowseDir.empty()) {
            calculateAndShowFolderSize(g->hwnd, g->currentBrowseDir);
            return;
        }
        MessageBoxW(g->hwnd,
                    g->str == &kTr ? L"Lütfen boyutunu hesaplamak için bir klasör seçin." : L"Please select a folder to calculate size.",
                    g->str->title, MB_ICONINFORMATION);
        return;
    }
    const Row* r = sel[0];
    std::wstring folder = r->isDir ? r->path : r->dir;
    calculateAndShowFolderSize(g->hwnd, folder);
}

void trimRamWorkingSet() {
    DirPool::global().finishScan();
    EmptyWorkingSet(GetCurrentProcess());
    updateIndexStatus();
    MessageBoxW(g->hwnd, g->str->ramTrimmed, g->str->title, MB_ICONINFORMATION);
}

void updateBreadcrumb() {
    if (!g || !g->breadcrumb) return;
    if (!g->settings.showBreadcrumb || g->currentBrowseDir.empty()) {
        ShowWindow(g->breadcrumb, SW_HIDE);
        return;
    }
    ShowWindow(g->breadcrumb, SW_SHOW);
    std::wstring p = g->currentBrowseDir;
    std::wstring linkText = (g->str == &kTr ? L"Konum: " : L"Location: ");
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < p.size()) {
        size_t slash = p.find_first_of(L"\\/", start);
        if (slash == std::wstring::npos) {
            parts.push_back(p.substr(start));
            break;
        }
        parts.push_back(p.substr(start, slash - start));
        start = slash + 1;
    }
    std::wstring accumulated;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;
        if (accumulated.empty()) {
            accumulated = parts[i];
            if (accumulated.size() == 2 && accumulated[1] == L':') accumulated += L'\\';
        } else {
            if (accumulated.back() != L'\\') accumulated += L'\\';
            accumulated += parts[i];
        }
        if (i > 0) linkText += L" > ";
        linkText += L"<a href=\"" + accumulated + L"\">" + parts[i] + L"</a>";
    }
    SetWindowTextW(g->breadcrumb, linkText.c_str());
}

void updateHeaderSortArrow() {
    HWND hdr = ListView_GetHeader(g->list);
    if (!hdr) return;
    int count = Header_GetItemCount(hdr);
    for (int i = 0; i < count; ++i) {
        HDITEMW hdi = {};
        hdi.mask = HDI_FORMAT;
        if (Header_GetItem(hdr, i, &hdi)) {
            hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (i == g->sortCol) {
                hdi.fmt |= (g->sortAsc ? HDF_SORTUP : HDF_SORTDOWN);
            }
            Header_SetItem(hdr, i, &hdi);
        }
    }
}

// ------------------------------------------------------------------ Dark Mode
void applyDarkMode() {
    BOOL dark = g->settings.darkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(g->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DwmSetWindowAttribute(g->hwnd, 19, &dark, sizeof(dark));

    if (dark) {
        if (!g->darkEditBrush) g->darkEditBrush = CreateSolidBrush(g->darkEditBg);
        if (!g->darkWndBrush) g->darkWndBrush = CreateSolidBrush(g->darkWndBg);
        SetWindowTheme(g->list, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g->tabs, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g->comboScope, L"DarkMode_CFD", nullptr);
        if (g->breadcrumb) SetWindowTheme(g->breadcrumb, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(g->list, RGB(28, 28, 28));
        ListView_SetTextBkColor(g->list, RGB(28, 28, 28));
        ListView_SetTextColor(g->list, RGB(230, 230, 230));
    } else {
        SetWindowTheme(g->list, L"Explorer", nullptr);
        SetWindowTheme(g->tabs, nullptr, nullptr);
        SetWindowTheme(g->comboScope, nullptr, nullptr);
        if (g->breadcrumb) SetWindowTheme(g->breadcrumb, nullptr, nullptr);
        ListView_SetBkColor(g->list, GetSysColor(COLOR_WINDOW));
        ListView_SetTextBkColor(g->list, GetSysColor(COLOR_WINDOW));
        ListView_SetTextColor(g->list, GetSysColor(COLOR_WINDOWTEXT));
    }

    InvalidateRect(g->hwnd, nullptr, TRUE);
    if (g->btnCopy) {
        for (HWND b : {g->btnCopy, g->btnOpen, g->btnFolder, g->btnPreview, g->btnDelete, g->btnDetails, g->btnRefresh}) {
            InvalidateRect(b, nullptr, TRUE);
        }
    }
    if (g->breadcrumb) InvalidateRect(g->breadcrumb, nullptr, TRUE);
    InvalidateRect(g->comboScope, nullptr, TRUE);
    InvalidateRect(g->edit, nullptr, TRUE);
    InvalidateRect(g->tabs, nullptr, TRUE);
    InvalidateRect(g->list, nullptr, TRUE);
    InvalidateRect(g->status, nullptr, TRUE);
}

// ------------------------------------------------------------------ Menus & Settings
void setLanguage(const std::wstring& lang);

HMENU buildMenuBar() {
    const Strings& s = *g->str;
    HMENU bar = CreateMenu();

    // 1. File Menu
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, CMD_OPEN, s.open);
    AppendMenuW(file, MF_STRING, CMD_PREVIEW, s.preview);
    AppendMenuW(file, MF_STRING, CMD_FOLDER, s.folder);
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, CMD_SELECT_ALL, s.selectAll);
    AppendMenuW(file, MF_STRING, CMD_INVERT_SELECT, s.invertSelect);
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, CMD_COPYFILE, s.copyFile);
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

    // 2. Search Menu (History & Saved Searches)
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

    // 3. View Menu (Toolbar, Tabs, Breadcrumb, Scope Position, Dark Mode)
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING | (g->settings.showToolbar ? MF_CHECKED : 0), CMD_TOGGLE_TOOLBAR, s.toolbar);
    AppendMenuW(view, MF_STRING | (g->settings.showTabs ? MF_CHECKED : 0), CMD_TOGGLE_TABS, s.categoryTabs);
    AppendMenuW(view, MF_STRING | (g->settings.showBreadcrumb ? MF_CHECKED : 0), CMD_TOGGLE_BREADCRUMB, s.breadcrumbBar);
    AppendMenuW(view, MF_STRING | (g->settings.scopeOnRight ? MF_CHECKED : 0), CMD_TOGGLE_SCOPE_POS, s.scopeRight);
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING | (g->settings.darkMode ? MF_CHECKED : 0), CMD_DARK_MODE, s.darkMode);

    // 4. Tools Menu (Size Filter, Batch Rename, Hash, Folder Size, Memory Trim, Excludes, Language)
    HMENU toolsMenu = CreatePopupMenu();

    HMENU sizeMenu = CreatePopupMenu();
    AppendMenuW(sizeMenu, MF_STRING, CMD_SIZE_ALL, s.sizeAll);
    AppendMenuW(sizeMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sizeMenu, MF_STRING, CMD_SIZE_GIGANTIC, s.sizeGigantic);
    AppendMenuW(sizeMenu, MF_STRING, CMD_SIZE_HUGE, s.sizeHuge);
    AppendMenuW(sizeMenu, MF_STRING, CMD_SIZE_LARGE, s.sizeLarge);
    AppendMenuW(sizeMenu, MF_STRING, CMD_SIZE_MEDIUM, s.sizeMedium);
    AppendMenuW(sizeMenu, MF_STRING, CMD_SIZE_SMALL, s.sizeSmall);
    AppendMenuW(sizeMenu, MF_STRING, CMD_SIZE_TINY, s.sizeTiny);
    AppendMenuW(sizeMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sizeMenu, MF_STRING, CMD_SIZE_CUSTOM, s.sizeCustom);
    AppendMenuW(toolsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizeMenu), s.filterBySize);
    AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(toolsMenu, MF_STRING, CMD_BATCH_RENAME, s.batchRename);
    AppendMenuW(toolsMenu, MF_STRING, CMD_FILE_HASH, s.fileHash);
    AppendMenuW(toolsMenu, MF_STRING, CMD_FOLDER_SIZE, s.folderSize);
    AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(toolsMenu, MF_STRING, CMD_TRIM_RAM, s.trimRam);
    AppendMenuW(toolsMenu, MF_STRING, CMD_EXCLUDES, s.excludes);
    AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);

    HMENU lang = CreatePopupMenu();
    AppendMenuW(lang, MF_STRING | (g->str == &kEn ? MF_CHECKED : 0), CMD_LANG_EN, L"English");
    AppendMenuW(lang, MF_STRING | (g->str == &kTr ? MF_CHECKED : 0), CMD_LANG_TR, L"Türkçe");
    AppendMenuW(toolsMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(lang), s.language);

    // 5. Help Menu (About QBFind Plus)
    HMENU helpMenu = CreatePopupMenu();
    AppendMenuW(helpMenu, MF_STRING, CMD_ABOUT, s.about);

    // Assemble top bar
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), s.file);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(searchMenu), s.search);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), s.view);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(toolsMenu), s.tools);
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), s.help);
    return bar;
}

void createMenuBar() {
    HMENU old = GetMenu(g->hwnd);
    SetMenu(g->hwnd, buildMenuBar());
    if (old) DestroyMenu(old);
    DrawMenuBar(g->hwnd);
}

void setupCategoryTabs() {
    if (!g || !g->tabs) return;
    TabCtrl_DeleteAllItems(g->tabs);
    bool tr = (g->str == &kTr);
    const wchar_t* tabTitlesTr[] = {
        L"Tümü", L"Belgeler", L"Resimler", L"Müzik", L"Videolar", L"Arşivler", L"Uygulamalar", L"Klasörler"
    };
    const wchar_t* tabTitlesEn[] = {
        L"All", L"Documents", L"Images", L"Audio", L"Videos", L"Archives", L"Apps", L"Folders"
    };
    const wchar_t** titles = tr ? tabTitlesTr : tabTitlesEn;
    for (int i = 0; i < CAT_COUNT; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(titles[i]);
        TabCtrl_InsertItem(g->tabs, i, &item);
    }
    TabCtrl_SetCurSel(g->tabs, static_cast<int>(g->category));
}

void setupScopeCombo() {
    if (!g || !g->comboScope) return;
    SendMessageW(g->comboScope, CB_RESETCONTENT, 0, 0);
    bool tr = (g->str == &kTr);
    const wchar_t* scopeTitlesTr[] = {
        L"Tüm Bilgisayar",
        L"Masaüstü",
        L"İndirilenler",
        L"Belgeler",
        L"Kullanıcı Alanı",
        L"Resimler",
        L"Müzik",
        L"Videolar",
        L"Yerel Disk (C:)",
        L"Yerel Disk (D:)"
    };
    const wchar_t* scopeTitlesEn[] = {
        L"All Computer",
        L"Desktop",
        L"Downloads",
        L"Documents",
        L"User Area",
        L"Pictures",
        L"Music",
        L"Videos",
        L"Drive (C:)",
        L"Drive (D:)"
    };
    const wchar_t** titles = tr ? scopeTitlesTr : scopeTitlesEn;
    for (int i = 0; i < SCOPE_COUNT; ++i) {
        SendMessageW(g->comboScope, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(titles[i]));
    }
    SendMessageW(g->comboScope, CB_SETCURSEL, static_cast<WPARAM>(g->scope), 0);
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
    if (g->btnCopy) SetWindowTextW(g->btnCopy, s.btnCopy);
    if (g->btnOpen) SetWindowTextW(g->btnOpen, s.btnOpen);
    if (g->btnFolder) SetWindowTextW(g->btnFolder, s.btnFolder);
    if (g->btnPreview) SetWindowTextW(g->btnPreview, s.btnPreview);
    if (g->btnDelete) SetWindowTextW(g->btnDelete, s.btnDelete);
    if (g->btnDetails) SetWindowTextW(g->btnDetails, s.btnDetails);
    if (g->btnRefresh) SetWindowTextW(g->btnRefresh, s.btnRefresh);
    setupCategoryTabs();
    setupScopeCombo();
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
    UINT dpi = GetDpiForWindow(g->hwnd);
    int pad = MulDiv(6, dpi, 96);
    int btnH = MulDiv(28, dpi, 96);
    int tabH = MulDiv(26, dpi, 96);

    TEXTMETRICW tm;
    HDC dc = GetDC(g->hwnd);
    HGDIOBJ old = SelectObject(dc, g->font);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g->hwnd, dc);

    int editH = tm.tmHeight + pad + 6;
    int statusH = tm.tmHeight + pad + 2;

    int curX = pad;
    int curY = pad;

    // 1. Action Toolbar (Optional)
    HWND btns[] = { g->btnCopy, g->btnOpen, g->btnFolder, g->btnPreview, g->btnDelete, g->btnDetails, g->btnRefresh };
    int btnCount = sizeof(btns) / sizeof(btns[0]);
    if (g->settings.showToolbar) {
        int availW = std::max(10, (int)rc.right - 2 * pad);
        int spacing = MulDiv(3, dpi, 96);
        int totalSpacing = spacing * (btnCount - 1);
        int btnW = std::max(30, (availW - totalSpacing) / btnCount);

        for (int i = 0; i < btnCount; ++i) {
            int w = (i == btnCount - 1) ? (pad + availW - curX) : btnW;
            if (btns[i]) {
                ShowWindow(btns[i], SW_SHOW);
                MoveWindow(btns[i], curX, curY, w, btnH, TRUE);
            }
            curX += w + spacing;
        }
        curY += btnH + pad;
    } else {
        for (HWND b : btns) if (b) ShowWindow(b, SW_HIDE);
    }

    // 2. Search Row: Location Selector + Up Button + Search Input
    int comboW = MulDiv(140, dpi, 96);
    int upW = MulDiv(32, dpi, 96);
    int spacing = MulDiv(4, dpi, 96);
    int editW = std::max(80, (int)rc.right - 2 * pad - comboW - upW - 2 * spacing);

    if (g->settings.scopeOnRight) {
        curX = pad;
        if (g->edit) MoveWindow(g->edit, curX, curY, editW, editH, TRUE);
        curX += editW + spacing;
        if (g->btnUp) MoveWindow(g->btnUp, curX, curY, upW, editH, TRUE);
        curX += upW + spacing;
        if (g->comboScope) MoveWindow(g->comboScope, curX, curY, comboW, editH * 10, TRUE);
    } else {
        curX = pad;
        if (g->comboScope) MoveWindow(g->comboScope, curX, curY, comboW, editH * 10, TRUE);
        curX += comboW + spacing;
        if (g->btnUp) MoveWindow(g->btnUp, curX, curY, upW, editH, TRUE);
        curX += upW + spacing;
        if (g->edit) MoveWindow(g->edit, curX, curY, editW, editH, TRUE);
    }
    curY += editH + pad / 2;

    // 2.5 Address Breadcrumb Bar (Optional)
    if (g->settings.showBreadcrumb && !g->currentBrowseDir.empty() && g->breadcrumb) {
        ShowWindow(g->breadcrumb, SW_SHOW);
        int breadcrumbH = tm.tmHeight + pad / 2;
        MoveWindow(g->breadcrumb, pad, curY, rc.right - 2 * pad, breadcrumbH, TRUE);
        curY += breadcrumbH + pad / 2;
    } else if (g->breadcrumb) {
        ShowWindow(g->breadcrumb, SW_HIDE);
    }

    // 3. Category Filter Tabs Row (Optional)
    if (g->settings.showTabs) {
        if (g->tabs) {
            ShowWindow(g->tabs, SW_SHOW);
            MoveWindow(g->tabs, pad, curY, rc.right - 2 * pad, tabH, TRUE);
        }
        curY += tabH + pad / 2;
    } else {
        if (g->tabs) ShowWindow(g->tabs, SW_HIDE);
    }

    // 4. Results List View
    int listTop = curY;
    int listH = std::max<int>(0, rc.bottom - listTop - statusH - pad / 2);
    if (g->list) MoveWindow(g->list, 0, listTop, rc.right, listH, TRUE);

    // 5. Status Bar
    if (g->status) MoveWindow(g->status, pad, rc.bottom - statusH, rc.right - 2 * pad, statusH, TRUE);

    // Column proportional sizing with guaranteed minimums
    if (g->list) {
        int total = rc.right - GetSystemMetrics(SM_CXVSCROLL) - 4;
        int wMod = MulDiv(145, dpi, 96); // Guaranteed space for "YYYY-MM-DD HH:MM"
        int wSize = MulDiv(85, dpi, 96);  // Guaranteed space for "999.9 MB" / "<klasör>"
        int rem = std::max(100, total - wMod - wSize);
        int wName = rem * 45 / 100;
        int wPath = std::max(50, rem - wName);

        ListView_SetColumnWidth(g->list, 0, wName);
        ListView_SetColumnWidth(g->list, 2, wSize);
        ListView_SetColumnWidth(g->list, 3, wMod);
        ListView_SetColumnWidth(g->list, 1, wPath);
    }
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
    if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, EditSub, 1);
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK ListSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (w == VK_RETURN) {
            if (alt) propertiesSelected();
            else openSelected();
            return 0;
        }
        if (w == VK_SPACE) { previewSelected(); return 0; }
        if (w == VK_DELETE) { deleteSelected(); return 0; }
        if (w == VK_ESCAPE) { SetFocus(g->edit); return 0; }
        if (w == VK_BACK && editText().empty()) { navigateUp(); return 0; }
        if (alt && w == VK_UP && editText().empty()) { navigateUp(); return 0; }
        if (ctrl && w == 'A') { selectAll(); return 0; }
        if (ctrl && !shift && w == 'C') { oneClickCopy(); return 0; }
        if (ctrl && shift && w == 'C') { copyPathSelected(); return 0; }
        if (ctrl && w == 'X') { cutSelected(); return 0; }
        if (ctrl && w == 'S') { toggleSavedSearch(editText()); createMenuBar(); return 0; }
        if (ctrl && w == 'F') { SetFocus(g->edit); SendMessageW(g->edit, EM_SETSEL, 0, -1); return 0; }
        if (ctrl && w == 'R') { batchRenameSelected(); return 0; }
        if (ctrl && w == 'H') { fileHashSelected(); return 0; }
        if (ctrl && w == 'K') { folderSizeSelected(); return 0; }
    }
    if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, ListSub, 1);
    }
    return DefSubclassProc(h, m, w, l);
}

void showContextMenu(POINT pt) {
    auto sel = selectedRows();
    const Strings& s = *g->str;
    HMENU menu = CreatePopupMenu();
    if (!sel.empty()) {
        AppendMenuW(menu, MF_STRING, CMD_OPEN, s.open);
        AppendMenuW(menu, MF_STRING, CMD_PREVIEW, s.preview);
        AppendMenuW(menu, MF_STRING, CMD_FOLDER, s.folder);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CMD_BATCH_RENAME, s.batchRename);
        AppendMenuW(menu, MF_STRING, CMD_FILE_HASH, s.fileHash);
        AppendMenuW(menu, MF_STRING, CMD_FOLDER_SIZE, s.folderSize);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CMD_COPYFILE, s.copyFile);
        AppendMenuW(menu, MF_STRING, CMD_COPYPATH, s.copyPath);
        AppendMenuW(menu, MF_STRING, CMD_COPYNAME, s.copyName);
        AppendMenuW(menu, MF_STRING, CMD_CUT, s.cut);
        AppendMenuW(menu, MF_STRING, CMD_DESKTOP, s.desktop);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CMD_DELETE, s.del);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CMD_PROPERTIES, s.properties);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, CMD_SELECT_ALL, s.selectAll);
    AppendMenuW(menu, MF_STRING, CMD_INVERT_SELECT, s.invertSelect);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_REFRESH, s.refresh);
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
    if (g->breadcrumb && nh->hwndFrom == g->breadcrumb) {
        if (nh->code == NM_CLICK || nh->code == NM_RETURN) {
            auto* pLink = reinterpret_cast<PNMLINK>(lParam);
            std::wstring target = pLink->item.szUrl;
            if (!target.empty()) {
                g->currentBrowseDir = target;
                updateBreadcrumb();
                layout();
                runSearch();
            }
            return 0;
        }
    }
    if (g->tabs && nh->hwndFrom == g->tabs) {
        if (nh->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(g->tabs);
            if (sel >= 0 && sel < CAT_COUNT) {
                g->category = static_cast<FileCategory>(sel);
                runSearch();
            }
            return 0;
        }
    }
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
        case LVN_ITEMCHANGED: {
            auto* nmlv = reinterpret_cast<NMLISTVIEW*>(nh);
            if ((nmlv->uChanged & LVIF_STATE) && ((nmlv->uNewState ^ nmlv->uOldState) & LVIS_SELECTED)) {
                int selCount = ListView_GetSelectedCount(g->list);
                if (selCount > 1) {
                    wchar_t selBuf[128];
                    if (g->str == &kTr) {
                        swprintf_s(selBuf, L"  |  %d öğe seçildi", selCount);
                    } else {
                        swprintf_s(selBuf, L"  |  %d items selected", selCount);
                    }
                    std::wstring ramStr = L"  |  RAM: " + std::to_wstring(getRamUsageMb()) + L" MB";
                    if (g->lastQuery.size() >= 2) {
                        setStatus(fmtStatus(g->str->statusResults, formatInt(static_cast<int64_t>(g->rows.size()), L','),
                                            formatInt(g->indexer.count(), L','), scanSuffix()) + selBuf + ramStr);
                    } else {
                        setStatus(fmtStatus(g->str->statusIndex, formatInt(g->indexer.count(), L','), scanSuffix(), L"") + selBuf + ramStr);
                    }
                }
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
            updateHeaderSortArrow();
            ListView_SetItemState(g->list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            InvalidateRect(g->list, nullptr, TRUE);
            break;
        }
        case NM_DBLCLK: {
            auto* a = reinterpret_cast<NMITEMACTIVATE*>(nh);
            if (a->iItem >= 0 && static_cast<size_t>(a->iItem) < g->rank.size()) {
                const Row& r = g->rows[g->rank[a->iItem]];
                if (r.isDir && editText().empty()) {
                    g->currentBrowseDir = r.path;
                    updateBreadcrumb();
                    layout();
                    runSearch();
                } else {
                    openRow(r);
                }
            }
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
                UINT dpi = GetDpiForWindow(h);
                int hFont = -MulDiv(9, dpi ? dpi : 96, 72);
                g->font = CreateFontW(hFont, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

                // 1. 7-Zip Style Modern Action Toolbar Buttons
                g->btnCopy = CreateWindowExW(0, L"BUTTON", g->str->btnCopy, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                             0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_COPY)), g->inst, nullptr);
                g->btnOpen = CreateWindowExW(0, L"BUTTON", g->str->btnOpen, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                             0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_OPEN)), g->inst, nullptr);
                g->btnFolder = CreateWindowExW(0, L"BUTTON", g->str->btnFolder, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                               0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_FOLDER)), g->inst, nullptr);
                g->btnPreview = CreateWindowExW(0, L"BUTTON", g->str->btnPreview, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_PREVIEW)), g->inst, nullptr);
                g->btnDelete = CreateWindowExW(0, L"BUTTON", g->str->btnDelete, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                               0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_DELETE)), g->inst, nullptr);
                g->btnDetails = CreateWindowExW(0, L"BUTTON", g->str->btnDetails, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_DETAILS)), g->inst, nullptr);
                g->btnRefresh = CreateWindowExW(0, L"BUTTON", g->str->btnRefresh, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_REFRESH)), g->inst, nullptr);

                // 2. Search Controls: Location Scope Selector, Up Button & Search Input
                g->comboScope = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                                0, 0, 10, 10, h,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_COMBO_SCOPE)), g->inst, nullptr);

                g->btnUp = CreateWindowExW(0, L"BUTTON", L"..", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_UP)), g->inst, nullptr);

                g->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, h,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT)), g->inst, nullptr);

                // 3. Category Filter Tabs
                g->tabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_BUTTONS | TCS_FLATBUTTONS,
                                          0, 0, 10, 10, h, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TABS)), g->inst, nullptr);

                // 4. Results List View
                g->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS, 0, 0, 10, 10, h,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)), g->inst, nullptr);

                // 4.5 Address Breadcrumb Bar (SysLink)
                g->breadcrumb = CreateWindowExW(0, WC_LINK, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, h,
                                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BREADCRUMB)), g->inst, nullptr);

                // 5. Status Bar
                g->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE, 0, 0, 10, 10, h,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), g->inst, nullptr);

                for (HWND c : {g->btnCopy, g->btnOpen, g->btnFolder, g->btnPreview, g->btnDelete, g->btnDetails, g->btnRefresh, g->tabs, g->comboScope, g->btnUp, g->edit, g->breadcrumb, g->list, g->status}) {
                    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g->font), TRUE);
                }
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
                updateBreadcrumb();
                layout();
                SetTimer(h, TIMER_STATUS, 400, nullptr);
                SetFocus(g->edit);

                g->indexer.setExcludes(g->settings.excludes);
                g->indexer.onChanged = [] {
                    if (g && g->hwnd) PostMessageW(g->hwnd, WM_INDEX_CHANGED, 0, 0);
                };
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
                int hFont = -MulDiv(9, HIWORD(w), 72);
                HFONT nf = CreateFontW(hFont, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                for (HWND c : {g->btnCopy, g->btnOpen, g->btnFolder, g->btnPreview, g->btnDelete, g->btnDetails, g->btnRefresh, g->tabs, g->comboScope, g->btnUp, g->edit, g->breadcrumb, g->list, g->status}) {
                    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(nf), TRUE);
                }
                DeleteObject(g->font);
                g->font = nf;
                layout();
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(l);
                mm->ptMinTrackSize = {420, 380};
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
            case WM_INDEX_CHANGED:
                if (g->lastQuery.size() >= 2) runSearch();
                else updateIndexStatus();
                return 0;
            case WM_SEARCH_DONE:
                applyResults();
                return 0;
            case WM_CTLCOLOREDIT: {
                HDC hdc = reinterpret_cast<HDC>(w);
                if (g && g->settings.darkMode && g->darkEditBrush) {
                    SetTextColor(hdc, g->darkEditText);
                    SetBkColor(hdc, g->darkEditBg);
                    return reinterpret_cast<LRESULT>(g->darkEditBrush);
                }
                break;
            }
            case WM_CTLCOLORBTN: {
                HDC hdc = reinterpret_cast<HDC>(w);
                if (g && g->settings.darkMode && g->darkWndBrush) {
                    SetTextColor(hdc, g->darkEditText);
                    SetBkColor(hdc, g->darkWndBg);
                    return reinterpret_cast<LRESULT>(g->darkWndBrush);
                }
                break;
            }
            case WM_CTLCOLORSTATIC: {
                HDC hdc = reinterpret_cast<HDC>(w);
                HWND ctl = reinterpret_cast<HWND>(l);
                if (g && g->settings.darkMode && g->darkWndBrush) {
                    SetTextColor(hdc, (ctl == g->status) ? g->darkStatusText : g->darkEditText);
                    SetBkColor(hdc, g->darkWndBg);
                    return reinterpret_cast<LRESULT>(g->darkWndBrush);
                }
                break;
            }
            case WM_ERASEBKGND: {
                if (g && g->settings.darkMode && g->darkWndBrush) {
                    HDC hdc = reinterpret_cast<HDC>(w);
                    RECT rc;
                    GetClientRect(h, &rc);
                    FillRect(hdc, &rc, g->darkWndBrush);
                    return 1;
                }
                break;
            }
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
                if (id == ID_COMBO_SCOPE && HIWORD(w) == CBN_SELCHANGE) {
                    int sel = static_cast<int>(SendMessageW(g->comboScope, CB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < SCOPE_COUNT) {
                        g->scope = static_cast<ScopeType>(sel);
                        const auto& sp = getScopePaths();
                        switch (g->scope) {
                            case SCOPE_ALL: g->currentBrowseDir.clear(); break;
                            case SCOPE_DESKTOP: g->currentBrowseDir = sp.desktopOriginal; break;
                            case SCOPE_DOWNLOADS: g->currentBrowseDir = sp.downloadsOriginal; break;
                            case SCOPE_DOCUMENTS: g->currentBrowseDir = sp.documentsOriginal; break;
                            case SCOPE_USER_AREA: g->currentBrowseDir = sp.userProfileOriginal; break;
                            case SCOPE_PICTURES: g->currentBrowseDir = sp.picturesOriginal; break;
                            case SCOPE_MUSIC: g->currentBrowseDir = sp.musicOriginal; break;
                            case SCOPE_VIDEOS: g->currentBrowseDir = sp.videosOriginal; break;
                            case SCOPE_DRIVE_C: g->currentBrowseDir = L"C:\\"; break;
                            case SCOPE_DRIVE_D: g->currentBrowseDir = L"D:\\"; break;
                            default: g->currentBrowseDir.clear(); break;
                        }
                        updateBreadcrumb();
                        layout();
                        runSearch();
                    }
                    return 0;
                }
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
                    case ID_BTN_COPY: oneClickCopy(); break;
                    case ID_BTN_OPEN: openSelected(); break;
                    case ID_BTN_FOLDER: { auto s = selectedRows(); if (!s.empty()) showInFolder(*s[0]); break; }
                    case ID_BTN_PREVIEW: previewSelected(); break;
                    case ID_BTN_DELETE: deleteSelected(); break;
                    case ID_BTN_DETAILS: propertiesSelected(); break;
                    case ID_BTN_REFRESH: g->indexer.rescan(); break;
                    case ID_BTN_UP: navigateUp(); break;
                    case CMD_TOGGLE_TOOLBAR:
                        g->settings.showToolbar = !g->settings.showToolbar;
                        saveSettings(g->settings);
                        createMenuBar();
                        layout();
                        break;
                    case CMD_TOGGLE_TABS:
                        g->settings.showTabs = !g->settings.showTabs;
                        saveSettings(g->settings);
                        createMenuBar();
                        layout();
                        break;
                    case CMD_TOGGLE_BREADCRUMB:
                        g->settings.showBreadcrumb = !g->settings.showBreadcrumb;
                        saveSettings(g->settings);
                        createMenuBar();
                        updateBreadcrumb();
                        layout();
                        break;
                    case CMD_TOGGLE_SCOPE_POS:
                        g->settings.scopeOnRight = !g->settings.scopeOnRight;
                        saveSettings(g->settings);
                        createMenuBar();
                        layout();
                        break;
                    case CMD_TRIM_RAM:
                        trimRamWorkingSet();
                        break;
                    case CMD_BATCH_RENAME:
                        batchRenameSelected();
                        break;
                    case CMD_FILE_HASH:
                        fileHashSelected();
                        break;
                    case CMD_FOLDER_SIZE:
                        folderSizeSelected();
                        break;

                    case CMD_OPEN: openSelected(); break;
                    case CMD_PREVIEW: previewSelected(); break;
                    case CMD_FOLDER: { auto s = selectedRows(); if (!s.empty()) showInFolder(*s[0]); break; }
                    case CMD_SELECT_ALL: selectAll(); break;
                    case CMD_INVERT_SELECT: invertSelection(); break;
                    case CMD_COPYFILE: oneClickCopy(); break;
                    case CMD_COPYPATH: copyPathSelected(); break;
                    case CMD_COPYNAME: copyNameSelected(); break;
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
                    case CMD_ABOUT:
                        showAboutDialog(h, g->str == &kTr);
                        break;
                    case CMD_SIZE_ALL: applySizeFilter(L""); break;
                    case CMD_SIZE_GIGANTIC: applySizeFilter(L">1gb"); break;
                    case CMD_SIZE_HUGE: applySizeFilter(L"100mb-1gb"); break;
                    case CMD_SIZE_LARGE: applySizeFilter(L"16mb-100mb"); break;
                    case CMD_SIZE_MEDIUM: applySizeFilter(L"1mb-16mb"); break;
                    case CMD_SIZE_SMALL: applySizeFilter(L"<1mb"); break;
                    case CMD_SIZE_TINY: applySizeFilter(L"<100kb"); break;
                    case CMD_SIZE_CUSTOM: {
                        std::wstring f;
                        if (showCustomSizeDialog(h, f, g->str == &kTr)) {
                            applySizeFilter(f);
                        }
                        break;
                    }
                    case CMD_EXIT: DestroyWindow(h); break;
                }
                return 0;
            }
            case WM_KEYDOWN:
                if (w == VK_F5) { g->indexer.rescan(); return 0; }
                if (w == VK_F1) { showAboutDialog(h, g->str == &kTr); return 0; }
                break;
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            case WM_DESTROY:
                KillTimer(h, TIMER_STATUS);
                if (g->darkEditBrush) { DeleteObject(g->darkEditBrush); g->darkEditBrush = nullptr; }
                if (g->darkWndBrush) { DeleteObject(g->darkWndBrush); g->darkWndBrush = nullptr; }
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
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_LINK_CLASS | ICC_PROGRESS_CLASS;
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

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, g->str->title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 600, 600, nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (msg.wParam == VK_F5) {
                g->indexer.rescan();
                continue;
            }
            if (msg.wParam == VK_F1) {
                showAboutDialog(g->hwnd, g->str == &kTr);
                continue;
            }
            if ((ctrl && msg.wParam == 'F') || msg.wParam == VK_F3) {
                SetFocus(g->edit);
                SendMessageW(g->edit, EM_SETSEL, 0, -1);
                continue;
            }
            if (ctrl && msg.wParam == 'S') {
                toggleSavedSearch(editText());
                createMenuBar();
                continue;
            }
            if (ctrl && msg.wParam == 'R') {
                batchRenameSelected();
                continue;
            }
            if (ctrl && msg.wParam == 'H') {
                fileHashSelected();
                continue;
            }
            if (ctrl && msg.wParam == 'K') {
                folderSizeSelected();
                continue;
            }
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
