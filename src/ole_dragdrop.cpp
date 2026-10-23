#include "ole_dragdrop.h"

#include <shlobj.h>
#include <atomic>
#include <cstring>

namespace qb {

namespace {

UINT getPreferredDropEffectFormat() {
    static UINT cf = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return cf;
}

// ------------------------------------------------------------- IEnumFORMATETC
class FormatEtcEnum : public IEnumFORMATETC {
public:
    explicit FormatEtcEnum(const std::vector<FORMATETC>& formats, size_t index = 0)
        : formats_(formats), index_(index), ref_(1) {}
    virtual ~FormatEtcEnum() = default;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *ppv = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++ref_;
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }

    // IEnumFORMATETC
    STDMETHODIMP Next(ULONG celt, FORMATETC* rgelt, ULONG* pceltFetched) override {
        if (celt > 0 && !rgelt) return E_INVALIDARG;
        if (celt > 1 && !pceltFetched) return E_INVALIDARG;
        ULONG fetched = 0;
        while (index_ < formats_.size() && fetched < celt) {
            rgelt[fetched] = formats_[index_++];
            rgelt[fetched].ptd = nullptr;
            ++fetched;
        }
        if (pceltFetched) *pceltFetched = fetched;
        return (fetched == celt) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Skip(ULONG celt) override {
        index_ += celt;
        if (index_ > formats_.size()) index_ = formats_.size();
        return (index_ < formats_.size()) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumFORMATETC** ppenum) override {
        if (!ppenum) return E_POINTER;
        *ppenum = new FormatEtcEnum(formats_, index_);
        return S_OK;
    }

private:
    std::vector<FORMATETC> formats_;
    size_t index_;
    std::atomic<ULONG> ref_;
};

// ------------------------------------------------------------- DropSource
class DropSource : public IDropSource {
public:
    DropSource() : ref_(1) {}
    virtual ~DropSource() = default;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++ref_;
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }

    // IDropSource
    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed) return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & (MK_LBUTTON | MK_RBUTTON))) {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    STDMETHODIMP GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<ULONG> ref_;
};

// ------------------------------------------------------------- DataObject
class DataObject : public IDataObject {
public:
    explicit DataObject(std::vector<std::wstring> files)
        : files_(std::move(files)), ref_(1) {}
    virtual ~DataObject() = default;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++ref_;
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --ref_;
        if (n == 0) delete this;
        return n;
    }

    // IDataObject
    STDMETHODIMP GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override {
        if (!pformatetcIn || !pmedium) return E_INVALIDARG;
        pmedium->tymed = TYMED_NULL;
        pmedium->pUnkForRelease = nullptr;
        pmedium->hGlobal = nullptr;

        if (pformatetcIn->cfFormat == CF_HDROP && (pformatetcIn->tymed & TYMED_HGLOBAL)) {
            // Calculate buffer size for DROPFILES + double-null terminated strings
            size_t totalChars = 0;
            for (const auto& f : files_) totalChars += f.size() + 1;
            totalChars += 1; // extra terminating null

            size_t totalBytes = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GHND | GMEM_SHARE, totalBytes);
            if (!hMem) return E_OUTOFMEMORY;

            auto* pData = static_cast<char*>(GlobalLock(hMem));
            if (!pData) {
                GlobalFree(hMem);
                return E_OUTOFMEMORY;
            }

            auto* df = reinterpret_cast<DROPFILES*>(pData);
            df->pFiles = sizeof(DROPFILES);
            df->pt.x = 0;
            df->pt.y = 0;
            df->fNC = FALSE;
            df->fWide = TRUE;

            wchar_t* pDest = reinterpret_cast<wchar_t*>(pData + sizeof(DROPFILES));
            for (const auto& f : files_) {
                std::memcpy(pDest, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
                pDest += f.size() + 1;
            }
            *pDest = L'\0'; // trailing null
            GlobalUnlock(hMem);

            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hMem;
            return S_OK;
        }

        UINT cfPref = getPreferredDropEffectFormat();
        if (cfPref && pformatetcIn->cfFormat == cfPref && (pformatetcIn->tymed & TYMED_HGLOBAL)) {
            HGLOBAL hMem = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DWORD));
            if (!hMem) return E_OUTOFMEMORY;
            auto* p = static_cast<DWORD*>(GlobalLock(hMem));
            if (!p) {
                GlobalFree(hMem);
                return E_OUTOFMEMORY;
            }
            *p = DROPEFFECT_COPY;
            GlobalUnlock(hMem);

            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hMem;
            return S_OK;
        }

        return DV_E_FORMATETC;
    }

    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }

    STDMETHODIMP QueryGetData(FORMATETC* pformatetc) override {
        if (!pformatetc) return E_INVALIDARG;
        if (pformatetc->cfFormat == CF_HDROP && (pformatetc->tymed & TYMED_HGLOBAL)) {
            return S_OK;
        }
        UINT cfPref = getPreferredDropEffectFormat();
        if (cfPref && pformatetc->cfFormat == cfPref && (pformatetc->tymed & TYMED_HGLOBAL)) {
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* pFormatetcOut) override {
        if (!pFormatetcOut) return E_INVALIDARG;
        pFormatetcOut->ptd = nullptr;
        return E_NOTIMPL;
    }

    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }

    STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override {
        if (!ppenumFormatEtc) return E_POINTER;
        *ppenumFormatEtc = nullptr;
        if (dwDirection != DATADIR_GET) return E_NOTIMPL;

        std::vector<FORMATETC> formats;
        FORMATETC f1 = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        formats.push_back(f1);

        UINT cfPref = getPreferredDropEffectFormat();
        if (cfPref) {
            FORMATETC f2 = {static_cast<CLIPFORMAT>(cfPref), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            formats.push_back(f2);
        }

        *ppenumFormatEtc = new FormatEtcEnum(formats);
        return S_OK;
    }

    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    std::vector<std::wstring> files_;
    std::atomic<ULONG> ref_;
};

}  // namespace

bool doDragDropFiles(HWND hwnd, const std::vector<std::wstring>& filePaths) {
    (void)hwnd;
    if (filePaths.empty()) return false;

    auto* pData = new DataObject(filePaths);
    auto* pSource = new DropSource();

    DWORD dwEffect = 0;
    HRESULT hr = DoDragDrop(pData, pSource, DROPEFFECT_COPY | DROPEFFECT_MOVE, &dwEffect);

    pData->Release();
    pSource->Release();

    return (hr == DRAGDROP_S_DROP);
}

}  // namespace qb
