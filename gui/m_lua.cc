//----------------------------------------------------------------------
//  LUA interface
//----------------------------------------------------------------------
//
//  Oblige Level Maker
//
//  Copyright (C) 2006-2017 Andrew Apted
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//----------------------------------------------------------------------

#include "headers.h"

#include <algorithm>

#include "hdr_fltk.h"
#include "hdr_lua.h"
#include "hdr_ui.h"

#include "physfs.h"

#include "lib_file.h"
#include "lib_signal.h"
#include "lib_util.h"

#include "main.h"
#include "m_lua.h"


#define AJ_RANDOM_IMPLEMENTATION
#include "aj_random.h"


static lua_State *LUA_ST;

static bool has_loaded = false;
static bool has_added_buttons = false;

static std::vector<std::string> * conf_line_buffer;

static const char *import_dir;

void Script_Load(const char *name);



// random number generator
static aj_Random_c GUI_RNG;

// color maps
color_mapping_t color_mappings[MAX_COLOR_MAPS];


// LUA: raw_log_print(str)
//
int gui_raw_log_print(lua_State *L)
{
	int nargs = lua_gettop(L);

	if (nargs >= 1)
	{
		const char *res = luaL_checkstring(L,1);
		SYS_ASSERT(res);

		// strip off colorizations
		if (res[0] == '@' && isdigit(res[1]))
			res += 2;

		LogPrintf("%s", res);
	}

	return 0;
}

// LUA: raw_debug_print(str)
//
int gui_raw_debug_print(lua_State *L)
{
	int nargs = lua_gettop(L);

	if (nargs >= 1)
	{
		const char *res = luaL_checkstring(L,1);
		SYS_ASSERT(res);

		DebugPrintf("%s", res);
	}

	return 0;
}


// LUA: gettext(str)
//
int gui_gettext(lua_State *L)
{
	const char *s = luaL_checkstring(L,1);

	lua_pushstring(L, ob_gettext(s));
	return 1;
}


// LUA: config_line(str)
//
int gui_config_line(lua_State *L)
{
	const char *res = luaL_checkstring(L,1);

	SYS_ASSERT(conf_line_buffer);

	conf_line_buffer->push_back(res);

	return 0;
}


// LUA: mkdir(dir_name)
//
int gui_mkdir(lua_State *L)
{
	const char *name = luaL_checkstring(L,1);

	bool result = FileMakeDir(name);

	lua_pushboolean(L, result ? 1 : 0);
	return 1;
}


// LUA: set_colormap(map, colors)
//
int gui_set_colormap(lua_State *L)
{
	int map_id = luaL_checkint(L, 1);

	if (map_id < 1 || map_id > MAX_COLOR_MAPS)
		return luaL_argerror(L, 1, "colmap value out of range");

	if (lua_type(L, 2) != LUA_TTABLE)
	{
		return luaL_argerror(L, 2, "expected a table: colors");
	}

	color_mapping_t *map = & color_mappings[map_id-1];

	map->size = 0;

	for (int i = 0; i < MAX_COLORS_PER_MAP; i++)
	{
		lua_pushinteger(L, 1+i);
		lua_gettable(L, 2);

		if (lua_isnil(L, -1))
		{
			lua_pop(L, 1);
			break;
		}

		map->colors[i] = luaL_checkint(L, -1);
		map->size = i+1;

		lua_pop(L, 1);
	}

	return 0;
}


// LUA: import(script_name)
//
int gui_import(lua_State *L)
{
	if (! import_dir or ! import_dir[0])
	{
		return luaL_error(L, "gui.import: no directory set!");
	}

	const char *script_name = luaL_checkstring(L, 1);

	Script_Load(script_name);

	return 0;
}


// LUA: set_import_dir(dir_name)
//
int gui_set_import_dir(lua_State *L)
{
	const char *dir_name = luaL_checkstring(L, 1);

	if (import_dir)
		StringFree(import_dir);

	import_dir = StringDup(dir_name);

	return 0;
}


// LUA: get_install_dir() --> string
//
int gui_get_install_dir(lua_State *L)
{
	lua_pushstring(L, install_dir);
	return 1;
}


