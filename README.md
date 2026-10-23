# QBFind+ (C++ Edition)

[![CI](https://github.com/tommyvercetti89/qbfindplus/actions/workflows/ci.yml/badge.svg)](https://github.com/tommyvercetti89/qbfindplus/actions/workflows/ci.yml)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows_10_|_11-0078D6?logo=windows&logoColor=white)](https://microsoft.com)
[![Language: C++20](https://img.shields.io/badge/Language-C%2B%2B20-00599C?logo=c%2B%2B&logoColor=white)](https://isocpp.org)
[![CMake: 3.16+](https://img.shields.io/badge/CMake-3.16+-064F8C)](https://cmake.org)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

High-performance native Windows desktop file search engine engineered with modern C++20 and pure Win32/COM APIs. Designed for instant query execution, low memory overhead, and rock-solid system stability across millions of indexed files.

---

## Technical Overview

QBFind+ represents a complete ground-up rewrite of the original Go file finder, eliminating Garbage Collector (GC) runtime pauses, heavy memory duplication, and unmanaged crash scenarios.

### Architecture Comparison

| Architectural Metric | Go Implementation | QBFind+ C++ Engine |
| :--- | :--- | :--- |
| **Runtime Model** | Go Runtime + GC pauses | Deterministic C++20 RAII, Zero GC overhead |
| **Directory Watcher Updates** | Clones entire index vector on updates | In-place `std::remove_if` with sub-tree prefix pruning (O(1) allocation) |
| **Index Cache Streaming** | High-allocation in-memory buffers | 64 KB chunked streaming I/O with compact UTF-8 paths |
| **Regular Expression Engine** | Standard RE2 | `std::wregex` with ReDoS heuristics, pattern length limits and exception traps |
| **Crash & Fault Handling** | Process panics caught only in Go space | Native `SetUnhandledExceptionFilter` generating DbgHelp minidumps |
| **Binary Output** | Runtime-dependent executable | Standalone statically linked native Windows PE binary (~4.2 MB) |

---

## Core Capabilities

- **Multi-Threaded Drive Indexing:** Dynamic worker pool scaled to CPU topology (`Workers = min(max(NumCPU * 2, 2), 16)`). User folders (`Desktop`, `Downloads`, `Documents`, etc.) are prioritized first for near-instant search availability.
- **Rich Query Language:**
  - Extensions: `ext:pdf`, `.pdf`, `*.pdf`, `ext:pdf|docx`
  - Name filter: `name:report`
  - Path filter: `path:finance`, `folder:records`
  - Size ranges: `size:>10mb`, `size:<500kb`, `size:1mb-10mb`
  - Date intervals: `date:today`, `date:yesterday`, `date:week`, `date:>2024-01-01`
  - Regular expressions: `regex:^report_\d{4}`, `pathregex:.*`
  - Glob & fuzzy: `rep*.xlsx`, `~quarterly`
  - Alternates: `pdf|docx|xlsx`
- **Turkish Character Folding:** Strict 1:1 case-folding support for Turkish glyphs (`ı, İ, ç, ğ, ö, ş, ü` to `i, i, c, g, o, s, u`).
- **COM / OLE Drag and Drop:** Full `IDataObject` and `IDropSource` native COM implementation supporting direct dragging of results to Explorer, external software, or browser drop targets (`CF_HDROP`).
- **In-App Text & Code Preview:** Rapid modal preview supporting UTF-8, UTF-16LE, and UTF-16BE (with BOM detection), JSON formatting, and a 4,000 character safety buffer.
- **File System Operations:**
  - Send to Recycle Bin (`SHFileOperationW` with `FO_DELETE` and `FOF_ALLOWUNDO`)
  - Copy and Cut to Clipboard (`CF_HDROP` with `DROPEFFECT_MOVE`)
  - Copy to Desktop (`SHFileOperationW` with `FO_COPY`)
  - Shell Properties Dialog (`SEE_MASK_INVOKEIDLIST`)
- **Search History & Favorites:** Persistent tracking under `%APPDATA%\QBFind` for recent and saved searches.
- **DirPool Virtualization & Ultra-Low RAM Footprint:** Interned directory string pools deduplicating paths in RAM (~40 MB RAM for 1.8M indexed files, down from 500+ MB).
- **Batch File Renamer (Ctrl+R):** Interactive dialog with live name previews, prefix/suffix additions, and find/replace matching.
- **Cryptographic File Integrity Engine (Ctrl+H):** Hardware-accelerated Windows CNG SHA-256 and MD5 stream hashing with background worker thread, progress bar, and instant hash match verification.
- **True Folder Size Calculator (Ctrl+K):** Recursive directory size accumulator counting exact bytes, files, and subfolders.
- **Address Breadcrumb Bar & Quick Navigation:** Interactive path navigation and location scopes (Desktop, Downloads, Documents, Drives).
- **Multi-File Selection (Ctrl+A):** Full multi-selection support with Select All, Invert Selection, and live selected item counter.
- **Working Set RAM Purge:** Release unneeded working set memory back to the OS on demand.
- **Modern Clean Windows Typography:** Professional ClearType Segoe UI styling with zero emoji bloat across all windows and dialogs.

---

## Repository Structure

```text
qbfindplus/
|-- .github/workflows/
|   `-- ci.yml              # Automated GitHub Actions Windows CI pipeline
|-- src/
|   |-- core.h / .cpp       # DirPool, string transformations, settings, history, crash reporter
|   |-- search.h / .cpp     # Multi-threaded query parser, matching rules, Turkish aliases
|   |-- indexer.h / .cpp    # Parallel directory crawler, streaming cache & directory watcher
|   |-- ole_dragdrop.h/.cpp # Native COM IDataObject / IDropSource implementation
|   |-- actions.h / .cpp    # Preview, batch renamer, hash verifier, folder size, about dialog
|   |-- main.cpp            # Win32 message loop, toolbar, scopes, breadcrumb & virtual ListView
|   |-- app.manifest        # DPI per-monitor v2 and LongPath aware application manifest
|   `-- app.rc              # Windows resource definition
|-- tests/
|   `-- search_test.cpp     # Deterministic query engine, parser & safety verification suite
|-- CMakeLists.txt          # Root build configuration
|-- .gitignore              # Repository artifact exclusion
`-- LICENSE                 # MIT License
```

---

## Building from Source

### Prerequisites
- CMake 3.16 or higher
- C++20 compliant compiler:
  - MSVC (Visual Studio 2022 / Build Tools) **OR**
  - MinGW-w64 (GCC 12.0+)

### Building with MinGW (PowerShell)
```powershell
# Configure build directory
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# Compile all targets
cmake --build build -j 8

# Execute test suite
.\build\search_test.exe

# Launch standalone binary
.\build\qfbi.exe
```

### Building with Visual Studio (MSVC)
```powershell
# Configure MSVC solution
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Compile release configuration
cmake --build build --config Release -j 8

# Run test target
ctest --test-dir build -C Release --output-on-failure
```

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for full legal text.
