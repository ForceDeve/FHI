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
