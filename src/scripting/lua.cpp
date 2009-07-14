/* $Id$ */
/*
   Copyright (C) 2009 by Guillaume Melquiond <guillaume.melquiond@gmail.com>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2
   or at your option any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 * @file scripting/lua.cpp
 * Provides a Lua interpreter.
 *
 * @warning Lua's error handling is done by setjmp/longjmp, so be careful
 *   never to call a Lua error function that will jump while in the scope
 *   of a C++ object with a destructor. This is why this file uses goto-s
 *   to force object unscoping before erroring out.
 */

#ifdef DISABLE_LUA
#include "log.hpp"
#include "scripting/lua.hpp"
static lg::log_domain lua("scripting/lua");
LuaKernel::LuaKernel() {LOG_STREAM(err, lua) << "Lua support disabled in this build";}
LuaKernel::~LuaKernel() {}
void LuaKernel::run_event(vconfig const&, game_events::queued_event const&) {}
void LuaKernel::run(char const *prog) {}
void LuaKernel::execute(char const *prog, int nArgs, int nRets) {}
#else  // HAVE LUA

extern "C" {
#include <lualib.h>
#include <lauxlib.h>
}

#include <cassert>
#include <cstring>

#include "filesystem.hpp"
#include "foreach.hpp"
#include "gamestatus.hpp"
#include "log.hpp"
#include "map.hpp"
#include "scripting/lua.hpp"
#include "terrain_translation.hpp"
#include "unit.hpp"

static lg::log_domain log_scripting_lua("scripting/lua");
#define LOG_LUA LOG_STREAM(info, log_scripting_lua)
#define ERR_LUA LOG_STREAM(err, log_scripting_lua)

/* Dummy pointer for getting unique keys for Lua's registry. */
static char const executeKey = 0;
static char const getsideKey = 0;
static char const gettextKey = 0;
static char const gettypeKey = 0;
static char const getunitKey = 0;
static char const tstringKey = 0;
static char const uactionKey = 0;

/**
 * Pushes a t_string on the top of the stack.
 */
static void lua_pushtstring(lua_State *L, t_string const &v)
{
	new(lua_newuserdata(L, sizeof(t_string))) t_string(v);
	lua_pushlightuserdata(L, (void *)&tstringKey);
	lua_gettable(L, LUA_REGISTRYINDEX);
	lua_setmetatable(L, -2);
}

/**
 * Converts a string into a Lua object pushed at the top of the stack.
 * Boolean ("yes"/"no") and numbers are detected and typed accordingly.
 */
static void scalar_of_wml_string(lua_State *L, t_string const &v)
{
	if (!v.translatable())
	{
		char *pe;
		char const *pb = v.c_str();
		double d = strtod(v.c_str(), &pe);
		if (pe != pb && *pe == '\0')
			lua_pushnumber(L, d);
		else if (v == "yes")
			lua_pushboolean(L, true);
		else if (v == "no")
			lua_pushboolean(L, false);
		else
			lua_pushstring(L, pb);
	}
	else
	{
		lua_pushtstring(L, v);
	}
}

/**
 * Converts a config object to a Lua table.
 * The destination table should be at the top of the stack on entry. It is
 * still at the top on exit.
 */
static void table_of_wml_config(lua_State *L, config const &cfg)
{
	if (!lua_checkstack(L, LUA_MINSTACK))
		return;

	int k = 1;
	foreach (const config::any_child &ch, cfg.all_children_range())
	{
		lua_createtable(L, 2, 0);
		lua_pushstring(L, ch.key.c_str());
		lua_rawseti(L, -2, 1);
		lua_newtable(L);
		table_of_wml_config(L, ch.cfg);
		lua_rawseti(L, -2, 2);
		lua_rawseti(L, -2, k++);
	}
	foreach (const config::attribute &attr, cfg.attribute_range())
	{
		scalar_of_wml_string(L, attr.second);
		lua_setfield(L, -2, attr.first.c_str());
	}
}

#define return_misformed() \
  do { lua_settop(L, initial_top); return false; } while (0)

/**
 * Converts a Lua table to a config object.
 * The source table should be at the top of the stack on entry. It is
 * still at the top on exit.
 * @param tstring_meta absolute stack position of t_string's metatable, or 0 if none.
 * @return false if some attributes had not the proper type.
 * @note If the table has holes in the integer keys or floating-point keys,
 *       some keys will be ignored and the error will go undetected.
 */
