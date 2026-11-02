# ---------------------------------------------------------------------------
# FHI's prettier prompt  (sourced from .zshrc, AFTER the user's own .zshrc)
#
# Only used when the user has NOT set up a prompt of their own, i.e. PS1 is
# empty, still looks like zsh's plain default ("user@host ~ %"), or is the
# "adam1" theme that Debian's recommended .zshrc template turns on. Anyone with
# powerlevel10k, starship, an oh-my-zsh theme, ... keeps their prompt.
#
#   FHI_PRETTY_PROMPT=0   (in your ~/.zshrc)  never touch the prompt
#   FHI_PRETTY_PROMPT=1                       always use this prompt
#
# ---------------------------------------------------------------------------

__fhi_prompt_is_default() {
  [[ -z "$PS1" ]] && return 0
  [[ "${prompt_theme[1]}" == adam1 ]] && return 0
  # Remove everything a plain default prompt is made of; if nothing is left,
  # there was no customization (colors, symbols, command substitutions...).
  local t="$PS1"
  t=${t//(%n|%m|%M|%~|%1~|%c|%C|%\/|%d|%#|%%|%B|%b|%\(\!.%#.%%\))/}
  t=${t//[ @:]/}
  [[ -z "$t" ]]
}

if [[ "${FHI_PRETTY_PROMPT:-auto}" != 0 ]] \
   && { [[ "$FHI_PRETTY_PROMPT" == 1 ]] || __fhi_prompt_is_default; }; then

  # A `prompt <theme>` keeps re-setting PS1 before every prompt, so switch it off
  # (it also turns prompt_subst off, hence the setopt comes after)
  (( $+functions[prompt] )) && prompt off 2>/dev/null
  setopt prompt_subst
  autoload -Uz vcs_info add-zsh-hook

  # Git branch (zsh's own vcs_info, no external prompt framework needed)
  zstyle ':vcs_info:*' enable git
  zstyle ':vcs_info:git:*' formats       '%K{94}%f %b %k'
  zstyle ':vcs_info:git:*' actionformats '%K{94}%f %b (%a) %k'
  __fhi_vcs_precmd() { vcs_info }
  add-zsh-hook precmd __fhi_vcs_precmd

  # Colored "banners" behind the text; the text itself keeps the terminal's
  # default color (%f). user@host = blue (red for root), folder = grey,
  # git branch = brown, arrow turns red after a failed command.
  PS1='%K{%(!.88.24)}%f %n@%m %k%K{238}%f %~ %k${vcs_info_msg_0_} %(?.%f.%F{203})❯%f '
  PS2='%F{244}…%f '
  unset RPS1 RPROMPT
fi
