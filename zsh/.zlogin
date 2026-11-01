# FHI's .zlogin  (read only by LOGIN shells, after .zshrc)
# Same pattern as .zshenv.
if [[ -r "$FHI_USER_ZDOTDIR/.zlogin" ]]; then
  ZDOTDIR="$FHI_USER_ZDOTDIR"
  source "$FHI_USER_ZDOTDIR/.zlogin"
  ZDOTDIR="$FHI_ZDOTDIR"
fi
