cmake_minimum_required(VERSION 4.3)
project(FHI C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# VTE (GTK4) is built from source into ~/.local
list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
# VTE's GTK4 build is a separate package (libvte-2.91-gtk4-dev); plain "vte" is the old GTK2 one
pkg_check_modules(VTE REQUIRED IMPORTED_TARGET vte-2.91-gtk4)

# Put the finished program in the project folder, next to the sources, as "FHI"
# (instead of inside the cmake-build-* folder)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(FHI main.c preview.c)
target_link_libraries(FHI PRIVATE PkgConfig::GTK4 PkgConfig::VTE)

# Folder with the zsh startup files; the program tells zsh to read them via ZDOTDIR.
# For a packaged build, replace this with the installed location of that folder.
target_compile_definitions(FHI PRIVATE FHI_ZSH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/zsh")