static bool wml_config_of_table(lua_State *L, config &cfg, int tstring_meta = 0)
{
	if (!lua_checkstack(L, LUA_MINSTACK))
		return false;

	// Get t_string's metatable, so that it can be used later to detect t_string object.
	int initial_top = lua_gettop(L);
	if (!tstring_meta) {
		lua_pushlightuserdata(L, (void *)&tstringKey);
		lua_gettable(L, LUA_REGISTRYINDEX);
		tstring_meta = lua_gettop(L);
		lua_pushvalue(L, -2);
	}

	// First convert the children (integer indices).
	for (int i = 1, i_end = lua_objlen(L, -1); i <= i_end; ++i)
	{
		lua_rawgeti(L, -1, i);
		if (!lua_istable(L, -1)) return_misformed();
		lua_rawgeti(L, -1, 1);
		char const *m = lua_tostring(L, -1);
		if (!m) return_misformed();
		lua_rawgeti(L, -2, 2);
		if (!lua_istable(L, -1) ||
		    !wml_config_of_table(L, cfg.add_child(m), tstring_meta))
			return_misformed();
		lua_pop(L, 3);
	}

	// Then convert the attributes (string indices).
	for (lua_pushnil(L); lua_next(L, -2); lua_pop(L, 1))
	{
		if (lua_isnumber(L, -2)) continue;
		if (!lua_isstring(L, -2)) return_misformed();
		t_string v;
		switch (lua_type(L, -1)) {
			case LUA_TBOOLEAN:
				v = lua_toboolean(L, -1) ? "yes" : "no";
				break;
			case LUA_TNUMBER:
			case LUA_TSTRING:
				v = lua_tostring(L, -1);
				break;
			case LUA_TUSERDATA:
			{
				if (!lua_getmetatable(L, -1)) return_misformed();
				bool tstr = lua_rawequal(L, -1, tstring_meta) != 0;
				lua_pop(L, 1);
				if (!tstr) return_misformed();
				v = *static_cast<t_string *>(lua_touserdata(L, -1));
				break;
			}
			default:
				return_misformed();
		}
		cfg[lua_tostring(L, -2)] = v;
	}

	lua_settop(L, initial_top);
	return true;
}

#undef return_misformed

/**
 * Creates a t_string object (__call metamethod).
 * - Arg 1: userdata containing the domain.
 * - Arg 2: string to translate.
 * - Ret 1: string containing the translatable string.
 */
static int lua_gettext(lua_State *L)
{
	char const *m = luaL_checkstring(L, 2);
	char const *d = static_cast<char *>(lua_touserdata(L, 1));
	// Hidden metamethod, so d has to be a string. Use it to create a t_string.
	lua_pushtstring(L, t_string(m, d));
	return 1;
}

/**
 * Creates an interface for gettext
 * - Arg 1: string containing the domain.
 * - Ret 1: a full userdata with __call pointing to lua_gettext.
 */
static int lua_textdomain(lua_State *L)
{
	size_t l;
	char const *m = luaL_checklstring(L, 1, &l);
	void *p = lua_newuserdata(L, l + 1);
	memcpy(p, m, l + 1);
	lua_pushlightuserdata(L, (void *)&gettextKey);
	lua_gettable(L, LUA_REGISTRYINDEX);
	lua_setmetatable(L, -2);
	return 1;
}

/**
 * Converts a Lua value at position @a src and appends it to @a dst.
 * @note This function is private to lua_tstring_concat. It expects two things.
 *       First, the t_string metatable is at the top of the stack on entry. (It
 *       is still there on exit.) Second, the caller hasn't any valuable object
 *       with dynamic lifetime, since they would leaked on error.
 */
static void lua_tstring_concat_aux(lua_State *L, t_string &dst, int src)
{
	switch (lua_type(L, src)) {
		case LUA_TNUMBER:
		case LUA_TSTRING:
			dst += lua_tostring(L, src);
			break;
		case LUA_TUSERDATA:
			// Compare its metatable with t_string's metatable.
			if (!lua_getmetatable(L, src) || !lua_rawequal(L, -1, -2))
				luaL_typerror(L, src, "string");
			dst += *static_cast<t_string *>(lua_touserdata(L, src));
			lua_pop(L, 1);
			break;
		default:
			luaL_typerror(L, src, "string");
	}
}

/**
 * Appends a scalar to a t_string object (__concat metamethod).
 */
static int lua_tstring_concat(lua_State *L)
{
	// Create a new t_string.
	t_string *t = new(lua_newuserdata(L, sizeof(t_string))) t_string;

	lua_pushlightuserdata(L, (void *)&tstringKey);
	lua_gettable(L, LUA_REGISTRYINDEX);

	// Append both arguments to t.
	lua_tstring_concat_aux(L, *t, 1);
	lua_tstring_concat_aux(L, *t, 2);

	lua_setmetatable(L, -2);
	return 1;
}