static bool scan_dir_process_name(const char *name, const char *parent, const char *match)
{
	if (name[0] == '.')
		return false;

// fprintf(stderr, "scan_dir_process_name: '%s'\n", name);

	// check if it is a directory
	// [ generally skip directories, unless match is "DIRS" ]

	char *temp_name = StringPrintf("%s/%s", parent, name);
	bool is_dir = PHYSFS_isDirectory(temp_name);

	if (strcmp(match, "DIRS") == 0)
	{
		StringFree(temp_name);
		return is_dir;
	}

	if (is_dir)
	{
		StringFree(temp_name);
		return false;
	}

	// pretend that zero-length files do not exist
	// [ allows a PK3 to _remove_ a file ]

	byte buffer[1];

	PHYSFS_File *fp = PHYSFS_openRead(temp_name);

	StringFree(temp_name);

	if (! fp)
		return false;

	if (PHYSFS_read(fp, buffer, 1, 1) < 1)
	{
		PHYSFS_close(fp);
		return false;
	}

	PHYSFS_close(fp);

	// lastly, check match

	if (strcmp(match, "*") == 0)
	{
		return true;
	}
	else if (match[0] == '*' && match[1] == '.' && isalnum(match[2]))
	{
		return MatchExtension(name, match + 2);
	}

	Main_FatalError("gui.scan_directory: unsupported match expression: %s\n", match);
	return false;  /* NOT REACHED */
}


struct scan_dir_nocase_CMP
{
	inline bool operator() (const std::string& A, const std::string& B) const
	{
		return StringCaseCmp(A.c_str(), B.c_str()) < 0;
	}
};


// LUA: scan_directory(dir, match) --> list
//
// Note: 'match' parameter must be of the form "*" or "*.xxx"
//       or must be "DIRS" to return all the sub-directories
//
int gui_scan_directory(lua_State *L)
{
	const char *dir_name = luaL_checkstring(L, 1);
	const char *match    = luaL_checkstring(L, 2);

	if (! PHYSFS_exists(dir_name))
	{
		lua_pushnil(L);
		lua_pushstring(L, "No such directory");
		return 2;
	}

	char ** got_names = PHYSFS_enumerateFiles(dir_name);

	// seems this only happens on out-of-memory error
	if (! got_names)
	{
		return luaL_error(L, "gui.scan_directory: %s", PHYSFS_getLastError());
	}

	// transfer matching names into another list

	std::vector<std::string> list;

	char ** p;

	for (p = got_names ; *p ; p++)
	{
		if (scan_dir_process_name(*p, dir_name, match))
			list.push_back(*p);
	}

	PHYSFS_freeList(got_names);


	// sort into alphabetical order [ Note: not unicode aware ]

	std::sort(list.begin(), list.end(), scan_dir_nocase_CMP());

	// create the list of filenames / dirnames

	lua_newtable(L);

	for (unsigned int k = 0 ; k < list.size() ; k++)
	{
		lua_pushstring(L, list[k].c_str());
		lua_rawseti(L, -2, (int)(k + 1));
	}

	return 1;
}


// LUA: add_choice(button, id, label)
//
int gui_add_choice(lua_State *L)
{
	const char *button = luaL_checkstring(L,1);
	const char *id     = luaL_checkstring(L,2);
	const char *label  = luaL_checkstring(L,3);

	SYS_ASSERT(button && id && label);

//	DebugPrintf("  add_choice: %s id:%s\n", button, id);

	if (! main_win)
		return 0;

	// only allowed during startup
	if (has_added_buttons)
		Main_FatalError("Script problem: gui.add_choice called late.\n");

	if (! main_win->game_box->AddChoice(button, id, label))
		return luaL_error(L, "add_choice: unknown button '%s'\n", button);

	return 0;
}


// LUA: enable_choice(what, id, shown)
//
int gui_enable_choice(lua_State *L)
{
	const char *button = luaL_checkstring(L,1);
	const char *id     = luaL_checkstring(L,2);

	int enable = lua_toboolean(L,3) ? 1 : 0;

	SYS_ASSERT(button && id);

//	DebugPrintf("  enable_choice: %s id:%s %s\n", button, id, enable ? "enable" : "DISABLE");

	if (! main_win)
		return 0;

	if (! main_win->game_box->EnableChoice(button, id, enable))
		return luaL_error(L, "enable_choice: unknown button '%s'\n", button);

	return 0;
}


// LUA: set_button(button, id)
//
int gui_set_button(lua_State *L)
{
	const char *button = luaL_checkstring(L,1);
	const char *id     = luaL_checkstring(L,2);

	SYS_ASSERT(button && id);

//	DebugPrintf("  change_button: %s --> %s\n", button, id);

	if (! main_win)
		return 0;

	if (! main_win->game_box->SetButton(button, id))
		return luaL_error(L, "set_button: unknown button '%s'\n", button);

	return 0;
}


