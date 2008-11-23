//------------------------------------------------------------------------
//  2.5D Constructive Solid Geometry
//------------------------------------------------------------------------
//
//  Oblige Level Maker (C) 2006-2008 Andrew Apted
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
//------------------------------------------------------------------------

#include "headers.h"
#include "hdr_fltk.h"
#include "hdr_lua.h"
#include "hdr_ui.h"   // for mini map

#include <algorithm>

#include "lib_util.h"
#include "main.h"

#include "csg_main.h"
#include "g_lua.h"
#include "ui_dialog.h"


// FIXME std::vector<area_face_c *> all_faces;

std::vector<csg_brush_c *> all_brushes;

std::vector<entity_info_c *> all_entities;

std::vector<merge_vertex_c *>  mug_vertices;
std::vector<merge_segment_c *> mug_segments;
std::vector<merge_region_c *>  mug_regions;


static void CSG2_BeginLevel(void);
static void CSG2_EndLevel(void);



slope_plane_c::slope_plane_c() :
      sx(-1),sy(-1),sz(-1),
      ex(-1),ey(-1),ez(-1)
{ }

slope_plane_c::~slope_plane_c()
{ }

double slope_plane_c::GetAngle() const
{
  double xy_dist = ComputeDist(sx, sy, ex, ey);

  return CalcAngle(0, sz, xy_dist, ez);
}


area_face_c::area_face_c() :
      tex(), x_offset(FVAL_NONE), y_offset(FVAL_NONE), peg(false)
{ }

area_face_c::~area_face_c()
{ }


area_vert_c::area_vert_c(csg_brush_c *_parent, double _x, double _y) :
      parent(_parent), x(_x), y(_y), w_face(NULL),
      line_kind(0), line_tag(0), line_flags(0),
      partner(NULL)
{
  memset(line_args, 0, sizeof(line_args));
}

area_vert_c::~area_vert_c()
{ }


csg_brush_c::csg_brush_c() :
     verts(),
     bflags(0),
     b_face(NULL), t_face(NULL), w_face(NULL),
     z1(-1), z2(-1), b_slope(NULL), t_slope(NULL),
     sec_kind(0), sec_tag(0), mark(0)
{ }

csg_brush_c::csg_brush_c(const csg_brush_c *other, bool do_verts) :
      verts(),
      bflags(other->bflags),
      b_face(other->b_face), t_face(other->t_face),
      w_face(other->w_face),
      z1(other->z1), z2(other->z2),
      b_slope(NULL), t_slope(NULL),
      sec_kind(other->sec_kind), sec_tag(other->sec_tag),
      mark(other->mark)
{
  // FIXME: do_verts

  // FIXME: duplicate slopes
}

csg_brush_c::~csg_brush_c()
{
  // FIXME: free verts

  // FIXME: free slopes
}


const char * csg_brush_c::Validate()
{
  if (verts.size() < 3)
    return "Line loop contains less than 3 vertices!";

  // make sure vertices are clockwise

  double average_ang = 0;

  for (unsigned int k = 0; k < verts.size(); k++)
  {
    area_vert_c *v1 = verts[k];
    area_vert_c *v2 = verts[(k+1) % (int)verts.size()];
    area_vert_c *v3 = verts[(k+2) % (int)verts.size()];

    if (fabs(v2->x - v1->x) < EPSILON && fabs(v2->y - v1->y) < EPSILON)
      return "Line loop contains a zero length line!";

    double ang1 = CalcAngle(v2->x, v2->y, v1->x, v1->y);
    double ang2 = CalcAngle(v2->x, v2->y, v3->x, v3->y);

    double diff = ang2 - ang1;

    if (diff < 0) diff += 360.0;

// fprintf(stderr, "... [%d] ang1=%1.5f  ang2=%1.5f diff=%1.5f\n", (int)k, ang1, ang2, diff);

    average_ang += diff;
  }

  average_ang /= (double)verts.size();

// fprintf(stderr, "Average angle = %1.4f\n\n", average_ang);

  if (average_ang > 180.0)
    return "Line loop is not clockwise!";

  return NULL; // OK
}