/**
 * Destroys a t_string object before it is collected (__gc metamethod).
 */
static int lua_tstring_collect(lua_State *L)
{
	t_string *t = static_cast<t_string *>(lua_touserdata(L, 1));
	t->t_string::~t_string();
	return 0;
}

/**
 * Converts a t_string object to a string (__tostring metamethod);
 * that is, performs a translation.
 */
static int lua_tstring_tostring(lua_State *L)
{
	t_string *t = static_cast<t_string *>(lua_touserdata(L, 1));
	lua_pushstring(L, t->c_str());
	return 1;
}

#define return_tstring_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		lua_pushtstring(L, accessor); \
		return 1; \
	}

#define return_string_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		lua_pushstring(L, accessor.c_str()); \
		return 1; \
	}

#define return_int_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		lua_pushinteger(L, accessor); \
		return 1; \
	}

#define return_bool_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		lua_pushboolean(L, accessor); \
		return 1; \
	}

#define return_cfg_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		config cfg; \
		accessor; \
		lua_newtable(L); \
		table_of_wml_config(L, cfg); \
		return 1; \
	}

#define return_cfgref_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		lua_newtable(L); \
		table_of_wml_config(L, accessor); \
		return 1; \
	}

#define modify_tstring_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		if (lua_type(L, -1) == LUA_TUSERDATA) { \
			lua_pushlightuserdata(L, (void *)&tstringKey); \
			lua_gettable(L, LUA_REGISTRYINDEX); \
			if (!lua_getmetatable(L, -2) || !lua_rawequal(L, -1, -2)) \
				return luaL_typerror(L, -3, "(translatable) string"); \
			const t_string &value = *static_cast<t_string *>(lua_touserdata(L, -3)); \
			accessor; \
		} else { \
			const char *value = luaL_checkstring(L, -1); \
			accessor; \
		} \
		return 0; \
	}

#define modify_string_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		const char *value = luaL_checkstring(L, -1); \
		accessor; \
		return 0; \
	}

#define modify_int_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		int value = luaL_checkint(L, -1); \
		accessor; \
		return 0; \
	}

#define modify_bool_attrib(name, accessor) \
	if (strcmp(m, name) == 0) { \
		int value = lua_toboolean(L, -1); \
		accessor; \
		return 0; \
	}

/**
 * Gets some data on a unit type (__index metamethod).
 * - Arg 1: table containing an "id" field.
 * - Arg 2: string containing the name of the property.
 * - Ret 1: something containing the attribute.
 */
static int lua_unit_type_get(lua_State *L)
{
	char const *m = luaL_checkstring(L, 2);
	lua_getfield(L, 1, "id");
	unit_type_data::unit_type_map_wrapper &ut_map = unit_type_data::types();
	unit_type_data::unit_type_map::const_iterator
		uti = ut_map.find_unit_type(lua_tostring(L, -1));
	if (uti == ut_map.end()) return 0;
	unit_type const &ut = uti->second;

	// Find the corresponding attribute.
	return_tstring_attrib("name", ut.type_name());
	return_int_attrib("max_hitpoints", ut.hitpoints());
	return_int_attrib("max_moves", ut.movement());
	return_int_attrib("max_experience", ut.experience_needed());
	return_int_attrib("cost", ut.cost());
	return_int_attrib("level", ut.level());
	return_cfgref_attrib("__cfg", ut.get_cfg());
	return 0;
}

/**
 * Gets the unit type corresponding to an id.
 * - Arg 1: string containing the unit type id.
 * - Ret 1: table with an "id" field and with __index pointing to lua_unit_type_get.
 */
static int lua_get_unit_type(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);
	unit_type_data::unit_type_map_wrapper &ut_map = unit_type_data::types();
	if (!ut_map.unit_type_exists(m)) return 0;

	lua_createtable(L, 0, 1);
	lua_pushvalue(L, 1);
	lua_setfield(L, -2, "id");
	lua_pushlightuserdata(L, (void *)&gettypeKey);
	lua_gettable(L, LUA_REGISTRYINDEX);
	lua_setmetatable(L, -2);
	return 1;
}

/**
 * Gets the ids of all the unit types.
 * - Ret 1: table containing the ids.
 */
static int lua_get_unit_type_ids(lua_State *L)
{
	lua_newtable(L);
	int i = 1;
	unit_type_data::unit_type_map_wrapper &ut_map = unit_type_data::types();
	for (unit_type_data::unit_type_map::const_iterator uti = ut_map.begin(),
	     uti_end = ut_map.end(); uti != uti_end; ++uti)
	{
		lua_pushstring(L, uti->first.c_str());
		lua_rawseti(L, -2, i);
		++i;
	}
	return 1;
}