// LUA: add_module(where, id, label, tooltip)
//
int gui_add_module(lua_State *L)
{
	const char *where = luaL_checkstring(L,1);
	const char *id    = luaL_checkstring(L,2);
	const char *label = luaL_checkstring(L,3);
	const char *tip   = luaL_optstring  (L,4, NULL);

	SYS_ASSERT(where && id && label);

//	DebugPrintf("  add_module: %s id:%s\n", where, id);

	if (! main_win)
		return 0;

	// only allowed during startup
	if (has_added_buttons)
		Main_FatalError("Script problem: gui.add_module called late.\n");

	if (StringCaseCmp(where, "left") == 0)
		main_win->left_mods->AddModule(id, label, tip);

	else if (StringCaseCmp(where, "right") == 0)
		main_win->right_mods->AddModule(id, label, tip);

	else
		return luaL_error(L, "add_module: unknown where value '%s'\n", where);

	return 0;
}


// LUA: set_module(id, bool)
//
int gui_set_module(lua_State *L)
{
	const char *module = luaL_checkstring(L,1);

	int opt_val = lua_toboolean(L,2) ? 1 : 0;

	SYS_ASSERT(module);

//	DebugPrintf("  set_module: %s --> %s\n", module, opt_val);

	if (! main_win)
		return 0;

	// FIXME : error if module is unknown

	// try both columns
	main_win-> left_mods->EnableMod(module, opt_val);
	main_win->right_mods->EnableMod(module, opt_val);

	return 0;
}


// LUA: show_module(module, shown)
//
int gui_show_module(lua_State *L)
{
	const char *module = luaL_checkstring(L,1);

	int shown = lua_toboolean(L,2) ? 1 : 0;

	SYS_ASSERT(module);

//	DebugPrintf("  show_module: %s --> %s\n", what, module, shown ? "show" : "HIDE");

	if (! main_win)
		return 0;

	// FIXME : error if module is unknown

	main_win-> left_mods->ShowModule(module, shown);
	main_win->right_mods->ShowModule(module, shown);

	return 0;
}


// LUA: add_module_option(module, option, label, tooltip, gap)
//
int gui_add_module_option(lua_State *L)
{
	const char *module = luaL_checkstring(L,1);
	const char *option = luaL_checkstring(L,2);

	const char *label  = luaL_checkstring(L,3);
	const char *tip    = luaL_optstring  (L,4, NULL);

	int gap = luaL_optint(L,5, 0);

	SYS_ASSERT(module && option);

//	DebugPrintf("  add_module_option: %s.%s\n", module, option);

	if (! main_win)
		return 0;

	// only allowed during startup
	if (has_added_buttons)
		Main_FatalError("Script problem: gui.add_module_option called late.\n");

	// FIXME : error if module is unknown

	main_win-> left_mods->AddOption(module, option, label, tip, gap);
	main_win->right_mods->AddOption(module, option, label, tip, gap);

	return 0;
}


// LUA: add_option_choice(module, option, id, label)
//
int gui_add_option_choice(lua_State *L)
{
	const char *module = luaL_checkstring(L,1);
	const char *option = luaL_checkstring(L,2);

	const char *id     = luaL_checkstring(L,3);
	const char *label  = luaL_checkstring(L,4);

	SYS_ASSERT(module && option);

//	DebugPrintf("  add_option_choice: %s.%s\n", module, option);

	if (! main_win)
		return 0;

	// only allowed during startup
	if (has_added_buttons)
		Main_FatalError("Script problem: gui.add_option_choice called late.\n");

	// FIXME : error if module or option is unknown

	main_win-> left_mods->AddOptionChoice(module, option, id, label);
	main_win->right_mods->AddOptionChoice(module, option, id, label);

	return 0;
}


// LUA: set_module_option(module, option, value)
//
int gui_set_module_option(lua_State *L)
{
	const char *module = luaL_checkstring(L,1);
	const char *option = luaL_checkstring(L,2);
	const char *value  = luaL_checkstring(L,3);

	SYS_ASSERT(module && option && value);

//	DebugPrintf("  set_module_option: %s.%s --> %s\n", module, option, value);

	if (! main_win)
		return 0;

	if (StringCaseCmp(option, "self") == 0)
		return luaL_error(L, "set_module_option: cannot use 'self' here\n", option);

	if (! (main_win-> left_mods->SetOption(module, option, value) ||
		   main_win->right_mods->SetOption(module, option, value)))
	{
		return luaL_error(L, "set_module_option: unknown option '%s.%s'\n", module, option);
	}

	return 0;
}


// LUA: at_level(name, idx, total)
//
int gui_at_level(lua_State *L)
{
	const char *name = luaL_checkstring(L,1);

	int index = luaL_checkint(L, 2);
	int total = luaL_checkint(L, 3);

	Main_ProgStatus(_("Making %s"), name);

	if (main_win)
		main_win->build_box->Prog_AtLevel(index, total);

	return 0;
}


