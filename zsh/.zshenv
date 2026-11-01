# ---------------------------------------------------------------------------
# FHI's .zshenv  (zsh reads this first, for EVERY zsh, interactive or not)
#
# How this whole folder works:
#   zsh looks for its startup files (.zshenv, .zprofile, .zshrc, .zlogin) in the
#   directory named by $ZDOTDIR, falling back to $HOME. The C program sets
#   ZDOTDIR to THIS folder, so zsh reads OUR files instead of the user's.
#
#   That would silently throw away the user's prompt, aliases and plugins, so
#   each file here does two jobs:
#     1. load the user's real file of the same name
#     2. (in .zshrc only) add our extra hook on top
#
# The C program passes us two variables:
#   FHI_ZDOTDIR       = this folder (so we can put ZDOTDIR back to it)
#   FHI_USER_ZDOTDIR  = where the user's own files live ($ZDOTDIR or $HOME)
# ---------------------------------------------------------------------------

if [[ -r "$FHI_USER_ZDOTDIR/.zshenv" ]]; then
  # While the user's file runs, pretend ZDOTDIR is theirs. Many users' configs
  # use $ZDOTDIR for things like the history file location, and they would
  # break (or write into our folder) if they saw our directory instead.
  ZDOTDIR="$FHI_USER_ZDOTDIR"
  source "$FHI_USER_ZDOTDIR/.zshenv"

  # Switch back, otherwise zsh would read the user's .zshrc next and never
  # reach ours. zsh re-reads $ZDOTDIR before opening each startup file.
  ZDOTDIR="$FHI_ZDOTDIR"
fi