/**
 * Gets some data on a unit (__index metamethod).
 * - Arg 1: full userdata containing the unit id.
 * - Arg 2: string containing the name of the property.
 * - Ret 1: something containing the attribute.
 */
static int lua_unit_get(lua_State *L)
{
	size_t id = *static_cast<size_t *>(lua_touserdata(L, 1));
	char const *m = luaL_checkstring(L, 2);

	unit_map::const_unit_iterator ui = game_events::resources->units->find(id);
	if (!ui.valid()) return 0;
	unit const &u = ui->second;

	// Find the corresponding attribute.
	return_int_attrib("x", ui->first.x);
	return_int_attrib("y", ui->first.y);
	return_int_attrib("side", u.side());
	return_string_attrib("id", u.id());
	return_int_attrib("hitpoints", u.hitpoints());
	return_int_attrib("max_hitpoints", u.max_hitpoints());
	return_int_attrib("experience", u.experience());
	return_int_attrib("max_experience", u.max_experience());
	return_int_attrib("moves", u.movement_left());
	return_int_attrib("max_moves", u.total_movement());
	return_tstring_attrib("name", u.name());
	return_string_attrib("side_id", u.side_id());
	return_bool_attrib("canrecruit", u.can_recruit());
	return_bool_attrib("petrified", u.incapacitated());
	return_bool_attrib("resting", u.resting());
	return_string_attrib("role", u.get_role());
	return_string_attrib("facing", map_location::write_direction(u.facing()));
	return_cfg_attrib("__cfg", u.write(cfg));
	return 0;
}

/**
 * Sets some data on a unit (__newindex metamethod).
 * - Arg 1: full userdata containing the unit id.
 * - Arg 2: string containing the name of the property.
 * - Arg 3: something containing the attribute.
 */
static int lua_unit_set(lua_State *L)
{
	size_t id = *static_cast<size_t *>(lua_touserdata(L, 1));
	char const *m = luaL_checkstring(L, 2);
	lua_settop(L, 3);

	unit_map::unit_iterator ui = game_events::resources->units->find(id);
	if (!ui.valid()) return 0;
	unit &u = ui->second;

	// Find the corresponding attribute.
	modify_int_attrib("side", u.set_side(value));
	modify_int_attrib("moves", u.set_movement(value));
	modify_bool_attrib("resting", u.set_resting(value != 0));
	modify_tstring_attrib("name", u.set_name(value));
	modify_string_attrib("role", u.set_role(value));
	modify_string_attrib("facing", u.set_facing(map_location::parse_direction(value)));
	return 0;
}

/**
 * Gets the numeric ids of all the units.
 * - Arg 1: optional table containing a filter
 * - Ret 1: table containing full userdata with __index pointing to
 *          lua_unit_get and __newindex pointing to lua_unit_set.
 */
static int lua_get_units(lua_State *L)
{
	bool has_filter = lua_gettop(L) >= 1;
	if (has_filter && !lua_istable(L, 1)) {
		error_call_destructors:
		return luaL_typerror(L, 1, "WML table");
	}
	config filter;
	if (has_filter) {
		lua_settop(L, 1);
		if (!wml_config_of_table(L, filter))
			goto error_call_destructors;
	}

	// Go through all the units while keeping the following stack:
	// 1: metatable, 2: return table, 3: userdata, 4: metatable copy
	lua_settop(L, 0);
	lua_pushlightuserdata(L, (void *)&getunitKey);
	lua_gettable(L, LUA_REGISTRYINDEX);
	lua_newtable(L);
	int i = 1;
	unit_map &units = *game_events::resources->units;
	for (unit_map::const_unit_iterator ui = units.begin(), ui_end = units.end();
	     ui != ui_end; ++ui)
	{
		if (has_filter && !ui->second.matches_filter(vconfig(filter), ui->first))
			continue;
		size_t *p = static_cast<size_t *>(lua_newuserdata(L, sizeof(size_t)));
		*p = ui->second.underlying_id();
		lua_pushvalue(L, 1);
		lua_setmetatable(L, 3);
		lua_rawseti(L, 2, i);
		++i;
	}
	return 1;
}

/**
 * Fires a WML event handler.
 * - Arg 1: string containing the handler name.
 * - Arg 2: optional WML config.
 * - Arg 3,4: optional first location.
 * - Arg 5,6: optional second location.
 */
