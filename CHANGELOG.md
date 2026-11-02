# Changelog

## 0.1.5 - 2026-11-03

### Added
- **Multi-select.** `Space` marks / unmarks the highlighted file, `Ctrl+A` marks all shown files,
  `Esc` clears. `Shift+Enter` pastes every marked file (shell-quoted paths, `@mentions` for AI
  assistants, a list of strings in python, one per line in vim / nano), and dragging a marked file
  drags all marked files (the highlighted row joins them). A counter under the list shows how many
  are marked. The marks clear by themselves after a paste or a successful drop; a cancelled drag keeps them.
- **Tabs.** `Ctrl+Shift+T` new tab (starts in the folder the list shows), `Ctrl+Shift+W` close,
  `Ctrl+PageUp/PageDown` and `Ctrl+Tab` / `Ctrl+Shift+Tab` switch, tabs can be dragged to reorder,
  and there is a "+" button. The bar is hidden with a single tab. Each tab has its own shell; the
  file list, command preview, ssh card and assistant watcher follow the tab in front. Themes and the
  terminal font apply to every tab. A tab closes when its shell exits; the last one closes the window.
- **tmux support.** While tmux is in front, the file list follows the focused pane: its folder
  and the program running in it (an assistant, ssh, an unsupported shell...). It asks the tmux
  server (`tmux list-clients`, also with `-L` / `-S`) twice a second. Folder navigation by
  typing `cd` only happens in panes whose shell was started by FHI.
- **AI coding assistant support.** FHI recognises Claude Code, Codex, Gemini CLI, Copilot CLI,
  Cursor Agent, Amp, Droid and open source ones (opencode, Qwen Code, Kimi CLI, Kilo Code, Mistral
  Vibe, Continue, Crush, aider, goose, Cline, OpenHands, Plandex, Open Interpreter, gptme, RA.Aid)
  and chat clients (Ollama, llm, aichat, sgpt). `Shift+Enter` pastes the highlighted file as
  `@relative/path`, or the plain path for tools without @-mentions. Files the assistant creates
  (green) or edits (yellow) light up in the list, with the folders above them, and opened folders
  show new files inside; deleted files are noticed too. A label under the list names the assistant.
  The folder is also compared with the disk once a second while one runs, as a fallback.

### Fixed
- A stray `,` after a comment made `main.c` fail to compile.
- Starting FHI from inside an FHI terminal no longer makes zsh load FHI's own `.zshrc` as the
  user's.

## 0.1.4 - 2026-11-02

### Added
- **SSH session panel.** While `ssh` is in front of the terminal the file list dims and a card shows
  the session: host, port, time connected, jump host, forwarding (agent, X11, tunnels, compression),
  and the account on the server: user and uid, primary group, other groups, home directory with its
  permissions, umask, shell, OS, kernel and uptime. The account details come from one extra silent
  `ssh` run with your options plus `BatchMode`; it never prompts, so it works with keys, the agent
  or a ControlMaster connection and shows "unavailable" otherwise. `FHI_SSH_NO_QUERY=1` turns it off.
  The card fills the panel, wraps long values, switches to a stacked layout when narrow and scrolls
  when tall.
- **Themes.** Theme files (INI, every key optional, whatever is left out follows the system GTK
  theme) in `~/.config/fhi/themes/` and the install folder; six are bundled: Pastel Pink, Dracula,
  Nord, Gruvbox Dark, Solarized Light, Catppuccin Mocha. The Theme page of the menu picks a theme,
  sets window / text / file list / accent / terminal / cursor colors, the 16 ANSI colors and the
  corner radius, can hand any color back to the system, and can save your colors as a new theme.
- **`~/.config/fhi/user.css`**, your own GTK CSS applied last, reloaded as soon as you save it.
  Errors are printed with their line number.
- **Window transparency** (Opacity slider). Needs a compositor. At 100 % nothing is set up for it at
  all, so it costs nothing unless used.
- **Build and packaging previews.** Commands that read a file without naming it light it up:
  `make`, `cmake`, `ninja`, `meson`, `just`, `makepkg` (also the package file it will produce and
  the `src/` / `pkg/` folders), `dpkg-buildpackage`, `debuild`, `docker build` / `compose`, `cargo`,
  `go`, `npm`, `pnpm`, `yarn`, `mvn`, `gradle`. `cargo clean` shows `target/` as deleted. A
  `./script` you are about to run is highlighted too, and `fakeroot` is treated like `sudo`.
- **Editor integration.** With vim or nano open, `Enter` on a text or source file saves the current
  file and opens the chosen one in the same editor.
- **Copy and paste** in the terminal with `Ctrl+Shift+C` / `Ctrl+Shift+V`.

### Changed
- The Theme page was rebuilt. The old "custom terminal colors" switch is gone: a terminal color is
  now either set or follows the system. Existing settings are read as before.
- The color scheme is stored as `system` / `light` / `dark` in `settings.ini` (it was 0 / 1 / 2).
  Going back to 0.1.3 resets the scheme to System; nothing else is affected.

### Requirements
GTK 4.8 or newer, VTE for GTK 4 0.70 or newer, zsh. `git` and an `ssh` client are optional.

## 0.1.3 and earlier
See the git history.
