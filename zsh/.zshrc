# ---------------------------------------------------------------------------
# FHI's .zshrc  (read by INTERACTIVE shells: this is the one VTE starts)
# ---------------------------------------------------------------------------

# Step 1: load the user's own .zshrc first (prompt, aliases, plugins...).
# Same ZDOTDIR swap as in .zshenv, see the explanation there.
if [[ -r "$FHI_USER_ZDOTDIR/.zshrc" ]]; then
  ZDOTDIR="$FHI_USER_ZDOTDIR"
  source "$FHI_USER_ZDOTDIR/.zshrc"
  ZDOTDIR="$FHI_ZDOTDIR"
fi

# Step 1b: a nicer prompt, but only if the user has not set one up themselves.
source "$FHI_ZDOTDIR/fhi-prompt.zsh"

# Step 2: our hook. We do this AFTER the user's file on purpose, so nothing
# they configured can remove or override it.
#
# The goal: every time the prompt is about to be shown, print a special
# "escape sequence" that tells the terminal widget (VTE) which directory the
# shell is in. It looks like this (\e = ESC, \a = bell character):
#
#       ESC ] 7 ; file:///home/me/some%20folder BEL
#
# This is called "OSC 7". The terminal swallows it silently (nothing appears on
# screen) and VTE then emits its "current-directory-uri-changed" signal, which
# is what the C program listens for.

# add-zsh-hook is zsh's safe way to register hooks. It lets several hooks
# coexist instead of overwriting each other like `precmd() { ... }` would.
# `autoload` loads it on demand.
autoload -Uz add-zsh-hook

__fhi_report_cwd() {
  # A URI can't contain raw spaces or most symbols, so the path must be
  # "percent-encoded": a space becomes %20, an é becomes %C3%A9, and so on.
  # We walk through the path one character at a time and keep safe characters
  # as-is. `local LC_ALL=C` makes zsh treat the text as raw bytes (not
  # multi-byte characters), so non-ASCII characters get encoded byte by byte,
  # which is exactly what URIs expect.
  local LC_ALL=C
  local i ch hex encoded=""
  for (( i = 1; i <= ${#PWD}; ++i )); do
    ch="${PWD[i]}"
    if [[ "$ch" == [/._~A-Za-z0-9-] ]]; then
      encoded+="$ch"
    else
      # "'$ch" makes printf print the character's numeric code; %02X formats it
      # as two hex digits. Example: space -> 20, so we append "%20".
      printf -v hex '%02X' "'$ch"
      encoded+="%$hex"
    fi
  done

  # "file://" followed by an EMPTY host name, then the path (which begins with
  # "/"), gives "file:///home/...". An empty host means "this machine", which
  # avoids VTE rejecting the URI because of a hostname mismatch.
  printf '\e]7;file://%s\a' "$encoded"
}

# precmd hooks run just before each prompt is drawn, i.e. after every command
# has finished. That's exactly "check every time a command is entered", and it
# also catches `cd` inside aliases and functions.
add-zsh-hook precmd __fhi_report_cwd

# ---------------------------------------------------------------------------
# Live command line for the file preview
#
# While the user types, the C program wants to know the text of the command
# line, so it can highlight the files the command would touch. Terminals have
# no standard way to send that, so we borrow one that VTE reliably reports:
# the window TITLE (escape sequence ESC ] 2 ; text BEL). VTE emits
# "window-title-changed" and the C program ignores every title that does not
# start with "FHI-BUF:".
#
# zsh runs "zle hooks" at set moments while you edit the line:
#   line-pre-redraw  - before the line is redrawn, i.e. after EVERY keystroke
#   line-finish      - when you press Enter (the line is accepted)
# $BUFFER holds the text of the line being edited.
# ---------------------------------------------------------------------------
autoload -Uz add-zle-hook-widget

typeset -g __fhi_last_buffer=""

# Send $1 as the new command line text. Control characters and "%" are
# percent-encoded (a newline becomes %0A) so they cannot break the escape
# sequence; the C program decodes them again.
__fhi_send_buffer() {
  setopt localoptions extendedglob
  local encoded=${1//(#m)[%[:cntrl:]]/%${(l:2::0:)$(( [##16] #MATCH ))}}
  print -rn -- $'\e]2;FHI-BUF:'"$encoded"$'\a'
}

__fhi_buffer_changed() {
  # only talk to the terminal when the text really changed
  [[ "$BUFFER" == "$__fhi_last_buffer" ]] && return
  __fhi_last_buffer="$BUFFER"
  __fhi_send_buffer "$BUFFER"
}

__fhi_buffer_finished() {
  # Enter was pressed: the command runs now, so clear the preview
  __fhi_last_buffer=""
  __fhi_send_buffer ""
}

add-zle-hook-widget line-pre-redraw __fhi_buffer_changed
add-zle-hook-widget line-finish __fhi_buffer_finished

# ---------------------------------------------------------------------------
# "A command has finished" signal
#
# precmd runs right before every prompt, so it runs after each command ends.
# We send a title "FHI-PROMPT:<counter>". The counter makes every title
# different: VTE only reports a title when it CHANGED. The C program then checks
# that its file list still matches the folder (see resync_directory in main.c).
# ---------------------------------------------------------------------------
typeset -gi __fhi_prompt_count=0

__fhi_prompt_ready() {
  print -rn -- $'\e]2;FHI-PROMPT:'"$(( ++__fhi_prompt_count ))"$'\a'
}

add-zsh-hook precmd __fhi_prompt_ready