static int lua_fire(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);
	bool has_config = lua_gettop(L) >= 2;
	if (has_config && !lua_istable(L, 2)) {
		error_call_destructors:
		return luaL_typerror(L, 2, "WML table");
	}
	config cfg;
	if (has_config) {
		lua_pushvalue(L, 2);
		if (!wml_config_of_table(L, cfg))
			goto error_call_destructors;
		lua_pop(L, 1);
	}
	map_location l1, l2;
	if (lua_gettop(L) >= 4)
		l1 = map_location(lua_tointeger(L, 3) - 1, lua_tointeger(L, 4) - 1);
	if (lua_gettop(L) >= 6)
		l2 = map_location(lua_tointeger(L, 5) - 1, lua_tointeger(L, 6) - 1);

	game_events::handle_event_command
		(m, game_events::queued_event("_from_lua", l1, l2, config()), vconfig(cfg, true));
	return 0;
}

/**
 * Fires an event the same way the fire_event WML tag does it.
 * - Arg 1: string containing the event name.
 * - Arg 2,3: optional first location.
 * - Arg 4,5: optional second location.
 * - Ret 1: boolean indicating whether the event was processed or not.
 */
static int lua_fire_event(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);
	map_location l1, l2;
	if (lua_gettop(L) >= 3)
		l1 = map_location(lua_tointeger(L, 2) - 1, lua_tointeger(L, 3) - 1);
	if (lua_gettop(L) >= 5)
		l2 = map_location(lua_tointeger(L, 4) - 1, lua_tointeger(L, 5) - 1);

	bool b = game_events::fire(m, l1, l2);
	lua_pushboolean(L, b);
	return 1;
}

/**
 * Gets a WML variable.
 * - Arg 1: string containing the variable name.
 * - Arg 2: optional bool indicating if tables for containers should be left empty.
 * - Ret 1: value of the variable, if any.
 */
static int lua_get_variable(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);
	variable_info v(m, false, variable_info::TYPE_SCALAR);
	if (v.is_valid) {
		scalar_of_wml_string(L, v.as_scalar());
		return 1;
	} else {
		variable_info w(m, false, variable_info::TYPE_CONTAINER);
		if (w.is_valid) {
			lua_newtable(L);
			if (lua_toboolean(L, 2))
				table_of_wml_config(L, w.as_container());
			return 1;
		}
	}
	return 0;
}

/**
 * Sets a WML variable.
 * - Arg 1: string containing the variable name.
 * - Arg 2: bool/int/string/table containing the value.
 */
static int lua_set_variable(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);
	if (false) {
		error_call_destructors:
		return luaL_typerror(L, 2, "WML table or scalar");
	}

	if (lua_isnoneornil(L, 2)) {
		game_events::get_state_of_game()->clear_variable(m);
		return 0;
	}

	variable_info v(m);
	switch (lua_type(L, 2)) {
		case LUA_TBOOLEAN:
			v.as_scalar() = lua_toboolean(L, 2) ? "yes" : "no";
			break;
		case LUA_TNUMBER:
		case LUA_TSTRING:
			v.as_scalar() = lua_tostring(L, 2);
			break;
		case LUA_TUSERDATA:
			// Compare its metatable with t_string's metatable.
			lua_pushlightuserdata(L, (void *)&tstringKey);
			lua_gettable(L, LUA_REGISTRYINDEX);
			if (!lua_getmetatable(L, 2) || !lua_rawequal(L, -1, -2))
				goto error_call_destructors;
			v.as_scalar() = *static_cast<t_string *>(lua_touserdata(L, 2));
			break;
		case LUA_TTABLE:
			lua_settop(L, 2);
			if (!wml_config_of_table(L, v.as_container()))
				goto error_call_destructors;
			break;
		default:
			goto error_call_destructors;
	}
	return 0;
}

/**
 * Loads and executes a Lua file.
 * - Arg 1: string containing the file name.
 * - Ret *: values returned by executing the file body.
 */
static int lua_dofile(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);
	if (false) {
		error_call_destructors_1:
		return luaL_argerror(L, 1, "file not found");
		error_call_destructors_2:
		return lua_error(L);
		continue_call_destructor:
		lua_call(L, 0, 1);
		return 1;
	}
	std::string p = get_wml_location(m);
	if (p.empty())
		goto error_call_destructors_1;

	if (luaL_loadfile(L, p.c_str()))
		goto error_call_destructors_2;

	goto continue_call_destructor;
}

/**
 * Proxy class for calling WML action handlers defined in Lua.
 */
struct lua_action_handler : game_events::action_handler
{
	lua_State *L;
	int num;

	lua_action_handler(lua_State *l, int n) : L(l), num(n) {}
	void handle(const game_events::queued_event &, const vconfig &);
};

