// QBFind C++ - Native COM / OLE Drag and Drop support.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace qb {

// Initiates OLE Drag and Drop operation with the given file paths.
// Returns true if files were dropped successfully.
bool doDragDropFiles(HWND hwnd, const std::vector<std::wstring>& filePaths);

}  // namespace qb