// LUA: prog_step(step_name)
//
int gui_prog_step(lua_State *L)
{
	const char *name = luaL_checkstring(L,1);

	if (main_win)
		main_win->build_box->Prog_Step(name);

	return 0;
}


// LUA: ticker()
//
int gui_ticker(lua_State *L)
{
	Main_Ticker();

	return 0;
}

// LUA: abort() --> boolean
//
int gui_abort(lua_State *L)
{
	int value = (main_action >= MAIN_CANCEL) ? 1 : 0;

	Main_Ticker();

	lua_pushboolean(L, value);
	return 1;
}


// LUA: rand_seed(seed)
//
int gui_rand_seed(lua_State *L)
{
	double the_seed = luaL_checknumber(L, 1);

	if (the_seed < 0)
		the_seed = - the_seed;

	double A = fmod(the_seed, 1073741824.0);
	the_seed = (the_seed - A) / 1073741824.0;

	double B = fmod(the_seed, 1073741824.0);
	the_seed = (the_seed - B) / 1073741824.0;

	// s1 and s2 are the most important
	// s3 and s4 are much less significant
	uint32_t s1 = (uint32_t)A & 0x55555555;
	uint32_t s2 = (uint32_t)A & 0x2AAAAAAA;
	uint32_t s3 = (uint32_t)B ^ s1;
	uint32_t s4 = s2 >> 11;

	GUI_RNG.FullSeed(s1, s2, s3, s4);

	return 0;
}

// LUA: random() --> number
//
int gui_random(lua_State *L)
{
	lua_Number value = GUI_RNG.Double();

	lua_pushnumber(L, value);
	return 1;
}


// LUA: bit_and(A, B) --> number
//
int gui_bit_and(lua_State *L)
{
	int A = luaL_checkint(L, 1);
	int B = luaL_checkint(L, 2);

	lua_pushinteger(L, A & B);
	return 1;
}

// LUA: bit_test(val) --> boolean
//
int gui_bit_test(lua_State *L)
{
	int A = luaL_checkint(L, 1);
	int B = luaL_checkint(L, 2);

	lua_pushboolean(L, (A & B) != 0);
	return 1;
}

// LUA: bit_or(A, B) --> number
//
int gui_bit_or(lua_State *L)
{
	int A = luaL_checkint(L, 1);
	int B = luaL_checkint(L, 2);

	lua_pushinteger(L, A | B);
	return 1;
}

// LUA: bit_xor(A, B) --> number
//
int gui_bit_xor(lua_State *L)
{
	int A = luaL_checkint(L, 1);
	int B = luaL_checkint(L, 2);

	lua_pushinteger(L, A ^ B);
	return 1;
}

// LUA: bit_not(val) --> number
//
int gui_bit_not(lua_State *L)
{
	int A = luaL_checkint(L, 1);

	// do not make the result negative
	lua_pushinteger(L, (~A) & 0x7FFFFFFF);
	return 1;
}


int gui_minimap_begin(lua_State *L)
{
	// dummy size when running in batch mode
	int map_W = 50;
	int map_H = 50;

	if (main_win)
	{
		map_W = main_win->build_box->mini_map->GetWidth();
		map_H = main_win->build_box->mini_map->GetHeight();

		main_win->build_box->mini_map->MapBegin();
	}

	lua_pushinteger(L, map_W);
	lua_pushinteger(L, map_H);

	return 2;
}

int gui_minimap_finish(lua_State *L)
{
	if (main_win)
		main_win->build_box->mini_map->MapFinish();

	return 0;
}

int gui_minimap_draw_line(lua_State *L)
{
	int x1 = luaL_checkint(L, 1);
	int y1 = luaL_checkint(L, 2);

	int x2 = luaL_checkint(L, 3);
	int y2 = luaL_checkint(L, 4);

	const char *color_str = luaL_checkstring(L, 5);

	int r = 255;
	int g = 255;
	int b = 255;

	sscanf(color_str, "#%2x%2x%2x", &r, &g, &b);

	if (main_win)
	{
		main_win->build_box->mini_map->DrawLine(x1,y1, x2,y2, (u8_t)r, (u8_t)g, (u8_t)b);
	}

	return 0;
}

