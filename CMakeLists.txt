cmake_minimum_required(VERSION 3.16)
project(QBFindCpp LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(qbcore STATIC
  src/core.cpp
  src/search.cpp
  src/indexer.cpp
  src/ole_dragdrop.cpp
  src/actions.cpp
)

target_compile_definitions(qbcore PUBLIC
  UNICODE
  _UNICODE
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(qbcore PUBLIC
  ole32
  oleaut32
  shell32
  uuid
  uxtheme
  dwmapi
  dbghelp
  psapi
  advapi32
  bcrypt
  gdiplus
)

if(MSVC)
  target_compile_options(qbcore PUBLIC /W4 /utf-8 /EHsc)
else()
  target_compile_options(qbcore PUBLIC -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

add_executable(qfbi WIN32
  src/main.cpp
  src/app.rc
)

target_link_libraries(qfbi PRIVATE
  qbcore
  comctl32
  uxtheme
  dwmapi
)

if(MSVC)
  target_link_options(qfbi PRIVATE /MANIFEST:NO)
else()
  target_compile_options(qbcore PRIVATE -ffunction-sections -fdata-sections)
  target_compile_options(qfbi PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -ffunction-sections -fdata-sections)
  target_link_options(qfbi PRIVATE -municode -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-s)
endif()

enable_testing()
add_executable(search_test tests/search_test.cpp)
target_link_libraries(search_test PRIVATE qbcore)
if(NOT MSVC)
  target_compile_options(search_test PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(search_test PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-s)
endif()
add_test(NAME search_test COMMAND search_test)