void csg_brush_c::ComputeBBox()
{
  min_x = +999999.9;
  min_y = +999999.9;
  max_x = -999999.9;
  max_y = -999999.9;

  for (unsigned int i = 0; i < verts.size(); i++)
  {
    area_vert_c *V = verts[i];

    if (V->x < min_x) min_x = V->x;
    if (V->x > max_x) max_x = V->x;

    if (V->y < min_y) min_y = V->y;
    if (V->y > max_y) max_y = V->y;
  }
}

entity_info_c::entity_info_c(const char *_name, double xpos, double ypos,
                             double zpos, int _flags) :
    name(_name),
    x(xpos), y(ypos), z(zpos),
    eflags(_flags),
    props()
{ }

entity_info_c::~entity_info_c()
{ }


//------------------------------------------------------------------------

void CSG2_GetBounds(double& min_x, double& min_y, double& min_z,
                    double& max_x, double& max_y, double& max_z)
{
  min_x = min_y = min_z = +9e9;
  max_x = max_y = max_z = -9e9;

  for (unsigned int i = 0; i < mug_segments.size(); i++)
  {
    merge_segment_c *S = mug_segments[i];

    // ignore lines "in the solid"
    if (! S->HasGap())
      continue;

    double x1 = MIN(S->start->x, S->end->x);
    double y1 = MIN(S->start->y, S->end->y);

    double x2 = MAX(S->start->x, S->end->x);
    double y2 = MAX(S->start->y, S->end->y);

    min_x = MIN(min_x, x1);
    min_y = MIN(min_y, y1);

    max_x = MAX(max_x, x2);
    max_y = MAX(max_y, y2);

    if (S->front && S->front->HasGap())
    {
      min_z = MIN(min_z, S->front->MinGapZ());
      max_z = MAX(max_z, S->front->MaxGapZ());
    }

    if (S->back && S->back->HasGap())
    {
      min_z = MIN(min_z, S->back->MinGapZ());
      max_z = MAX(max_z, S->back->MaxGapZ());
    }
  }

  if (min_x > max_x)
    Main_FatalError("CSG2_GetBounds: map is completely solid!\n");

  // add some leeyway
  min_x -= 24; min_y -= 24; min_z -= 64;
  max_x += 24; max_y += 24; max_z += 64;
}


void CSG2_MakeMiniMap(void)
{
  int scale = 32*2;

  double min_x, min_y, min_z;
  double max_x, max_y, max_z;

  CSG2_GetBounds(min_x, min_y, min_z,  max_x, max_y, max_z);

  double cent_x = (min_x + max_x) / 2.0;
  double cent_y = (min_y + max_y) / 2.0;

  int map_W = main_win->build_box->mini_map->GetWidth();
  int map_H = main_win->build_box->mini_map->GetHeight();

  main_win->build_box->mini_map->MapBegin();

  for (unsigned int i = 0; i < mug_segments.size(); i++)
  {
    merge_segment_c *S = mug_segments[i];

    if (! S->HasGap())
      continue;

    int x1 = (int)ceil(S->start->x - cent_x) / scale + map_W/2;
    int y1 = (int)ceil(S->start->y - cent_y) / scale + map_H/2;

    int x2 = (int)ceil(S->end  ->x - cent_x) / scale + map_W/2;
    int y2 = (int)ceil(S->end  ->y - cent_y) / scale + map_H/2;

    bool two_sided = (S->front && S->front->gaps.size() > 0) &&
                     (S->back  && S->back ->gaps.size() > 0);

    u8_t r = 255;
    u8_t g = 255;
    u8_t b = 255;

    if (two_sided)
    {
      double f1 = S->front->MinGapZ();
      double f2 = S->back ->MinGapZ();

      double c1 = S->front->MaxGapZ();
      double c2 = S->back ->MaxGapZ();

      if (fabs(f1 - f2) < 0.1 && fabs(c1 - c2) < 0.1)
        continue;

      if (MIN(c1, c2) < MAX(f1, f2) + 52.5)
      {
        r = 255; g = 0; b = 0;
      }
      else if (fabs(f1 - f2) > 24.5)
      {
        r = 0; g = 255; b = 192;
      }
      else
      {
        r = g = b = 160;
      }
    }

    main_win->build_box->mini_map->DrawLine(x1,y1, x2,y2, r,g,b);
  }

  // entities
  for (unsigned k = 0; k < all_entities.size(); k++)
  {
    entity_info_c *E = all_entities[k];

    int x = (int)ceil(E->x - cent_x) / scale + map_W/2;
    int y = (int)ceil(E->y - cent_y) / scale + map_H/2;

    main_win->build_box->mini_map->DrawEntity(x,y, 255,255,0);
  }

  main_win->build_box->mini_map->MapFinish();
}



