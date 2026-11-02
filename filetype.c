/*
 * FHI - ForceDev's Hybrid Interface
 * Copyright (c) 2026 ForceDev (https://github.com/ForceDeve)
 * Licensed under the FHI License, see the LICENSE file.
 */

#include "filetype.h"
#include <fontconfig/fontconfig.h>
#include <string.h>

typedef struct { const char *keys; const char *glyph; const char *color; } Kind;

/* `keys` is a space separated list of lower case extensions (no dot) */
static const Kind by_ext[] = {
	/* languages */
	{ "c",                       "", "#5c8dbc" },
	{ "h",                       "", "#a074c4" },
	{ "cpp cc cxx c++ ino",      "", "#519aba" },
	{ "hpp hh hxx h++",          "", "#a074c4" },
	{ "cs",                      "", "#68a063" },
	{ "py pyw pyi ipynb",        "", "#ffbc03" },
	{ "js mjs cjs",              "", "#cbcb41" },
	{ "ts mts cts",              "", "#519aba" },
	{ "jsx tsx",                 "", "#20c2e3" },
	{ "gd gdscript tscn tres godot", "", "#6d8086" },
	{ "rs",                      "", "#dea584" },
	{ "go",                      "", "#519aba" },
	{ "java jar",                "", "#cc3e44" },
	{ "kt kts",                  "", "#a074c4" },
	{ "swift",                   "", "#e37933" },
	{ "lua",                     "", "#51a0cf" },
	{ "rb gemspec",              "", "#cc3e44" },
	{ "php",                     "", "#a074c4" },
	{ "pl pm",                   "", "#519aba" },
	{ "r",                       "", "#519aba" },
	{ "jl",                      "", "#a074c4" },
	{ "dart",                    "", "#519aba" },
	{ "ex exs",                  "", "#a074c4" },
	{ "clj cljs",                "", "#8dc149" },
	{ "scala",                   "", "#cc3e44" },
	{ "hs lhs",                  "", "#a074c4" },
	{ "ml mli",                  "", "#e37933" },
	{ "elm",                     "", "#519aba" },
	{ "nim",                     "", "#cbcb41" },
	{ "zig",                     "", "#e37933" },
	{ "vue",                     "", "#8dc149" },
	{ "svelte",                  "", "#cc3e44" },
	{ "sh bash zsh fish ksh",    "", "#4d5a5e" },
	{ "tex sty bib",             "", "#3d6117" },
	{ "gradle",                  "", "#519aba" },
	{ "mk mak",                  "", "#6d8086" },
	/* web, data, config */
	{ "html htm xhtml",          "", "#e44d26" },
	{ "css",                     "", "#42a5f5" },
	{ "scss sass",               "", "#f55385" },
	{ "json jsonc json5",        "", "#cbcb41" },
	{ "yml yaml",                "", "#6d8086" },
	{ "xml xsd xsl plist",       "", "#e37933" },
	{ "csv tsv",                 "", "#89e051" },
	{ "toml ini cfg conf",       "", "#6d8086" },
	{ "md markdown rst",         "", "#519aba" },
	{ "txt log",                 "", "#89e051" },
	{ "pdf",                     "", "#cc3e44" },
	{ "lock",                    "", "#bbbbbb" },
	/* media and archives */
	{ "svg",                     "", "#ffb13b" },
	{ "png jpg jpeg gif bmp webp ico tiff avif xcf psd", "", "#a074c4" },
	{ "mp3 wav flac ogg opus m4a aac", "", "#519aba" },
	{ "mp4 mkv webm avi mov",    "", "#fd971f" },
	{ "zip tar gz tgz xz bz2 zst 7z rar deb rpm", "", "#e5a50a" },
	{ "o a so obj bin elf exe dll", "", "#6d8086" },
};

/* Whole file names that mean something regardless of the extension (lower case) */
static const Kind by_name[] = {
	{ "makefile gnumakefile",    "", "#6d8086" },
	{ "cmakelists.txt",          "", "#519aba" },
	{ "dockerfile docker-compose.yml compose.yaml", "", "#519aba" },
	{ ".gitignore .gitattributes .gitmodules", "", "#e37933" },
	{ "license copying",         "", "#cbcb41" },
	{ ".zshrc .zshenv .zprofile .zlogin .bashrc .profile", "", "#4d5a5e" },
	{ "package.json",            "", "#cc3e44" },
};

static GHashTable *ext_map, *name_map;

static void
fill(GHashTable *map, const Kind *kinds, gsize n)
{
	for (gsize i = 0; i < n; i++) {
		char **words = g_strsplit(kinds[i].keys, " ", -1);
		for (char **w = words; *w; w++) g_hash_table_insert(map, g_strdup(*w), (gpointer)&kinds[i]);
		g_strfreev(words);
	}
}

const char *
fhi_file_icon(const char *name, gboolean is_dir, const char **color)
{
	if (is_dir) {
		*color = "#7aa2d6";
		return "";
	}
	if (!ext_map) {
		ext_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		name_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		fill(ext_map, by_ext, G_N_ELEMENTS(by_ext));
		fill(name_map, by_name, G_N_ELEMENTS(by_name));
	}

	char *lower = g_ascii_strdown(name, -1);
	const Kind *k = g_hash_table_lookup(name_map, lower);
	if (!k) {
		const char *dot = strrchr(lower, '.');
		if (dot && dot != lower) k = g_hash_table_lookup(ext_map, dot + 1);
	}
	g_free(lower);

	if (!k) {
		*color = "#9a9996";
		return "";   /* generic file */
	}
	*color = k->color;
	return k->glyph;
}

void
fhi_icon_font_register(const char *path)
{
	FcConfigAppFontAddFile(NULL, (const FcChar8 *)path);
}