int gui_minimap_fill_box(lua_State *L)
{
	int x1 = luaL_checkint(L, 1);
	int y1 = luaL_checkint(L, 2);

	int x2 = luaL_checkint(L, 3);
	int y2 = luaL_checkint(L, 4);

	const char *color_str = luaL_checkstring(L, 5);

	int r = 255;
	int g = 255;
	int b = 255;

	sscanf(color_str, "#%2x%2x%2x", &r, &g, &b);

	if (main_win)
	{
		main_win->build_box->mini_map->DrawBox(x1,y1, x2,y2, (u8_t)r, (u8_t)g, (u8_t)b);
	}

	return 0;
}


//------------------------------------------------------------------------


extern int SPOT_begin(lua_State *L);
extern int SPOT_draw_line(lua_State *L);
extern int SPOT_fill_poly(lua_State *L);
extern int SPOT_fill_box(lua_State *L);
extern int SPOT_apply_brushes(lua_State *L);
extern int SPOT_dump(lua_State *L);
extern int SPOT_get_mons(lua_State *L);
extern int SPOT_get_items(lua_State *L);
extern int SPOT_end(lua_State *L);

extern int CSG_begin_level(lua_State *L);
extern int CSG_end_level(lua_State *L);
extern int CSG_property(lua_State *L);
extern int CSG_tex_property(lua_State *L);
extern int CSG_add_brush(lua_State *L);
extern int CSG_add_entity(lua_State *L);
extern int CSG_trace_ray(lua_State *L);

extern int WF_wolf_block(lua_State *L);
extern int WF_wolf_read(lua_State *L);

extern int DM_wad_name_gfx(lua_State *L);
extern int DM_wad_logo_gfx(lua_State *L);

extern int DM_wad_add_text_lump(lua_State *L);
extern int DM_wad_add_binary_lump(lua_State *L);
extern int DM_wad_insert_file(lua_State *L);
extern int DM_wad_transfer_lump(lua_State *L);
extern int DM_wad_transfer_map(lua_State *L);
extern int DM_wad_merge_sections(lua_State *L);
extern int DM_wad_read_text_lump(lua_State *L);

extern int DM_fsky_create(lua_State *L);
extern int DM_fsky_write(lua_State *L);
extern int DM_fsky_solid_box(lua_State *L);
extern int DM_fsky_add_stars(lua_State *L);
extern int DM_fsky_add_clouds(lua_State *L);
extern int DM_fsky_add_hills(lua_State *L);

extern int DM_title_create(lua_State *L);
extern int DM_title_free(lua_State *L);
extern int DM_title_write(lua_State *L);
extern int DM_title_set_palette(lua_State *L);
extern int DM_title_property(lua_State *L);
extern int DM_title_draw_line(lua_State *L);
extern int DM_title_draw_rect(lua_State *L);
extern int DM_title_draw_disc(lua_State *L);
extern int DM_title_draw_clouds(lua_State *L);
extern int DM_title_draw_planet(lua_State *L);
extern int DM_title_load_image(lua_State *L);

extern int wadfab_load(lua_State *L);
extern int wadfab_free(lua_State *L);
extern int wadfab_get_polygon(lua_State *L);
extern int wadfab_get_sector(lua_State *L);
extern int wadfab_get_side(lua_State *L);
extern int wadfab_get_line(lua_State *L);
extern int wadfab_get_3d_floor(lua_State *L);
extern int wadfab_get_thing(lua_State *L);

extern int Q1_add_mapmodel(lua_State *L);
extern int Q1_add_tex_wad(lua_State *L);