//------------------------------------------------------------------------

static slope_plane_c * Grab_Slope(lua_State *L, int stack_pos)
{
  if (stack_pos < 0)
    stack_pos += lua_gettop(L) + 1;

  if (lua_type(L, stack_pos) != LUA_TTABLE)
  {
    luaL_argerror(L, stack_pos, "expected a table: slope info");
    return NULL; /* NOT REACHED */
  }

  slope_plane_c *P = new slope_plane_c();

  lua_getfield(L, stack_pos, "sx");
  lua_getfield(L, stack_pos, "sy");
  lua_getfield(L, stack_pos, "sz");

  P->sx = luaL_checknumber(L, -3);
  P->sy = luaL_checknumber(L, -2);
  P->sz = luaL_checknumber(L, -1);

  lua_pop(L, 3);

  lua_getfield(L, stack_pos, "ex");
  lua_getfield(L, stack_pos, "ey");
  lua_getfield(L, stack_pos, "ez");

  P->sx = luaL_checknumber(L, -3);
  P->sy = luaL_checknumber(L, -2);
  P->sz = luaL_checknumber(L, -1);

  lua_pop(L, 3);

#if 0
  if (A->z2 <= A->z1 + EPSILON)
  {
    return luaL_error(L, "add_brush: bad z1..z2 range given (%1.2f .. %1.2f)", A->z1, A->z2);
  }
#endif

  return P;
}


static area_face_c * Grab_Face(lua_State *L, int stack_pos)
{
  if (stack_pos < 0)
    stack_pos += lua_gettop(L) + 1;

  if (lua_type(L, stack_pos) != LUA_TTABLE)
  {
    luaL_argerror(L, stack_pos, "expected a table: face info");
    return NULL; /* NOT REACHED */
  }

  area_face_c *F = new area_face_c();

  lua_getfield(L, stack_pos, "texture");
  lua_getfield(L, stack_pos, "x_offset");
  lua_getfield(L, stack_pos, "y_offset");
  lua_getfield(L, stack_pos, "peg");

  F->tex = std::string(luaL_checkstring(L, -4));

  if (! lua_isnil(L, -3)) F->x_offset = luaL_checknumber(L, -3);
  if (! lua_isnil(L, -2)) F->y_offset = luaL_checknumber(L, -2);

  if (lua_toboolean(L, -1)) F->peg = true;

  lua_pop(L, 4);

  // FIXME: store every face in the 'all_faces' list

  return F;
}


static csg_brush_c * Grab_AreaInfo(lua_State *L, int stack_pos)
{
  if (stack_pos < 0)
    stack_pos += lua_gettop(L) + 1;

  if (lua_type(L, stack_pos) != LUA_TTABLE)
  {
    luaL_argerror(L, stack_pos, "expected a table: area info");
    return NULL; /* NOT REACHED */
  }

  csg_brush_c *B = new csg_brush_c();

  lua_getfield(L, stack_pos, "t_face");
  lua_getfield(L, stack_pos, "b_face");
  lua_getfield(L, stack_pos, "w_face");

  B->t_face = Grab_Face(L, -3);
  B->b_face = Grab_Face(L, -2);
  B->w_face = Grab_Face(L, -1);

  lua_pop(L, 3);

  lua_getfield(L, stack_pos, "sec_kind");
  lua_getfield(L, stack_pos, "sec_tag");
  lua_getfield(L, stack_pos, "mark");

  if (lua_isnumber(L, -3)) B->sec_kind = lua_tointeger(L, -3);
  if (lua_isnumber(L, -2)) B->sec_tag  = lua_tointeger(L, -2);
  if (lua_isnumber(L, -1)) B->mark     = lua_tointeger(L, -1);

  lua_pop(L, 3);

  lua_getfield(L, stack_pos, "flag_liquid");
  lua_getfield(L, stack_pos, "flag_detail");
  lua_getfield(L, stack_pos, "flag_noclip");

  if (lua_toboolean(L, -3)) B->bflags |= BRU_F_Liquid;
  if (lua_toboolean(L, -2)) B->bflags |= BRU_F_Detail;
  if (lua_toboolean(L, -1)) B->bflags |= BRU_F_NoClip;

  lua_pop(L, 3);

  lua_getfield(L, stack_pos, "flag_door");
  lua_getfield(L, stack_pos, "flag_skyclose");
  lua_getfield(L, stack_pos, "flag_revdoor");

  if (lua_toboolean(L, -3)) B->bflags |= BRU_F_Door;
  if (lua_toboolean(L, -2)) B->bflags |= BRU_F_SkyClose;
  if (lua_toboolean(L, -1)) B->bflags |= BRU_F_RevDoor;
 
  lua_pop(L, 3);

  // TODO: peg, lighting ???

  return B;
}