void lua_action_handler::handle(const game_events::queued_event &, const vconfig &cfg)
{
	// Load the error handler from the registry.
	lua_pushlightuserdata(L, (void *)&executeKey);
	lua_gettable(L, LUA_REGISTRYINDEX);

	// Load the user function from the registry.
	lua_pushlightuserdata(L, (void *)&uactionKey);
	lua_gettable(L, LUA_REGISTRYINDEX);
	lua_rawgeti(L, -1, num);
	lua_remove(L, -2);

	// Push the WML table argument.
	lua_newtable(L);
	table_of_wml_config(L, cfg.get_parsed_config());

	int res = lua_pcall(L, 1, 0, -3);
	if (res)
	{
		ERR_LUA
			<< "Failure while running Lua action handler: "
			<< lua_tostring(L, -1) << '\n';
		lua_pop(L, 2);
		return;
	}

	lua_pop(L, 1);
}

/**
 * Registers a function as WML action handler.
 * - Arg 1: string containing the WML tag.
 * - Arg 2: function taking a WML table as argument.
 */
static int lua_register_wml_action(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);

	// Retrieve the user action table from the registry.
	lua_pushlightuserdata(L, (void *)&uactionKey);
	lua_gettable(L, LUA_REGISTRYINDEX);
	size_t length = lua_objlen(L, -1);

	// Push the function on it so that it is not collected.
	lua_pushvalue(L, 2);
	lua_rawseti(L, -2, length + 1);

	// Create the proxy C++ action handler.
	game_events::register_action_handler(m, new lua_action_handler(L, length + 1));
	return 0;
}

/**
 * Gets some data on a side (__index metamethod).
 * - Arg 1: full userdata containing the team.
 * - Arg 2: string containing the name of the property.
 * - Ret 1: something containing the attribute.
 */
static int lua_side_get(lua_State *L)
{
	// Hidden metamethod, so arg1 has to be a pointer to a team.
	team &t = **static_cast<team **>(lua_touserdata(L, 1));
	char const *m = luaL_checkstring(L, 2);

	// Find the corresponding attribute.
	return_int_attrib("gold", t.gold());
	return_tstring_attrib("objectives", t.objectives());
	return_int_attrib("village_gold", t.village_gold());
	return_int_attrib("base_income", t.base_income());
	return_int_attrib("total_income", t.total_income());
	return_bool_attrib("objectives_changed", t.objectives_changed());
	return_tstring_attrib("user_team_name", t.user_team_name());
	return_string_attrib("team_name", t.team_name());
	return_cfg_attrib("__cfg", t.write(cfg));
	return 0;
}

/**
 * Sets some data on a side (__newindex metamethod).
 * - Arg 1: full userdata containing the team.
 * - Arg 2: string containing the name of the property.
 * - Arg 3: something containing the attribute.
 */
static int lua_side_set(lua_State *L)
{
	// Hidden metamethod, so arg1 has to be a pointer to a team.
	team &t = **static_cast<team **>(lua_touserdata(L, 1));
	char const *m = luaL_checkstring(L, 2);
	lua_settop(L, 3);

	// Find the corresponding attribute.
	modify_int_attrib("gold", t.set_gold(value));
	modify_tstring_attrib("objectives", t.set_objectives(value, true));
	modify_int_attrib("village_gold", t.set_village_gold(value));
	modify_int_attrib("base_income", t.set_base_income(value));
	modify_bool_attrib("objectives_changed", t.set_objectives_changed(value != 0));
	modify_tstring_attrib("user_team_name", t.change_team(t.team_name(), value));
	modify_string_attrib("team_name", t.change_team(value, t.user_team_name()));
	return 0;
}

/**
 * Gets a proxy userdata for a side.
 * - Arg 1: integer for the side.
 * - Ret 1: full userdata with __index pointing to lua_side_get
 *          and __newindex pointing to lua_side_set.
 */
static int lua_get_side(lua_State *L)
{
	int s = luaL_checkint(L, 1);

	size_t t = s - 1;
	std::vector<team> &teams = *game_events::resources->teams;
	if (t >= teams.size()) return 0;

	// Create a full userdata containing a pointer to the team.
	team **p = static_cast<team **>(lua_newuserdata(L, sizeof(team *)));
	*p = &teams[t];

	// Get the metatable from the registry and set it.
	lua_pushlightuserdata(L, (void *)&getsideKey);
	lua_gettable(L, LUA_REGISTRYINDEX);
	lua_setmetatable(L, 2);

	return 1;
}

/**
 * Gets a terrain code.
 * - Args 1,2: map location.
 * - Ret 1: string.
 */
static int lua_get_terrain(lua_State *L)
{
	int x = luaL_checkint(L, 1);
	int y = luaL_checkint(L, 2);

	t_translation::t_terrain const &t = game_events::resources->game_map->
		get_terrain(map_location(x - 1, y - 1));
	lua_pushstring(L, t_translation::write_terrain_code(t).c_str());
	return 1;
}

