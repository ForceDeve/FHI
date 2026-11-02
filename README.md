# FHI

**FHI** (*ForceDev's Hybrid Interface*) is a zsh terminal with a file list beside it. The list shows
the folder you're in and highlights the files the command you're typing would affect, before you
press Enter.

> **Beta (0.1.3):** usable, but little tested outside the author's machine.

## Features

- **Command preview** – files light up by what the command would do: mentioned (yellow), created
  (green), `mv`/`cp`/`ln` destination (blue), deleted (red). Understands globs, `{a,b}` braces,
  quoting, `sudo`, redirections and pipes. Files a command would create appear as faint ghost rows.
  `git commit` lights up the files it would commit (staged ones, or all tracked changes with `-a`)
  and `git push` the files in the commits it would send. Needs `git`; a change inside a subfolder
  lights up that folder.
- **Keyboard-driven list** – `Shift+←/→` switch between list and terminal, `Enter` opens a file or
  `cd`s into a folder, `Shift+Enter` pastes the path into whatever runs in the terminal (shell-quoted at the prompt, plain text in vim/nano, a quoted string in python), `Backspace` is `cd ..`.
- **Folder peeking** – `→` opens the highlighted folder inside the list (indented, nesting works,
  no `cd`); `←` closes it, or jumps from an entry back to its folder. `Enter` still `cd`s, and keeps
  whatever you were typing on the command line.
- **Drag and drop** – drag a file or folder out of the list into a browser, Discord, a file manager
  or any app that accepts files (upload to a git release, share in chat...). It copies, never moves.
- **File-type icons** – a colored Nerd Font icon per file: languages (C, C++, Python, JS/TS, Rust,
  Go, GDScript, ...), config and data files, media, archives, and special names such as `Makefile`
  or `.gitignore`. A Symbols Nerd Font is bundled, nothing to install.
- **Nicer terminal** – a banner-style prompt (used only if you haven't set up your own; turn it off
  with `FHI_PRETTY_PROMPT=0` in your `~/.zshrc`), terminal font choice in the Theme page, and the
  bundled Nerd Font as a fallback so prompt icons always show. Other shells (bash, fish, ...)
  dim the list and say they are not supported.
- **Search by typing** – type in the list to filter with shell patterns (`*.png`, `{a,b}*`, `!*.o`).
  `Enter` keeps the filter, `Esc` clears it.
- **Sorting** – name, size, modified, created or type; `Shift+↑/↓` changes the mode, `Ctrl+R` reverses.
- **File info bar** – type detected from the file header (not the extension), size, created and
  last-accessed dates.
- **Hamburger menu** (button, or `Tab` in the list) – theme and colors, cheat sheet, hidden files,
  sorting and info settings.

## Install

Needs **zsh**, GTK 4, VTE for GTK 4 (`libvte-2.91-gtk4`: Debian 13+ / Ubuntu 24.04+, Fedora, Arch)
and fontconfig. `git` is optional (for the git commit / push preview).

- Debian/Ubuntu: download the `.deb` from the Releases page and `sudo apt install ./fhi_*.deb`.
- Arch: download the `.pkg.tar.zst` and `sudo pacman -U fhi-*.pkg.tar.zst` (or build it with
  `packaging/PKGBUILD`).
- From source: see [`packaging/INSTALL.txt`](packaging/INSTALL.txt).

## License

Free to use, change and share. Two conditions: keep the author's copyright notice in the source
code, and if you rename a modified version, the new name must still contain **FHI** (FHIR, SFHI, ...).
See [`LICENSE`](LICENSE).

*Built by an undergrad learning and making a tool he enjoys himself.*