static void Grab_HexenArgs(lua_State *L, byte *args)
{
  // NOTE: we assume table is on top of stack
  if (lua_type(L, -1) != LUA_TTABLE)
  {
    luaL_argerror(L, -1, "expected a table: line_args");
    return; /* NOT REACHED */
  }
 
  for (int i = 0; i < 5; i++)
  {
    lua_pushinteger(L, i+1);
    lua_gettable(L, -2);

    if (lua_isnumber(L, -1))
    {
      args[i] = lua_tointeger(L, -1);
    }

    lua_pop(L, 1);
  }
}


static area_vert_c * Grab_Vertex(lua_State *L, int stack_pos, csg_brush_c *B)
{
  if (stack_pos < 0)
    stack_pos += lua_gettop(L) + 1;

  if (lua_type(L, stack_pos) != LUA_TTABLE)
  {
    luaL_argerror(L, stack_pos, "expected a table: vertex");
    return NULL; /* NOT REACHED */
  }

  area_vert_c *V = new area_vert_c(B);

  lua_getfield(L, stack_pos, "x");
  lua_getfield(L, stack_pos, "y");

  V->x = luaL_checknumber(L, -2);
  V->y = luaL_checknumber(L, -1);

  lua_pop(L, 2);

  lua_getfield(L, stack_pos, "w_face");

  if (! lua_isnil(L, -1))
  {
    V->w_face = Grab_Face(L, -1);
  }

  lua_pop(L, 1);

  lua_getfield(L, stack_pos, "line_kind");
  lua_getfield(L, stack_pos, "line_tag");
  lua_getfield(L, stack_pos, "line_flags");
  lua_getfield(L, stack_pos, "line_args");

  if (lua_isnumber(L, -4)) V->line_kind  = lua_tointeger(L, -4);
  if (lua_isnumber(L, -3)) V->line_tag   = lua_tointeger(L, -3);
  if (lua_isnumber(L, -2)) V->line_flags = lua_tointeger(L, -2);

  if (! lua_isnil(L, -1))
  {
    Grab_HexenArgs(L, V->line_args);
  }

  lua_pop(L, 4);

  return V;
}


static void Grab_LineLoop(lua_State *L, int stack_pos, csg_brush_c *B)
{
  if (lua_type(L, stack_pos) != LUA_TTABLE)
  {
    luaL_argerror(L, stack_pos, "expected a table: line loop");
    return; /* NOT REACHED */
  }

  int index = 1;

  for (;;)
  {
    lua_pushinteger(L, index);
    lua_gettable(L, stack_pos);

    if (lua_isnil(L, -1))
    {
      lua_pop(L, 1);
      break;
    }

    area_vert_c *V = Grab_Vertex(L, -1, B);

    B->verts.push_back(V);

    lua_pop(L, 1);

    index++;
  }

  const char *err_msg = B->Validate();

  if (err_msg)
    luaL_error(L, "%s", err_msg);

  B->ComputeBBox();
}


// LUA: begin_level()
//
int CSG2_begin_level(lua_State *L)
{
  // call our own initialisation function first
  CSG2_BeginLevel();

  SYS_ASSERT(game_object);

  game_object->BeginLevel();

  return 0;
}


// LUA: end_level()
//
int CSG2_end_level(lua_State *L)
{
  SYS_ASSERT(game_object);

  game_object->EndLevel();

  // call our own termination function afterwards
  CSG2_EndLevel();

  return 0;
}


