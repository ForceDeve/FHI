# FHI's .zprofile  (read only by LOGIN shells, after .zshenv)
# Same pattern as .zshenv: run the user's file with ZDOTDIR pointing at theirs,
# then point ZDOTDIR back at us so zsh keeps reading OUR files.
if [[ -r "$FHI_USER_ZDOTDIR/.zprofile" ]]; then
  ZDOTDIR="$FHI_USER_ZDOTDIR"
  source "$FHI_USER_ZDOTDIR/.zprofile"
  ZDOTDIR="$FHI_ZDOTDIR"
fi
