cmake_minimum_required(VERSION 3.21)
project(FHI C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# VTE (GTK4) is built from source into ~/.local
list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
# VTE's GTK4 build is a separate package (libvte-2.91-gtk4-dev); plain "vte" is the old GTK2 one
pkg_check_modules(VTE REQUIRED IMPORTED_TARGET vte-2.91-gtk4)

# Two ways to build:
#   - normal (CLion): the program runs from this folder and reads zsh/ and icons/ from here
#   - -DFHI_PACKAGE=ON (the .deb): data files are installed under <prefix>/share/fhi
option(FHI_PACKAGE "Build for installing (used by packaging/build-deb.sh)" OFF)

if(FHI_PACKAGE)
	set(FHI_DATA_DIR "${CMAKE_INSTALL_PREFIX}/share/fhi")
else()
	# Put the finished program in the project folder, next to the sources, as "FHI"
	# (instead of inside the cmake-build-* folder)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})
	set(FHI_DATA_DIR "${CMAKE_SOURCE_DIR}")
endif()

add_executable(FHI main.c preview.c)
target_link_libraries(FHI PRIVATE PkgConfig::GTK4 PkgConfig::VTE)

# Folders with the zsh startup files (the program tells zsh to read them via ZDOTDIR)
# and the window icon.
target_compile_definitions(FHI PRIVATE
	FHI_ZSH_DIR="${FHI_DATA_DIR}/zsh"
	FHI_ICON_DIR="${FHI_DATA_DIR}/icons")

if(FHI_PACKAGE)
	include(GNUInstallDirs)
	install(TARGETS FHI RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
	install(DIRECTORY zsh/ DESTINATION share/fhi/zsh)   # includes the dotfiles
	install(FILES icons/fhi.png DESTINATION share/fhi/icons)
	install(FILES icons/fhi.png DESTINATION share/icons/hicolor/256x256/apps)
	install(FILES packaging/dev.forcedev.myterm.desktop DESTINATION share/applications)
	install(FILES LICENSE DESTINATION share/doc/fhi RENAME copyright)
endif()