/**
 * Sets a terrain code.
 * - Args 1,2: map location.
 * - Arg 3: terrain code string.
 */
static int lua_set_terrain(lua_State *L)
{
	int x = luaL_checkint(L, 1);
	int y = luaL_checkint(L, 2);
	char const *m = luaL_checkstring(L, 3);

	t_translation::t_terrain t = t_translation::read_terrain_code(m);
	if (t != t_translation::NONE_TERRAIN)
		change_terrain(map_location(x - 1, y - 1), t, gamemap::BOTH, false);
	return 0;
}

/**
 * Gets info about a terrain.
 * - Arg 1: terrain code string.
 * - Ret 1: table
 */
static int lua_get_terrain_info(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);
	t_translation::t_terrain t = t_translation::read_terrain_code(m);
	if (t == t_translation::NONE_TERRAIN) return 0;
	terrain_type const &info = game_events::resources->game_map->get_terrain_info(t);

	lua_newtable(L);
	lua_pushstring(L, info.id().c_str());
	lua_setfield(L, -2, "id");
	lua_pushtstring(L, info.name());
	lua_setfield(L, -2, "name");
	lua_pushtstring(L, info.description());
	lua_setfield(L, -2, "description");
	lua_pushboolean(L, info.is_village());
	lua_setfield(L, -2, "village");
	lua_pushboolean(L, info.is_castle());
	lua_setfield(L, -2, "castle");
	lua_pushboolean(L, info.is_keep());
	lua_setfield(L, -2, "keep");
	lua_pushinteger(L, info.gives_healing());
	lua_setfield(L, -2, "healing");

	return 1;
}

static int lua_message(lua_State *L)
{
	char const *m = luaL_checkstring(L, 1);
	LOG_LUA << "Script says: \"" << m << "\"\n";
	return 0;
}

LuaKernel::LuaKernel()
	: mState(luaL_newstate())
{
	lua_State *L = mState;

	// Open safe libraries. (Debug is not, but it will be closed below.)
	static const luaL_Reg safe_libs[] = {
		{ "",       luaopen_base   },
		{ "table",  luaopen_table  },
		{ "string", luaopen_string },
		{ "math",   luaopen_math   },
		{ "debug",  luaopen_debug  },
		{ NULL, NULL }
	};
	for (luaL_Reg const *lib = safe_libs; lib->func; ++lib)
	{
		lua_pushcfunction(L, lib->func);
		lua_pushstring(L, lib->name);
		lua_call(L, 1, 0);
	}

	// Put some callback functions in the scripting environment.
	static luaL_reg const callbacks[] = {
		{ "dofile",                   &lua_dofile                   },
		{ "fire",                     &lua_fire                     },
		{ "fire_event",               &lua_fire_event               },
		{ "get_side",                 &lua_get_side                 },
		{ "get_terrain",              &lua_get_terrain              },
		{ "get_terrain_info",         &lua_get_terrain_info         },
		{ "get_unit_type",            &lua_get_unit_type            },
		{ "get_unit_type_ids",        &lua_get_unit_type_ids        },
		{ "get_units",                &lua_get_units                },
		{ "get_variable",             &lua_get_variable             },
		{ "message",                  &lua_message                  },
		{ "register_wml_action",      &lua_register_wml_action      },
		{ "set_terrain",              &lua_set_terrain              },
		{ "set_variable",             &lua_set_variable             },
		{ "textdomain",               &lua_textdomain               },
		{ NULL, NULL }
	};
	luaL_register(L, "wesnoth", callbacks);

	// Create the getside metatable.
	lua_pushlightuserdata(L, (void *)&getsideKey);
	lua_createtable(L, 0, 2);
	lua_pushcfunction(L, lua_side_get);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, lua_side_set);
	lua_setfield(L, -2, "__newindex");
	lua_pushstring(L, "Hands off! (getside metatable)");
	lua_setfield(L, -2, "__metatable");
	lua_settable(L, LUA_REGISTRYINDEX);

	// Create the gettext metatable.
	lua_pushlightuserdata(L, (void *)&gettextKey);
	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, lua_gettext);
	lua_setfield(L, -2, "__call");
	lua_pushstring(L, "Hands off! (gettext metatable)");
	lua_setfield(L, -2, "__metatable");
	lua_settable(L, LUA_REGISTRYINDEX);

	// Create the gettype metatable.
	lua_pushlightuserdata(L, (void *)&gettypeKey);
	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, lua_unit_type_get);
	lua_setfield(L, -2, "__index");
	lua_pushstring(L, "Hands off! (gettype metatable)");
	lua_setfield(L, -2, "__metatable");
	lua_settable(L, LUA_REGISTRYINDEX);

	// Create the getunit metatable.
	lua_pushlightuserdata(L, (void *)&getunitKey);
	lua_createtable(L, 0, 2);
	lua_pushcfunction(L, lua_unit_get);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, lua_unit_set);
	lua_setfield(L, -2, "__newindex");
	lua_pushstring(L, "Hands off! (getunit metatable)");
	lua_setfield(L, -2, "__metatable");
	lua_settable(L, LUA_REGISTRYINDEX);

	// Create the tstring metatable.
	lua_pushlightuserdata(L, (void *)&tstringKey);
	lua_createtable(L, 0, 3);
	lua_pushcfunction(L, lua_tstring_concat);
	lua_setfield(L, -2, "__concat");
	lua_pushcfunction(L, lua_tstring_collect);
	lua_setfield(L, -2, "__gc");
	lua_pushcfunction(L, lua_tstring_tostring);
	lua_setfield(L, -2, "__tostring");
	lua_pushstring(L, "Hands off! (tstring metatable)");
	lua_setfield(L, -2, "__metatable");
	lua_settable(L, LUA_REGISTRYINDEX);

	// Delete dofile and loadfile.
	lua_pushnil(L);
	lua_setglobal(L, "dofile");
	lua_pushnil(L);
	lua_setglobal(L, "loadfile");

	// Create the user action table.
	lua_pushlightuserdata(L, (void *)&uactionKey);
	lua_newtable(L);
	lua_settable(L, LUA_REGISTRYINDEX);

	// Store the error handler, then close debug.
	lua_pushlightuserdata(L, (void *)&executeKey);
	lua_getglobal(L, "debug");
	lua_getfield(L, -1, "traceback");
	lua_remove(L, -2);
	lua_settable(L, LUA_REGISTRYINDEX);
	lua_pushnil(L);
	lua_setglobal(L, "debug");

	lua_settop(L, 0);
}