// LUA: property(key, value)
//
int CSG2_property(lua_State *L)
{
  const char *key   = luaL_checkstring(L,1);
  const char *value = luaL_checkstring(L,2);

  // TODO: eat propertities intended for CSG2

  SYS_ASSERT(game_object);

  game_object->Property(key, value);

  return 0;
}


// LUA: add_brush(info, loop, z1, z2)
//
// info is a table:
//    t_face, b_face : top and bottom faces
//    w_face         : default side face
//
//    flag_xxx       : various CSG flags (e.g. Liquid)
//
//    mark           : separating number
//    sec_kind       : DOOM sector type
//    sec_tag        : DOOM sector tag
//
// ?? liquid         : usually nil, otherwise name (e.g. "lava")
// ?? special        : usually nil, otherwise name (e.g. "damage20")
//
// z1 & z2 are either a height (number) or a slope table:
//    sx, sy, sz     : start point on slope
//    ex, ey, ez     : end point on slope
//
// loop is an array of Vertices:
//    x, y
//    w_face (can be nil)
//
//    line_kind,  line_tag
//    line_flags, line_args
//    rail (= face table)
//
// face is a table:
//    texture
//    x_offset, y_offset
//    peg (DOOM only)
//
int CSG2_add_brush(lua_State *L)
{
  csg_brush_c *B = Grab_AreaInfo(L, 1);

  Grab_LineLoop(L, 2, B);

  if (lua_isnumber(L, 3))
    B->z1 = lua_tonumber(L, 3);
  else
    B->b_slope = Grab_Slope(L, 3);

  if (lua_isnumber(L, 4))
    B->z2 = lua_tonumber(L, 4);
  else
    B->t_slope = Grab_Slope(L, 4);

  all_brushes.push_back(B);

  return 0;
}


// LUA: add_entity(x, y, z, props)
//
// props is a table:
//   name   : entity type name
//   light  : amount of light emitted
//
//   flag_xxx : various CSG flags
//
//   angle
//   options (DOOM, HEXEN)
//   args (HEXEN only)
//   ...
//
int CSG2_add_entity(lua_State *L)
{
  double x = luaL_checknumber(L,1);
  double y = luaL_checknumber(L,2);
  double z = luaL_checknumber(L,3);

  if (lua_type(L, 4) != LUA_TTABLE)
  {
    luaL_argerror(L, 4, "expected a table: entity info");
    return 0; /* NOT REACHED */
  }

  const char *name;

  lua_getfield(L, 4, "name");
  name = luaL_checkstring(L,-1);
  lua_pop(L, 1);


  int eflags = 0;

  // FIXME: grab some flags

  entity_info_c *E = new entity_info_c(name, x, y, z, eflags);


  // grab properties

  for (lua_pushnil(L) ; lua_next(L, 4) != 0 ; lua_pop(L,1))
  {
    if (lua_type(L, -2) != LUA_TSTRING)
      continue; // skip keys which are not strings

    const char *p_key   = lua_tostring(L, -2);
    const char *p_value = lua_tostring(L, -1);

    E->props[p_key] = std::string(p_value);
  }

  all_entities.push_back(E);

  return 0;
}


//------------------------------------------------------------------------


void CSG2_FreeMerges(void)
{
  unsigned int k;

  for (k = 0; k < mug_vertices.size(); k++)
    delete mug_vertices[k];

  for (k = 0; k < mug_segments.size(); k++)
    delete mug_segments[k];

  for (k = 0; k < mug_regions.size(); k++)
    delete mug_regions[k];

  mug_vertices.clear();
  mug_segments.clear();
  mug_regions .clear();
}

void CSG2_FreeAll(void)
{
  CSG2_FreeMerges();

  unsigned int k;

  for (k = 0; k < all_brushes.size(); k++)
    delete all_brushes[k];

  for (k = 0; k < all_entities.size(); k++)
    delete all_entities[k];

  all_brushes.clear();
  all_entities.clear();
}


void CSG2_BeginLevel(void)
{
}

void CSG2_EndLevel(void)
{
  CSG2_FreeAll();
}



//--- editor settings ---
// vi:ts=2:sw=2:expandtab
