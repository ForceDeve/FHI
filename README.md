# FHI

**FHI** (*ForceDev's Hybrid Interface*) is a zsh terminal with a file list beside it. The list shows
the folder you're in and highlights the files the command you're typing would affect, before you
press Enter.

> **Beta (0.1.1):** usable, but little tested outside the author's machine.

## Features

- **Command preview** – files light up by what the command would do: mentioned (yellow), created
  (green), `mv`/`cp`/`ln` destination (blue), deleted (red). Understands globs, `{a,b}` braces,
  quoting, `sudo`, redirections and pipes. Files a command would create appear as faint ghost rows.
- **Keyboard-driven list** – `Shift+←/→` switch between list and terminal, `Enter` opens a file or
  `cd`s into a folder, `Shift+Enter` types the path onto your command line, `Backspace` is `cd ..`.
- **Search by typing** – type in the list to filter with shell patterns (`*.png`, `{a,b}*`, `!*.o`).
  `Enter` keeps the filter, `Esc` clears it.
- **Sorting** – name, size, modified, created or type; `Shift+↑/↓` changes the mode, `Ctrl+R` reverses.
- **File info bar** – type detected from the file header (not the extension), size, created and
  last-accessed dates.
- **Hamburger menu** (button, or `Tab` in the list) – theme and colors, cheat sheet, hidden files,
  sorting and info settings.

## Install

Needs **zsh**, GTK 4 and VTE for GTK 4 (`libvte-2.91-gtk4`: Debian 13+ / Ubuntu 24.04+, Fedora, Arch).

- Debian/Ubuntu: download the `.deb` from the Releases page and `sudo apt install ./fhi_*.deb`.
- From source: see [`packaging/INSTALL.txt`](packaging/INSTALL.txt).

## License

Free to use, change and share. Two conditions: keep the author's copyright notice in the source
code, and if you rename a modified version, the new name must still contain **FHI** (FHIR, SFHI, ...).
See [`LICENSE`](LICENSE).

*Built by an undergrad learning and making a tool he enjoys himself.*
