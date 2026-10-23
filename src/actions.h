// QBFind C++ - Native file actions: preview, desktop copy, cut, delete, properties.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "core.h"

namespace qb {

// Previews file contents (UTF-8/UTF-16 BOM, JSON formatting, 4000 char limit).
// Returns true if text preview was displayed.
bool previewFile(HWND parent, const std::wstring& path, const std::wstring& name, HFONT font);

// Copies files to Desktop using SHFileOperationW with recycle bin / collision rename.
bool copyFilesToDesktop(HWND parent, const std::vector<std::wstring>& paths);

// Puts files on clipboard with CF_HDROP and optional DROPEFFECT_MOVE (cut).
bool putFilesOnClipboard(HWND parent, const std::vector<std::wstring>& paths, bool isCut);

// Deletes files to Recycle Bin using SHFileOperationW with FOF_ALLOWUNDO.
bool deleteFilesToRecycleBin(HWND parent, const std::vector<std::wstring>& paths);

// Shows the native Windows file properties dialog.
void showFileProperties(HWND parent, const std::wstring& path);

// Shows file hash dialog (SHA-256 and MD5) with clipboard copy and hash comparison.
bool showFileHashDialog(HWND parent, const std::wstring& path, bool isTurkish);

// Shows calculated folder statistics dialog.
void showFolderSizeDialog(HWND parent, const std::wstring& folderPath, int64_t totalBytes, int64_t fileCount, int64_t subDirCount, bool isTurkish);

// Shows interactive batch renamer dialog.
bool showBatchRenameDialog(HWND parent, const std::vector<std::wstring>& paths, bool isTurkish);

// Shows professional About dialog with architecture and shortcut details.
void showAboutDialog(HWND parent, bool isTurkish);

// Shows custom size filter input dialog.
bool showCustomSizeDialog(HWND parent, std::wstring& outSizeFilter, bool isTurkish);

}  // namespace qb
