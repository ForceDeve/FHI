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

add_executable(FHI main.c)
target_link_libraries(FHI PRIVATE PkgConfig::GTK4 PkgConfig::VTE)