static const luaL_Reg gui_script_funcs[] =
{
	{ "raw_log_print",     gui_raw_log_print },
	{ "raw_debug_print",   gui_raw_debug_print },

	{ "gettext",        gui_gettext },
	{ "config_line",    gui_config_line },
	{ "set_colormap",   gui_set_colormap },

	{ "add_choice",     gui_add_choice },
	{ "enable_choice",  gui_enable_choice },
	{ "set_button",     gui_set_button },

	{ "add_module",   gui_add_module },
	{ "show_module",  gui_show_module },
	{ "set_module",   gui_set_module },

	{ "add_module_option", gui_add_module_option },
	{ "add_option_choice", gui_add_option_choice },
	{ "set_module_option", gui_set_module_option },

	{ "at_level",    gui_at_level },
	{ "prog_step",   gui_prog_step },
	{ "ticker",      gui_ticker },
	{ "abort",       gui_abort },
	{ "rand_seed",   gui_rand_seed },
	{ "random",      gui_random },

	// file & directory functions
	{ "import",          gui_import },
	{ "set_import_dir",  gui_set_import_dir },
	{ "get_install_dir", gui_get_install_dir },
	{ "scan_directory",  gui_scan_directory },
	{ "mkdir",           gui_mkdir },

	// CSG functions
	{ "begin_level", CSG_begin_level },
	{ "end_level",   CSG_end_level },
	{ "property",    CSG_property },
	{ "tex_property",CSG_tex_property },
	{ "add_brush",   CSG_add_brush  },
	{ "add_entity",  CSG_add_entity },
	{ "trace_ray",   CSG_trace_ray },

	// Mini-Map functions
	{ "minimap_begin",     gui_minimap_begin },
	{ "minimap_finish",    gui_minimap_finish },
	{ "minimap_draw_line", gui_minimap_draw_line },
	{ "minimap_fill_box",  gui_minimap_fill_box },

	// Wolf-3D functions
	{ "wolf_block",     WF_wolf_block },
	{ "wolf_read",      WF_wolf_read },

	// Doom/Heretic/Hexen functions
	{ "wad_name_gfx",   DM_wad_name_gfx  },
	{ "wad_logo_gfx",   DM_wad_logo_gfx  },
	{ "wad_add_text_lump",   DM_wad_add_text_lump },
	{ "wad_add_binary_lump", DM_wad_add_binary_lump },

	{ "wad_insert_file",   DM_wad_insert_file },
	{ "wad_transfer_lump", DM_wad_transfer_lump },
	{ "wad_transfer_map",  DM_wad_transfer_map },
	{ "wad_merge_sections",DM_wad_merge_sections },
	{ "wad_read_text_lump",DM_wad_read_text_lump },

	{ "fsky_create",      DM_fsky_create },
	{ "fsky_write",       DM_fsky_write },
	{ "fsky_solid_box",   DM_fsky_solid_box },
	{ "fsky_add_stars",   DM_fsky_add_stars },
	{ "fsky_add_clouds",  DM_fsky_add_clouds },
	{ "fsky_add_hills",   DM_fsky_add_hills },

	{ "title_create",      DM_title_create },
	{ "title_free",        DM_title_free },
	{ "title_write",       DM_title_write },
	{ "title_set_palette", DM_title_set_palette },
	{ "title_prop",        DM_title_property },
	{ "title_draw_line",   DM_title_draw_line },
	{ "title_draw_rect",   DM_title_draw_rect },
	{ "title_draw_disc",   DM_title_draw_disc },
	{ "title_draw_clouds", DM_title_draw_clouds },
	{ "title_draw_planet", DM_title_draw_planet },
	{ "title_load_image",  DM_title_load_image },

	{ "wadfab_load",         wadfab_load },
	{ "wadfab_free",         wadfab_free },
	{ "wadfab_get_polygon",  wadfab_get_polygon },
	{ "wadfab_get_sector",   wadfab_get_sector },
	{ "wadfab_get_side",     wadfab_get_side },
	{ "wadfab_get_line",     wadfab_get_line },
	{ "wadfab_get_3d_floor", wadfab_get_3d_floor },
	{ "wadfab_get_thing",    wadfab_get_thing },

	// Quake functions
	{ "q1_add_mapmodel",  Q1_add_mapmodel },
	{ "q1_add_tex_wad",   Q1_add_tex_wad },

	// SPOT functions
	{ "spots_begin",     SPOT_begin },
	{ "spots_draw_line", SPOT_draw_line },
	{ "spots_fill_poly", SPOT_fill_poly },
	{ "spots_fill_box",  SPOT_fill_box },
	{ "spots_apply_brushes", SPOT_apply_brushes },
	{ "spots_dump",      SPOT_dump },
	{ "spots_get_mons",  SPOT_get_mons },
	{ "spots_get_items", SPOT_get_items },
	{ "spots_end",       SPOT_end },

	{ NULL, NULL } // the end
};


static const luaL_Reg bit_functions[] =
{
	{ "band",    gui_bit_and },
	{ "btest",   gui_bit_test },
	{ "bor",     gui_bit_or  },
	{ "bxor",    gui_bit_xor },
	{ "bnot",    gui_bit_not },

	{ NULL, NULL } // the end
};


int Script_RegisterLib(const char *name, const luaL_Reg *reg)
{
	SYS_NULL_CHECK(LUA_ST);

	luaL_register(LUA_ST, name, reg);

	// remove the table which luaL_register created
	lua_pop(LUA_ST, 1);

	return 0;
}


static int p_init_lua(lua_State *L)
{
	/* stop collector during initialization */
	lua_gc(L, LUA_GCSTOP, 0);
	{
		luaL_openlibs(L);  /* open libraries */

		Script_RegisterLib("gui", gui_script_funcs);
		Script_RegisterLib("bit", bit_functions);
	}
	lua_gc(L, LUA_GCRESTART, 0);

	return 0;
}


