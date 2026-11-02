/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#ifndef FHI_FILETYPE_H
#define FHI_FILETYPE_H

#include <glib.h>

/*
 * Picks a Nerd Font icon for a file name: language (C, Python, JS, GDScript, ...),
 * well-known names (Makefile, Dockerfile, .gitignore) or a generic kind
 * (image, archive, ...). `color` is a "#rrggbb" string that stays valid forever.
 * Returns the glyph as UTF-8 (also static).
 */
const char *fhi_file_icon(const char *name, gboolean is_dir, const char **color);

/* Family name of the bundled font, and registering the font file with fontconfig */
#define FHI_ICON_FONT "Symbols Nerd Font"
void fhi_icon_font_register(const char *path);

#endif
