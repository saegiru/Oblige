//------------------------------------------------------------------------
//  2.5D Constructive Solid Geometry
//------------------------------------------------------------------------
//
//  Oblige Level Maker
//
//  Copyright (C) 2006-2010 Andrew Apted
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

#ifndef __OBLIGE_CSG_MAIN_H__
#define __OBLIGE_CSG_MAIN_H__


// grid spacing for quantizing vertices  
extern double QUANTIZE_GRID;

// distance between two points to consider them equal.
// the 1.98 provides a small overlap between two quantized vertices.
#define EPSILON  (QUANTIZE_GRID / 1.98)

#define ANGLE_EPSILON  0.0003

// very high (low) value for uncapped brushes
#define EXTREME_H  4000


// unset values (handy sometimes)
#define IVAL_NONE  -27777
#define FVAL_NONE  -27777.75f


/* ----- CLASSES ----- */

class csg_brush_c;


class slope_info_c
{
  // defines the planes used for sloped floors or ceiling.
  // gives two points on the 2D map, and change in Z between them.
  //
  // the absolute Z coords are not here, this is implicitly relative
  // to an external height (such as the top of the brush).

public:
  double sx, sy;
  double ex, ey;

  double dz;

public:
   slope_info_c();
   slope_info_c(const slope_info_c *other);
  ~slope_info_c();

  void Reverse();

  double GetAngle() const;

  double CalcZ(double base_z, double x, double y) const;
};


class csg_property_set_c
{
private:
  std::map<std::string, std::string> dict;

public:
  csg_property_set_c() : dict()
  { }

  ~csg_property_set_c()
  { }

  void Add(const char *key, const char *value);

  const char * getStr(const char *key, const char *def_val = NULL);

  double getDouble(const char *key, double def_val = 0);
  int    getInt   (const char *key, int def_val = 0);

  void getHexenArgs(const char *key, u8_t *args);

public:
  typedef std::map<std::string, std::string>::iterator iterator;

  iterator begin() { return dict.begin(); }
  iterator   end() { return dict.  end(); }
};


class brush_vert_c
{
public:
  csg_brush_c *parent;

  double x, y;

  csg_property_set_c face;

///---  merge_vertex_c *partner;

public:
   brush_vert_c(csg_brush_c *_parent, double _x = 0, double _y = 0);
  ~brush_vert_c();
};


class brush_plane_c
{
public:
  // without slope, this is just the height of the top or bottom
  // of the brush.  When sloped, it still represents a bounding
  // height of the brush.
  double z;

  slope_info_c *slope;  // NULL if not sloped

  csg_property_set_c face;

public:
  brush_plane_c(double _z = 0) : z(_z), slope(NULL), face()
  { }

  brush_plane_c(const brush_plane_c& other);

  ~brush_plane_c();
};


typedef enum
{
  BKIND_Solid = 0,
  BKIND_Detail,   // ignored for clipping (Quake 1/2 only)
  BKIND_Clip,     // clipping only, no visible faces (Quake 1/2 only)

  BKIND_Sky,
  BKIND_Liquid,
  BKIND_Rail,     // supply a railing (DOOM/Nukem only)
  BKIND_Light,    // supply extra lighting or shadow
}
brush_kind_e;

typedef enum
{
  // internal flags
  BRU_IF_Quad    = (1 << 16),  // brush is a four-sided box
}
brush_flags_e;


class csg_brush_c
{
  // This represents a "brush" in Quake terms, a solid area
  // on the map with out-facing sides and top/bottom.  Like
  // quake brushes, these must be convex, but co-linear sides
  // are allowed.

public:
  int bkind;
  int bflags;

  std::vector<brush_vert_c *> verts;

  brush_plane_c b;  // bottom
  brush_plane_c t;  // top

  double min_x, min_y;
  double max_x, max_y;

public:
   csg_brush_c();
  ~csg_brush_c();

  // copy constructor
  csg_brush_c(const csg_brush_c *other, bool do_verts = false);

  void ComputeBBox();

  const char * Validate();
  // makes sure there are enough vertices and they are in
  // anti-clockwise order.  Returns NULL if OK, otherwise an
  // error message string.
};


class entity_info_c
{
public:
  std::string name;

  double x, y, z;

  csg_property_set_c props;

public:
   entity_info_c(const char *_name, double xpos, double ypos, double zpos,
                 int _flags = 0);
  ~entity_info_c();
};


//------------------------------------------------------------------------


/* ----- VARIABLES ----- */

extern std::vector<csg_brush_c *> all_brushes;

extern std::vector<entity_info_c *> all_entities;

extern int bounds_x1, bounds_y1, bounds_z1;
extern int bounds_x2, bounds_y2, bounds_z2;

extern std::string dummy_wall_tex;
extern std::string dummy_plane_tex;


/* ----- FUNCTIONS ----- */

void CSG2_MergeAreas(bool do_clips = false);

#if 0
brush_vert_c * CSG2_FindSideVertex(merge_segment_c *G, double z,
                                  bool is_front, bool exact = false);
csg_brush_c * CSG2_FindSideBrush(merge_segment_c *G, double z,
                                 bool is_front, bool exact = false);
brush_vert_c * CSG2_FindSideFace(merge_segment_c *G, double z, bool is_front,
                                 brush_vert_c *V = NULL);

merge_region_c *CSG2_FindRegionForPoint(double x, double y);
#endif

void CSG2_FreeMerges(void);

void CSG2_UpdateBounds(bool three_d);

#endif /* __OBLIGE_CSG_MAIN_H__ */

//--- editor settings ---
// vi:ts=2:sw=2:expandtab