static bool Script_CallFunc(const char *func_name, int nresult = 0, const char **params = NULL)
{
	// Note: the results of the function will be on the Lua stack

	lua_getglobal(LUA_ST, "ob_traceback");

	if (lua_type(LUA_ST, -1) == LUA_TNIL)
		Main_FatalError("Script problem: missing function '%s'", "ob_traceback");

	lua_getglobal(LUA_ST, func_name);

	if (lua_type(LUA_ST, -1) == LUA_TNIL)
		Main_FatalError("Script problem: missing function '%s'", func_name);

	int nargs = 0;
	if (params)
	{
		for ( ; *params ; params++, nargs++)
			lua_pushstring(LUA_ST, *params);
	}

	int status = lua_pcall(LUA_ST, nargs, nresult, -2-nargs);
	if (status != 0)
	{
		const char *msg = lua_tolstring(LUA_ST, -1, NULL);

		// skip the filename
		const char *err_msg = strstr(msg, ": ");
		if (err_msg)
			err_msg += 2;
		else
			err_msg = msg;

		// this will appear in the log file too

		DLG_ShowError(_("Script Error: %s"), err_msg);

		lua_pop(LUA_ST, 2);  // ob_traceback, message
		return false;
	}

	// remove the traceback function
	lua_remove(LUA_ST, -1-nresult);

	return true;
}


/* UNUSED
bool Script_RunString(const char *str, ...)
{
	static char buffer[MSG_BUF_LEN];

	va_list args;

	va_start(args, str);
	vsnprintf(buffer, MSG_BUF_LEN-1, str, args);
	va_end(args);

	buffer[MSG_BUF_LEN-2] = 0;


	lua_getglobal(LUA_ST, "ob_traceback");

	if (lua_type(LUA_ST, -1) == LUA_TNIL)
		Main_FatalError("Script problem: missing function '%s'", "ob_traceback");

	int status = luaL_loadbuffer(LUA_ST, buffer, strlen(buffer), "=CONSOLE");

	if (status != 0)
	{
		// const char *msg = lua_tolstring(LUA_ST, -1, NULL);

		ConPrintf("Error: @1Bad Syntax or Unknown Command\n");

		lua_pop(LUA_ST, 2);  // ob_traceback, message
		return false;
	}

	status = lua_pcall(LUA_ST, 0, 0, -2);
	if (status != 0)
	{
		const char *msg = lua_tolstring(LUA_ST, -1, NULL);

		// skip the filename
		const char *err_msg = strstr(msg, ": ");
		if (err_msg)
			err_msg += 2;
		else
			err_msg = msg;

		LogPrintf("\nScript Error: %s\n", err_msg);

		lua_pop(LUA_ST, 2);  // ob_traceback, message
		return false;
	}

	lua_pop(LUA_ST, 1);  // ob_traceback
	return true;
}
*/


typedef struct load_info_t
{
	PHYSFS_File *fp;
	char *error_msg;
	char buffer[2048];

} load_info_t;


static const char * my_reader(lua_State *L, void *ud, size_t *size)
{
	(void)L;

	load_info_t *info = (load_info_t *)ud;

	if (PHYSFS_eof(info->fp))
		return NULL;

	PHYSFS_sint64 len = PHYSFS_read(info->fp, info->buffer, 1, sizeof(info->buffer));

	// negative result indicates a "complete failure"
	if (len < 0)
	{
		info->error_msg = StringDup(PHYSFS_getLastError());
		len = 0;
	}

	*size = (size_t)len;

	if (! size)
		return NULL;

	return info->buffer;  // OK
}


static int my_loadfile(lua_State *L, const char *filename)
{
	/* index of filename on the stack */
	int fnameindex = lua_gettop(L) + 1;

	lua_pushfstring(L, "@%s", filename);

	load_info_t info;

	info.fp = PHYSFS_openRead(filename);
	info.error_msg = NULL;

	if (! info.fp)
	{
		lua_pushfstring(L, "file open error: %s", PHYSFS_getLastError());
		lua_remove(L, fnameindex);

		return LUA_ERRFILE;
	}

	int status = lua_load(L, my_reader, &info, lua_tostring(L, -1));

	/* close file (even in case of errors) */
	PHYSFS_close(info.fp);

	if (info.error_msg)
	{
		/* ignore results from 'lua_load' */
		lua_settop(L, fnameindex);
		status = LUA_ERRFILE;

		lua_pushfstring(L, "file read error: %s", info.error_msg);
	}

	lua_remove(L, fnameindex);

	return status;
}