LuaKernel::~LuaKernel()
{
	lua_close(mState);
}

/**
 * Runs a script from an event handler.
 */
void LuaKernel::run_event(vconfig const &cfg, game_events::queued_event const &ev)
{
	lua_State *L = mState;

	// Get user-defined arguments; append locations and weapons to it.
	config args;
	vconfig vargs = cfg.child("args");
	if (!vargs.null()) {
		args = vargs.get_parsed_config();
	}
	if (const config &weapon = ev.data.child("first")) {
		args.add_child("weapon", weapon);
	}
	if (const config &weapon = ev.data.child("first")) {
		args.add_child("second_weapon", weapon);
	}
	lua_newtable(L);
	table_of_wml_config(L, args);
	if (ev.loc1.valid()) {
		lua_pushinteger(L, ev.loc1.x + 1);
		lua_setfield(L, -2, "x1");
		lua_pushinteger(L, ev.loc1.y + 1);
		lua_setfield(L, -2, "y1");
	}
	if (ev.loc2.valid()) {
		lua_pushinteger(L, ev.loc2.x + 1);
		lua_setfield(L, -2, "x2");
		lua_pushinteger(L, ev.loc2.y + 1);
		lua_setfield(L, -2, "y2");
	}

	// Get the code from the uninterpolated config object, so that $ symbols
	// are not messed with.
	const std::string &prog = cfg.get_config()["code"];

	execute(prog.c_str(), 1, 0);
}

/**
 * Runs a plain script.
 */
void LuaKernel::run(char const *prog)
{
	execute(prog, 0, 0);
}

/**
 * Runs a script on a preset stack.
 */
void LuaKernel::execute(char const *prog, int nArgs, int nRets)
{
	lua_State *L = mState;

	// Load the error handler before the function arguments.
	lua_pushlightuserdata(L, (void *)&executeKey);
	lua_gettable(L, LUA_REGISTRYINDEX);
	if (nArgs)
		lua_insert(L, -1 - nArgs);

	// Compile script into a variadic function.
	int res = luaL_loadstring(L, prog);
	if (res)
	{
		ERR_LUA
			<< "Failure while loading Lua script: "
			<< lua_tostring(L, -1) << '\n';
		lua_pop(L, 2);
		return;
	}

	// Place the function before its arguments.
	if (nArgs)
		lua_insert(L, -1 - nArgs);

	res = lua_pcall(L, nArgs, nRets, -2 - nArgs);
	if (res)
	{
		ERR_LUA
			<< "Failure while running Lua script: "
			<< lua_tostring(L, -1) << '\n';
		lua_pop(L, 2);
		return;
	}

	lua_remove(L, -1 - nRets);
}

#endif // HAVE LUA