void Script_Load(const char *script_name)
{
	SYS_ASSERT(import_dir);
	SYS_ASSERT(import_dir[0]);

	// add extension if missing
	if (! HasExtension(script_name))
	{
		script_name = ReplaceExtension(script_name, "lua");
	}

	char *filename = StringPrintf("%s/%s", import_dir, script_name);

	DebugPrintf("  loading script: '%s'\n", filename);

	int status = my_loadfile(LUA_ST, filename);

	if (status == 0)
		status = lua_pcall(LUA_ST, 0, 0, 0);

	if (status != 0)
	{
		const char *msg = lua_tolstring(LUA_ST, -1, NULL);

		Main_FatalError("Unable to load script '%s'\n%s", filename, msg);
	}

	StringFree(filename);
}


void Script_Open()
{
	LogPrintf("\n--- OPENING LUA VM ---\n\n");


	// create Lua state

	LUA_ST = luaL_newstate();
	if (! LUA_ST)
		Main_FatalError("LUA Init failed: cannot create new state");

	int status = lua_cpcall(LUA_ST, &p_init_lua, NULL);
	if (status != 0)
		Main_FatalError("LUA Init failed: cannot load standard libs (%d)", status);


	// load main scripts

	LogPrintf("Loading main script: oblige.lua\n");

	import_dir = StringDup("scripts");

	Script_Load("oblige.lua");

	has_loaded = true;
	LogPrintf("DONE.\n\n");


	// ob_init() will load all the game-specific scripts, engine scripts, and
	// module scripts.

	if (! Script_CallFunc("ob_init"))
		Main_FatalError("The ob_init script failed.\n");

	has_added_buttons = true;
}


void Script_Close()
{
	if (LUA_ST)
		lua_close(LUA_ST);

	LUA_ST = NULL;

	LogPrintf("\n--- CLOSED LUA VM ---\n\n");
}


//------------------------------------------------------------------------
// WRAPPERS TO LUA FUNCTIONS
//------------------------------------------------------------------------


bool ob_set_config(const char *key, const char *value)
{
	// See the document 'doc/Config_Flow.txt' for a good
	// description of the flow of configuration values
	// between the C++ GUI and the Lua scripts.

	SYS_NULL_CHECK(key);
	SYS_NULL_CHECK(value);

	if (! has_loaded)
	{
		DebugPrintf("ob_set_config(%s) called before loaded!\n", key);
		return false;
	}

	const char *params[3];

	params[0] = key;
	params[1] = value;
	params[2] = NULL; // end of list

	return Script_CallFunc("ob_set_config", 0, params);
}


bool ob_set_mod_option(const char *module, const char *option,
		const char *value)
{
	if (! has_loaded)
	{
		DebugPrintf("ob_set_mod_option() called before loaded!\n");
		return false;
	}

	const char *params[4];

	params[0] = module;
	params[1] = option;
	params[2] = value;
	params[3] = NULL;

	return Script_CallFunc("ob_set_mod_option", 0, params);
}


bool ob_read_all_config(std::vector<std::string> * lines, bool need_full)
{
	if (! has_loaded)
	{
		DebugPrintf("ob_read_all_config() called before loaded!\n");
		return false;
	}

	conf_line_buffer = lines;

	const char *params[2];

	params[0] = need_full ? "need_full" : "";
	params[1] = NULL; // end of list

	bool result = Script_CallFunc("ob_read_all_config", 0, params);

	conf_line_buffer = NULL;

	return result;
}


const char * ob_game_format()
{
	if (! Script_CallFunc("ob_game_format", 1))
		return NULL;

	const char *res = lua_tolstring(LUA_ST, -1, NULL);

	if (res)
		res = StringDup(res);

	// remove result from lua stack
	lua_pop(LUA_ST, 1);

	return res;
}


const char * ob_default_filename()
{
	if (! Script_CallFunc("ob_default_filename", 1))
		return NULL;

	const char *res = lua_tolstring(LUA_ST, -1, NULL);

	if (res)
		res = StringDup(res);

	// remove result from lua stack
	lua_pop(LUA_ST, 1);

	return res;
}


bool ob_build_cool_shit()
{
	if (! Script_CallFunc("ob_build_cool_shit", 1))
	{
		Main_ProgStatus(_("Script Error"));
		return false;
	}

	const char *res = lua_tolstring(LUA_ST, -1, NULL);

	// remove result from lua stack
	lua_pop(LUA_ST, 1);

	if (res && strcmp(res, "ok") == 0)
		return true;

	Main_ProgStatus(_("Cancelled"));
	return false;
}


//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
