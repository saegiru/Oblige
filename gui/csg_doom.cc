//------------------------------------------------------------------------
//  CSG : DOOM and HEXEN output
//------------------------------------------------------------------------
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
//------------------------------------------------------------------------

#include "headers.h"
#include "hdr_fltk.h"
#include "hdr_ui.h"  // ui_build.h

#include <algorithm>

#include "lib_file.h"
#include "lib_util.h"
#include "main.h"

#include "csg_main.h"
#include "csg_local.h"

#include "dm_extra.h"
#include "g_doom.h"



// Properties
int ef_solid_type;
int ef_liquid_type;

// How to place things on 3D floors:
// 1 = use FraggleScript (in header lump)
// 2 = use EDGE thing flags
int ef_thing_mode;


// valid after DM_CreateLinedefs()
static int map_bound_x1, map_bound_x2;
static int map_bound_y1, map_bound_y2;

static int dummy_pos_x;
static int dummy_pos_y;


#define SEC_FLOOR_SPECIAL  (1 << 1)
#define SEC_CEIL_SPECIAL   (1 << 2)


double light_dist_factor = 800.0;

#define COLINEAR_THRESHHOLD  0.24


#define MTF_ALL_SKILLS  (MTF_Easy | MTF_Medium | MTF_Hard)

#define MTF_HEXEN_CLASSES  (32 + 64 + 128)
#define MTF_HEXEN_MODES    (256 + 512 + 1024)

#define MTF_EDGE_EXFLOOR_SHIFT  10


// fake linedef special for lower-unpegging
#define LIN_FAKE_UNPEGGED	991

// fake sector special to merge sector with lowest neighbor floor
#define SEC_GRAB_NB_FLOOR	993

// fake sector special for teleporting monster closets
#define SEC_DEPOT_PEER		988


class extrafloor_c
{
public:
	int line_special;

	int top_h;
	int bottom_h;

	// textures
	std::string top;
	std::string bottom;
	std::string wall;

	// sector properties underneath
	int u_light;
	int u_special;
	int u_tag;

public:
	extrafloor_c() : line_special(0), top(), bottom(), wall(),
	u_light(128), u_special(0), u_tag(0)
	{ }

	~extrafloor_c()
	{ } 

	bool Match(const extrafloor_c *other) const
	{
		return
			(line_special == other->line_special) &&

			(   top_h == other->top_h)    &&
			(bottom_h == other->bottom_h) &&

			(u_light   == other->u_light)   &&
			(u_special == other->u_special) &&
			(u_tag     == other->u_tag)     &&

			(strcmp(top.c_str(),    other->top.c_str())    == 0) &&
			(strcmp(bottom.c_str(), other->bottom.c_str()) == 0) &&
			(strcmp(wall.c_str(),   other->wall.c_str())   == 0);
	}
};


class doom_linedef_c;


class doom_sector_c 
{
public:
	int f_h;
	int c_h;

	std::string f_tex;
	std::string c_tex;

	int light;
	int special;
	int tag;
	int mark;

	int index;

	region_c *region;

	int misc_flags;
	int valid_count;

	int light2;
	int sound_area;

	bool unused;

	bool is_cave;

	std::vector<extrafloor_c *> exfloors;

	std::vector<doom_sector_c *> ef_neighbors;

public:
	doom_sector_c() :
		f_h(0), c_h(0), f_tex(), c_tex(),
		light(0), special(0), tag(0), mark(0), index(-1),
		region(NULL), misc_flags(0), valid_count(0),
		light2(0), unused(false), is_cave(false),
		exfloors(), ef_neighbors()
	{ }

	~doom_sector_c()
	{ }

	void MarkUnused()
	{
		unused = true;
	}

	bool isUnused() const
	{
		return unused;
	}

	void AddExtrafloor(extrafloor_c *EF)
	{
		exfloors.push_back(EF);
	}

	bool SameExtraFloors(const doom_sector_c *other) const
	{
		if (exfloors.size() != other->exfloors.size())
			return false;

		for (unsigned int i = 0 ; i < exfloors.size() ; i++)
		{
			extrafloor_c *A = exfloors[i];
			extrafloor_c *B = other->exfloors[i];

			if (A == B || A->Match(B))
				continue;

			return false;
		}

		return true;
	}

	bool MatchMost(const doom_sector_c *other) const
	{
		return  (f_h == other->f_h) &&
				(c_h == other->c_h) &&
				(light == other->light) &&
				(special == other->special) &&
				(tag == other->tag) &&
				(sound_area == other->sound_area) &&

				(strcmp(f_tex.c_str(), other->f_tex.c_str()) == 0) &&
				(strcmp(c_tex.c_str(), other->c_tex.c_str()) == 0);
	}

	bool MatchNoLight(const doom_sector_c *other) const
	{
		return  (mark == other->mark) &&
				(f_h == other->f_h) &&
				(c_h == other->c_h) &&
				(special == other->special) &&
				(tag  == other->tag)  &&

				(strcmp(f_tex.c_str(), other->f_tex.c_str()) == 0) &&
				(strcmp(c_tex.c_str(), other->c_tex.c_str()) == 0);
	}

	bool Match(const doom_sector_c *other) const
	{
		// deliberately absent: misc_flags

		return (mark == other->mark) &&
				MatchMost(other) &&
				SameExtraFloors(other);
	}

	bool ShouldMerge(const doom_sector_c *other) const
	{
		// special logic for secrets
		if (special == 9 && other->special == 9)
		{
			return	(mark == other->mark) &&
					(f_h  == other->f_h);
		}

		return Match(other);
	}

	int Write();
};


class doom_vertex_c 
{
public:
	int x, y;

	int index;

	// keep track of a few (but not all) linedefs touching this vertex.
	// this is used to detect colinear lines which can be merged. and
	// also for horizontal texture alignment.
	doom_linedef_c *lines[4];

	// was the vertex created by corner rounding code, and it split an
	// existing linedef in half?
	bool rounded_half;

public:
	doom_vertex_c() : x(0), y(0), index(-1), rounded_half(false)
	{
		lines[0] = lines[1] = lines[2] = lines[3] = NULL;
	}

	doom_vertex_c(int _x, int _y) : x(_x), y(_y), index(-1), rounded_half(false)
	{
		lines[0] = lines[1] = lines[2] = lines[3] = NULL;
	}

	~doom_vertex_c()
	{ }

	int getNumLines() const
	{
		for (int i = 0 ; i < 4 ; i++)
			if (! lines[i])
				return i;

		return 4;
	}

	void Kill()
	{
		rounded_half = false;

		for (int i = 0 ; i < 4 ; i++)
			lines[i] = NULL;
	}

	int FindLine(const doom_linedef_c *L) const
	{
		for (int i = 0 ; i < 4 ; i++)
			if (lines[i] == L)
				return i;

		return -1;
	}

	void AddLine(doom_linedef_c *L)
	{
		for (int i = 0 ; i < 4 ; i++)
			if (! lines[i])
			{
				lines[i] = L; return;
			}
	}

	void RemoveLine(doom_linedef_c *L)
	{
		int i = FindLine(L);

		if (i < 0)
			return;

		for ( ; i < 3 ; i++)
		{
			lines[i] = lines[i+1];
		}

		lines[i] = NULL;
	}

	void ReplaceLine(doom_linedef_c *old_L, doom_linedef_c *new_L)
	{
		for (int i = 0 ; i < 4 ; i++)
			if (lines[i] == old_L)
			{
				lines[i] = new_L;
				return;
			}
	}

	doom_linedef_c *SecondLine(const doom_linedef_c *L) const
	{
		if (lines[2])  // three or more lines?
			return NULL;

		if (! lines[1])  // only one line?
			return NULL;

		if (lines[0] == L)
			return lines[1];

		SYS_ASSERT(lines[1] == L);
		return lines[0];
	}

	int Write();
};


class doom_sidedef_c 
{
public:
	std::string lower;
	std::string mid;
	std::string upper;

	int x_offset;
	int y_offset;

	doom_sector_c * sector;

	int index;

public:
	doom_sidedef_c() :
		lower("-"), mid("-"), upper("-"),
		x_offset(0), y_offset(0),
		sector(NULL), index(-1)
	{ }

	~doom_sidedef_c()
	{ }

	doom_sidedef_c(const doom_sidedef_c& other) :
		lower(other.lower), mid(other.mid), upper(other.upper),
		x_offset(IVAL_NONE), y_offset(other.y_offset),
		sector(other.sector), index(-1)
	{ }

	int Write();

	inline bool SameTex(const doom_sidedef_c *T) const
	{
		return	(strcmp(mid  .c_str(), T->mid  .c_str()) == 0) &&
				(strcmp(lower.c_str(), T->lower.c_str()) == 0) &&
				(strcmp(upper.c_str(), T->upper.c_str()) == 0);
	}
};


class doom_linedef_c 
{
public:
	doom_vertex_c *start;  // NULL means "unused linedef"
	doom_vertex_c *end;

	doom_sidedef_c *front;
	doom_sidedef_c *back;

	int flags;
	int special;
	int tag;

	u8_t args[5];

	double length;

	// similar linedef touching our start (end) vertex, or NULL if none.
	// only takes front sidedefs into account.
	// used for texture aligning.
	doom_linedef_c *sim_prev;
	doom_linedef_c *sim_next;

public:
	doom_linedef_c() :
		start(NULL), end(NULL),
		front(NULL), back(NULL),
		flags(0), special(0), tag(0), length(0),
		sim_prev(NULL), sim_next(NULL)
	{
		args[0] = args[1] = args[2] = args[3] = args[4] = 0;
	}

	~doom_linedef_c()
	{ }

	doom_linedef_c(const doom_linedef_c& other) :
		start(NULL), end(NULL),
		front(NULL), back(NULL),
		flags(other.flags), special(other.special),
		tag(other.tag), length(0),
		sim_prev(NULL), sim_next(NULL)
	{
		// NOTE: be sure to add these sidedefs into dm_sidedefs!
		if (other.front)
			front = new doom_sidedef_c(*other.front);

		if (other.back)
			back = new doom_sidedef_c(*other.back);
	}

	void CalcLength()
	{
		length = ComputeDist(start->x, start->y, end->x, end->y);
	}

	inline doom_vertex_c *OtherVert(const doom_vertex_c *V) const
	{
		if (start == V)
			return end;

		SYS_ASSERT(end == V);
		return start;
	}

	inline bool isValid() const
	{
		return (start != NULL);
	}

	bool TouchesCoord(int x, int y) const
	{
		if (isValid())
		{
			if (start->x == x && start->y == y)
				return true;

			if (end->x == x && end->y == y)
				return true;
		}

		return false;
	}

	void Kill()
	{
		start->RemoveLine(this);
		start = NULL;

		end->RemoveLine(this);
		end = NULL;
	}

	void Flip()
	{
		std::swap(start, end);
		std::swap(front, back);
	}

	inline bool ShouldFlip() const
	{
		if (! front)
			return true;

		if (! back)
			return false;

		doom_sector_c *F = front->sector;
		doom_sector_c *B = back->sector;

		// TODO: a way to ensure a certain orientation (from Lua)

		if (F->f_h != B->f_h) return (F->f_h > B->f_h);
		if (F->c_h != B->c_h) return (F->c_h < B->c_h);

		return false;
	}

	bool hasRail() const
	{
		if (front && front->mid[0] != '-') return true;
		if ( back &&  back->mid[0] != '-') return true;

		return false;
	}

	inline bool CanMergeSides(const doom_sidedef_c *A, const doom_sidedef_c *B) const
	{
		if (! A || ! B)
			return (!A && !B);

		if (A->sector != B->sector)
			return false;

		// X offsets not done here

		if (A->y_offset != B->y_offset &&
			A->y_offset != IVAL_NONE   &&
			B->y_offset != IVAL_NONE)
			return false;

		return A->SameTex(B);
	}

	bool ColinearWith(const doom_linedef_c *B) const
	{
		// never merge a pure horizontal/vertical with a diagonal
		if (start->x == end->x)
			return B->start->x == B->end->x;

		if (start->y == end->y)
			return B->start->y == B->end->y;

		if (B->start->x == B->end->x || B->start->y == B->end->y)
			return false;

		float adx = end->x - start->x;
		float ady = end->y - start->y;
		float a_len = sqrt(adx*adx + ady*ady);

		float bdx = B->end->x - B->start->x;
		float bdy = B->end->y - B->start->y;
		float b_len = sqrt(bdx*bdx + bdy*bdy);

		adx /= a_len;  bdx /= b_len;
		ady /= a_len;  bdy /= b_len;

		double d = fabs(adx * bdy - bdx * ady);

		//  fprintf(stderr, "Colinear Lines: d=%1.4f\n", d);
		//  fprintf(stderr, "  A:(%d %d) .. (%d %d) : %+d %+d\n", start->x,start->y, end->x,end->y, end->x - start->x, end->y - start->y);
		//  fprintf(stderr, "  B:(%d %d) .. (%d %d) : %+d %+d\n", B->start->x,B->start->y, B->end->x,B->end->y, B->end->x - B->start->x, B->end->y - B->start->y);

		return d < COLINEAR_THRESHHOLD;
	}

private:
	bool TryMerge3(doom_linedef_c *B)
	{
		// meh, this should not be possible (but it CAN happen)
		if (start == B->end)
			return false;

		// test sidedefs
		doom_sidedef_c *B_front = B->front;
		doom_sidedef_c *B_back  = B->back;

		int A_len = I_ROUND(   length);
		int B_len = I_ROUND(B->length);

		if (! CanMergeSides(front, B_front)) return false;
		if (! CanMergeSides(back,  B_back))  return false;

		if (front)
		{
			int ax =   front->x_offset;
			int bx = B_front->x_offset;

			if (bx == IVAL_NONE)
			{
				/* OK */
			}
			else if (ax == IVAL_NONE)
			{
				return false;
			}
			else
			{
				int diff = (ax + A_len) - bx;

				// the < 4 accounts for precision loss after multiple merges
				if (abs(diff) >= 4)
					return false;
			}
		}

		if (back)
		{
			int ax =   back->x_offset;
			int bx = B_back->x_offset;

			if (bx == IVAL_NONE)
			{
				/* OK */
			}
			else if (ax == IVAL_NONE)
			{
				return false;
			}
			else
			{
				int diff = (bx + B_len) - ax;

				if (abs(diff) >= 4)
					return false;
			}
		}

		/* CAN MERGE THEM */

		if (back && back->x_offset != IVAL_NONE)
			back->x_offset -= B_len;


		end->Kill();

		end = B->end;

		end->ReplaceLine(B, this);

		CalcLength();

		B->Kill();

		return true;
	}

	bool TryMerge2(doom_linedef_c *B)
	{
		if (end != B->end)
			return TryMerge3(B);

		B->Flip();

		if (TryMerge3(B))
			return true;  // B has been killed

		B->Flip();

		return false;
	}

public:
	bool TryMerge(doom_linedef_c *B)
	{
		if (! ColinearWith(B))
			return false;

		// must have matching special, tag and flags
		if (B->special != special) return false;
		if (B->tag     != tag)     return false;
		if (B->flags   != flags)   return false;

		// this flip malarkey is to ensure that B's start == A's end,
		// which greatly simplifies the offset tests.

		if (! (start == B->start || start == B->end))
			return TryMerge2(B);

		Flip();

		bool result = TryMerge2(B);

		Flip();

		return result;
	}

	bool isVert() const
	{
		return start->x == end->x;
	}

	bool isHoriz() const
	{
		return start->y == end->y;
	}

	bool isFrontSimilar(const doom_linedef_c *P) const
	{
		if (! back && ! P->back)
			return (strcmp(front->mid.c_str(), P->front->mid.c_str()) == 0);

		if (back && P->back)
			return front->SameTex(P->front);

		const doom_linedef_c *L = this;

		if (back)
			std::swap(L, P);

		// now L is single sided and P is double sided.

		// allow either upper or lower to match
		return (strcmp(L->front->mid.c_str(), P->front->lower.c_str()) == 0) ||
			(strcmp(L->front->mid.c_str(), P->front->upper.c_str()) == 0);
	}

	void Write();
};



struct fs_thing_t
{
	int x, y, z;

	// this will point into a std::string in a csg_entity_c in the
	// all_entities list -- guaranteed to stay around until map is
	// fully written.
	const char *fs_name;

	int angle;
	int options;
};



/********* TABLES *********/


static std::vector<doom_vertex_c *>  dm_vertices;
static std::vector<doom_linedef_c *> dm_linedefs;
static std::vector<doom_sidedef_c *> dm_sidedefs;
static std::vector<doom_sector_c *>  dm_sectors;

class dummy_sector_c;

static std::vector<dummy_sector_c *> dm_dummies;
static std::vector<extrafloor_c *>   dm_exfloors;
static std::vector<fs_thing_t>       dm_fs_things;

static std::map<int, unsigned int>   dm_vertex_map;


//------------------------------------------------------------------------

#if 0

void DM_WriteDoom(void);  // forward


void Doom_TestBrushes(void)
{
	// for debugging only: each csg_brush_c becomes a single
	// sector on the map.

	DM_StartWAD("brush_test.wad");
	DM_BeginLevel();

	for (unsigned int k = 0; k < all_brushes.size(); k++)
	{
		csg_brush_c *P = all_brushes[k];

		int sec_idx = DM_NumSectors();

		const char *b_tex = P->b.face.getStr("tex", "LAVA1");
		const char *t_tex = P->t.face.getStr("tex", "LAVA1");

		DM_AddSector(I_ROUND(P->b.z), b_tex, I_ROUND(P->t.z), t_tex, 192, 0, 0);

		int side_base = DM_NumSidedefs();
		int vert_base = DM_NumVertexes();

		for (int j1 = 0; j1 < (int)P->verts.size(); j1++)
		{
			int j2 = (j1 + 1) % (int)P->verts.size();

			brush_vert_c *v1 = P->verts[j1];

			const char *w_tex = v1->face.getStr("tex", "CRACKLE4");

			DM_AddVertex(I_ROUND(v1->x), I_ROUND(v1->y));

			DM_AddSidedef(sec_idx, "-", w_tex, "-", 0, 0);

			DM_AddLinedef(vert_base+j2, vert_base+j1, side_base+j1, -1,
					0, 1 /*impassible*/, 0, NULL /* args */);
		}
	}

	DM_EndLevel("MAP01");
	DM_EndWAD();
}


void Doom_TestClip(void)
{
	// for Quake debugging only....

	DM_StartWAD("clip_test.wad");
	DM_BeginLevel();

	DM_WriteDoom();

	DM_EndLevel("MAP01");
	DM_EndWAD();
}


void DM_TestRegions(void)
{
	// for debugging only: each merge_region becomes a single
	// sector on the map.

	unsigned int i;

	for (i = 0; i < mug_vertices.size(); i++)
	{
		merge_vertex_c *V = mug_vertices[i];

		V->index = (int)i;

		DM_AddVertex(I_ROUND(V->x), I_ROUND(V->y));
	}


	for (i = 0; i < mug_regions.size(); i++)
	{
		merge_region_c *R = mug_regions[i];

		R->index = (int)i;

		const char *flat = "FLAT1";

		DM_AddSector(0,flat, 144,flat, 255,(int)R->brushes.size(),(int)R->gaps.size());

		const char *tex = R->faces_out ? "COMPBLUE" : "STARTAN3";

		DM_AddSidedef(R->index, tex, "-", tex, 0, 0);
	}


	for (i = 0; i < mug_segments.size(); i++)
	{
		merge_segment_c *S = mug_segments[i];

		SYS_ASSERT(S);
		SYS_ASSERT(S->start);

		DM_AddLinedef(S->start->index, S->end->index,
				S->front ? S->front->index : -1,
				S->back  ? S->back->index  : -1,
				0, 1 /*impassible*/, 0,
				NULL /* args */);
	}
}
#endif


//------------------------------------------------------------------------

static void DM_ExtraFloors(doom_sector_c *S, region_c *R);


static void DM_LightInSector(doom_sector_c *S, region_c *R,
                             csg_property_set_c *f_face,
                             csg_property_set_c *c_face)
{
	// use the shade computed in CSG_Shade()
	S->light = R->shade;

	S->light = CLAMP(0, S->light, 255);


	// handle "fx_delta" property for light effects

	if (S->special)
	{
		int delta = c_face->getInt("fx_delta");

		if (delta == 0)
			delta = f_face->getInt("fx_delta");

		if (delta > 0)
		{
			S->light2 = S->light - delta;
		}
	}

	// TODO: Doom 64 TC support : colored sectors
	//       this value is a sector type, and has lowest priority
	if (S->special == 0)
	{
		// blah blah
	}
}


static void DM_MakeSector(region_c *R)
{
	// completely solid (no gaps) ?
	if (R->gaps.size() == 0)
	{
		R->index = -1;
		return;
	}


	doom_sector_c *S = new doom_sector_c;

	S->region = R;
	R->index = (int)dm_sectors.size();

	dm_sectors.push_back(S);


	csg_brush_c *B = R->gaps.front()->bottom;
	csg_brush_c *T = R->gaps.back() ->top;

	csg_property_set_c *f_face = &B->t.face;
	csg_property_set_c *c_face = &T->b.face;

	// determine floor and ceiling heights
	double f_delta = f_face->getDouble("delta_z");
	double c_delta = c_face->getDouble("delta_z");

	S->f_h = I_ROUND(B->t.z + f_delta);
	S->c_h = I_ROUND(T->b.z + c_delta);

	// when delta-ing up the floor, limit it to the ceiling
	// (this can be important in outdoor rooms)
	if (f_delta > (fabs(c_delta) + 7) && S->f_h > S->c_h)
		S->f_h = S->c_h;

	if (S->c_h < S->f_h)
		S->c_h = S->f_h;

	S->f_tex = f_face->getStr("tex", dummy_plane_tex.c_str());
	S->c_tex = c_face->getStr("tex", dummy_plane_tex.c_str());

	int f_mark = f_face->getInt("mark");
	int c_mark = c_face->getInt("mark");

	S->mark = f_mark ? f_mark : c_mark;

	S->sound_area = f_face->getInt("sound_area");

	S->is_cave = (f_face->getInt("is_cave") > 0);


	// floors have priority over ceilings
	int f_special = f_face->getInt("special");
	int c_special = c_face->getInt("special");

	int f_tag = f_face->getInt("tag");
	int c_tag = c_face->getInt("tag");

	if (f_special || ! c_special)
	{
		S->special = f_special;
		S->tag = (f_tag > 0) ? f_tag : c_tag;

		S->misc_flags |= SEC_FLOOR_SPECIAL;
	}
	else
	{
		S->special = c_special;
		S->tag = (c_tag > 0) ? c_tag : f_tag;

		S->misc_flags |= SEC_CEIL_SPECIAL;
	}


	DM_LightInSector(S, R, f_face, c_face);


	// find brushes floating in-between --> make extrafloors

	DM_ExtraFloors(S, R);
}


static void DM_CreateSectors()
{
	for (unsigned int i = 0 ; i < all_regions.size() ; i++)
	{
		DM_MakeSector(all_regions[i]);
	}
}


static void DM_TryGrabFloor(doom_sector_c *D1, int sec_idx)
{
	// clear the fake special now
	D1->special = 0;

	// to prevent creating inescapable pits, we ensure that only a
	// single lower neighbor can be used (copied from) -- multiple
	// ones cause the floor to stay the same.

	std::string got_tex;
	int got_floor = IVAL_NONE;
	int got_special = 0;
	int got_light = 0;

	for (unsigned int i = 0 ; i < all_regions.size() ; i++)
	{
		region_c *R = all_regions[i];

		if (R->index != sec_idx)
			continue;

		for (unsigned int k = 0 ; k < R->snags.size() ; k++)
		{
			snag_c *S = R->snags[k];

			region_c *N = S->partner ? S->partner->region : NULL;

			if (!N || N->index < 0)
				continue;

			// require partner in a different sector

			doom_sector_c *D2 = dm_sectors[N->index];

			SYS_ASSERT(! D2->isUnused());

			if (D2 == D1)
				continue;

			if (D2->f_h >= D1->f_h)
				continue;

			if (got_floor != IVAL_NONE && D2->f_h != got_floor)
			{
				// Failed : two neighbors at different heights
				return;
			}

			got_tex     = D2->f_tex;
			got_floor   = D2->f_h;
			got_special = D2->special;
			got_light   = D2->light;
		}
	}

	if (got_floor == IVAL_NONE)
	{
		// no lower neighbor sector to grab
		return;
	}

	D1->f_tex   = got_tex;
	D1->f_h     = got_floor;
	D1->special = got_special;
	D1->light   = got_light;
}


static void DM_GrabNeighborFloors()
{
	for (unsigned int i = 0 ; i < all_regions.size() ; i++)
	{
		region_c *R = all_regions[i];

		if (R->index < 0)
			continue;

		doom_sector_c *D1 = dm_sectors[R->index];

		SYS_ASSERT(! D1->isUnused());

		if (D1->special == SEC_GRAB_NB_FLOOR)
		{
			DM_TryGrabFloor(D1, R->index);
		}
	}
}


static int DM_CoalescePass()
{
	int changes = 0;

	for (unsigned int i = 0 ; i < all_regions.size() ; i++)
	{
		region_c *R = all_regions[i];

		if (R->index < 0)
			continue;

		doom_sector_c *D1 = dm_sectors[R->index];

		for (unsigned int k = 0 ; k < R->snags.size() ; k++)
		{
			snag_c *S = R->snags[k];

			region_c *N = S->partner ? S->partner->region : NULL;

			if (!N || N->index < 0)
				continue;

			// only check the relationship once
			if (N->index <= R->index)
				continue;

			doom_sector_c *D2 = dm_sectors[N->index];

			if (D2->ShouldMerge(D1))
			{
				D2->MarkUnused();

				N->index = R->index;

				D1->is_cave |= D2->is_cave;

				changes++;
			}
		}
	}

	// fprintf(stderr, "DM_CoalescePass  changes:%d\n", changes);

	return changes;
}


static void DM_CoalesceSectors()
{
	while (DM_CoalescePass() > 0)
	{ }

  	DM_GrabNeighborFloors();

	// Note: we cannot remove & delete the unused sectors since the
	// region_c::index fields would need to be updated as well.
}


//------------------------------------------------------------------------

static doom_vertex_c * DM_MakeVertex(int x, int y)
{
	// look for existing vertex
	int combo = (x << 16) + y;

	if (dm_vertex_map.find(combo) != dm_vertex_map.end())
	{
		return dm_vertices[dm_vertex_map[combo]];
	}

	// create new one
	doom_vertex_c * V = new doom_vertex_c(x, y);

	dm_vertex_map[combo] = dm_vertices.size();

	dm_vertices.push_back(V);

	return V;
}


static int NaturalXOffset(doom_linedef_c *L, int side)
{
	double along;

	if (side == 0)
		along = AlongDist(0, 0,  L->start->x, L->start->y, L->end->x, L->end->y);
	else
		along = AlongDist(0, 0,  L->end->x, L->end->y, L->start->x, L->start->y);

	return I_ROUND(- along);
}


static int CalcXOffset(snag_c *S, brush_vert_c *V, int ox) 
{
	double along = 0;

	if (S)
	{
		along = ComputeDist(V->x, V->y, S->x2, S->y2);
	}

	return (int)(along + ox);
}


static int NormalizeYOffset(int oy)
{
	if (oy >= 256)
		oy &= 255;
	else if (oy < -256)
		oy = - (-oy & 255);

	return oy;
}


static doom_sidedef_c * DM_MakeSidedef(
	doom_linedef_c *L,
	doom_sector_c *sec, doom_sector_c *back,
	snag_c *snag, snag_c *other,
	brush_vert_c *rail,
	bool unpeg_L, bool unpeg_U)
{
	if (! sec)
		return NULL;


	doom_sidedef_c *SD = new doom_sidedef_c;

	dm_sidedefs.push_back(SD);

	SD->sector = sec;


	// the 'natural' X/Y offsets
	SD->x_offset = IVAL_NONE;  // updated in DM_AlignTextures()
	SD->y_offset = - sec->c_h;


	brush_vert_c *lower = NULL;
	brush_vert_c *upper = NULL;

	const char *dummy_tex = dummy_wall_tex.c_str();

	// Note: 'snag' actually faces into the region _behind_ this sidedef

	if (! snag || snag->region->gaps.empty())
	{
		// ONE SIDED LINE

		if (snag)
		{
			double z = (sec->f_h + sec->c_h) / 2.0;
			lower = snag->FindOneSidedVert(z);
		}

		if (! lower)
		{
			SD->mid = dummy_tex;
		}
		else
		{
			SD->mid = lower->face.getStr("tex", dummy_tex);

			int ox = lower->face.getInt("u1", IVAL_NONE);
			int oy = lower->face.getInt("v1", IVAL_NONE);

			if (ox != IVAL_NONE)
				SD->x_offset = CalcXOffset(snag, lower, ox);

			if (oy != IVAL_NONE)
				SD->y_offset = oy;
		}
	}
	else
	{
		// Two Sided Line

		csg_brush_c *l_brush = snag->region->gaps.front()->bottom;
		csg_brush_c *u_brush = snag->region->gaps. back()->top;

		lower = snag->FindBrushVert(l_brush);
		upper = snag->FindBrushVert(u_brush);

		// offset handling
		int r_ox = IVAL_NONE;
		int r_oy = IVAL_NONE;

		int l_ox = IVAL_NONE;
		int l_oy = IVAL_NONE;

		int u_ox = IVAL_NONE;
		int u_oy = IVAL_NONE;

		if (rail)
		{
			const char *rail_tex = rail->face.getStr("tex", NULL);

			if (rail_tex)
			{
				SD->mid = rail_tex;

				r_ox = rail->face.getInt("u1", IVAL_NONE);
				r_oy = rail->face.getInt("v1", 0);

				// adjust Y-offset for higher floor than expected
				int sec_max_z = MAX(sec->f_h, back->f_h);

				r_oy += I_ROUND(rail->parent->b.z) - sec_max_z;
			}
		}

		if (lower)
		{
			l_ox = lower->face.getInt("u1", IVAL_NONE);
			// on a moving brush, default Y offset is zero
			l_oy = lower->face.getInt("v1", l_brush->props.getInt("mover") ? 0 : IVAL_NONE);
		}

		if (upper)
		{
			u_ox = upper->face.getInt("u1", IVAL_NONE);
			u_oy = upper->face.getInt("v1", u_brush->props.getInt("mover") ? 0 : IVAL_NONE);
		}

		if (back && back->f_h > sec->f_h && !rail && l_oy != IVAL_NONE)
		{
			L->flags &= ~MLF_LowerUnpeg;
			unpeg_L = false;
		}

		if (r_ox != IVAL_NONE)
			SD->x_offset = CalcXOffset(snag, rail,  r_ox);
		else if (l_ox != IVAL_NONE)
			SD->x_offset = CalcXOffset(snag, lower, l_ox);
		else if (u_ox != IVAL_NONE)
			SD->x_offset = CalcXOffset(snag, upper, u_ox);

		if (r_oy != IVAL_NONE)
			SD->y_offset = r_oy;
		else if (l_oy != IVAL_NONE)
			SD->y_offset = l_oy;
		else if (u_oy != IVAL_NONE)
			SD->y_offset = u_oy;

		// get textures, but fallback to something safe
		if (! lower) lower = l_brush->verts[0];
		if (! upper) upper = u_brush->verts[0];

		SD->lower = lower->face.getStr("tex", dummy_tex);
		SD->upper = upper->face.getStr("tex", dummy_tex);
	}

	SD->y_offset = NormalizeYOffset(SD->y_offset);

	return SD;
}


static csg_property_set_c * DM_FindTrigger(snag_c *S, doom_sector_c *front, doom_sector_c *back)
{
	// triggers require a two-sided line
	if (! front || ! back)
		return NULL;

	float f_max = MAX(front->f_h, back->f_h) + 4;
	float c_min = MIN(front->c_h, back->c_h) - 4;

	if (f_max >= c_min)
		return NULL;

	for (int side = 0 ; side < 2 ; side++)
	{
		snag_c *test_S = (side == 0) ? S->partner : S;

		if (! test_S)
			continue;

		for (unsigned int k = 0 ; k < test_S->sides.size() ; k++)
		{
			brush_vert_c *V = test_S->sides[k];

			if (V->parent->bkind != BKIND_Trigger)
				continue;

			// is the trigger in lala land?
			if (V->parent->b.z > c_min || V->parent->t.z < f_max)
				continue;

			if (! V->face.getStr("special"))
				continue;

			return &V->face;  // found it
		}
	}

	return NULL;
}


static csg_property_set_c * DM_FindSpecial(snag_c *S, region_c *R1, region_c *R2)
{
	brush_vert_c *V;

	// we want the brushes for the floor or ceiling next to a linedef
	// to take precedence over any other brushes.  Hence the passes.

	// Note: ignoring TRIGGER brushes here (they are found separately)

	for (int pass = 0 ; pass < 4 ; pass++)
	{
		int side = pass & 1;

		// try partner first
		snag_c *test_S = (side == 0) ? S->partner : S;

		if (! test_S)
			continue;

		if (pass < 2)
		{
			// region to test is one _behind_ the snag
			region_c *test_R = (side == 0) ? R1 : R2;

			if (! test_R || test_R->gaps.empty())
				continue;

			V = test_S->FindBrushVert(test_R->gaps.front()->bottom);

			if (V && V->face.getStr("special") &&
				V->parent->bkind != BKIND_Trigger)
				return &V->face;

			V = test_S->FindBrushVert(test_R->gaps.back()->top);

			if (V && V->face.getStr("special") &&
				V->parent->bkind != BKIND_Trigger)
				return &V->face;
		}
		else
		{
			// check every brush_vert in the snag
			for (unsigned int i = 0 ; i < test_S->sides.size() ; i++)
			{
				V = test_S->sides[i];

				if (V && V->face.getStr("special") &&
					V->parent->bkind != BKIND_Trigger)
					return &V->face;
			}
		}
	}

	return NULL;
}


static brush_vert_c * DM_FindRail(const snag_c *S, const region_c *R, const region_c *N)
{
	if (! S)
		return NULL;

	// railings require a two-sided line
	if (! R || ! N || ! S->partner)
		return NULL;

	if (R->gaps.size() == 0 || N->gaps.size() == 0)
		return NULL;

	for (unsigned int k = 0 ; k < S->sides.size() ; k++)
	{
		brush_vert_c *V = S->sides[k];

		if (V->parent->bkind != BKIND_Rail)
			continue;

		if (V->face.getStr("tex", NULL))
			return V;  // found it!
	}

	return NULL;
}


static void DM_DeterminePegging(doom_linedef_c *L,
								region_c *front, region_c *back,
								bool has_rail)
{
	// sanity check
	if (front->gaps.empty())
		return;

	// one-sided?
	// set LOWER-UNPEG only when ceiling is a door
	if (! back || back->gaps.empty())
	{
		csg_brush_c *T = front->gaps.back() ->top;

		if (T->props.getInt("mover"))
		{
			L->flags |= MLF_LowerUnpeg;
		}

		return;
	}

	// two-sided
	// set LOWER-UNPEG unless one side is a lift with higher floor_h
	// set UPPER-UNPEG unless one side is a door with lower ceil_h
	//
	// Note: later we will clear LOWER-UNPEG if lower is visible and it
	//       has a y_offset.

	csg_brush_c *B1 = front->gaps.front()->bottom;
	csg_brush_c *T1 = front->gaps.back() ->top;

	csg_brush_c *B2 =  back->gaps.front()->bottom;
	csg_brush_c *T2 =  back->gaps.back() ->top;

	if (has_rail)
	{
		L->flags |= MLF_LowerUnpeg;
	}
	else if ((/*  back->f_h > front->f_h && */ B2->props.getInt("mover")) ||
			 (/* front->f_h >  back->f_h && */ B1->props.getInt("mover")))
	{
		// pegged lower
	}
	else
	{
		L->flags |= MLF_LowerUnpeg;
	}

	if ((/*  back->c_h < front->c_h && */ T2->props.getInt("mover")) ||
		(/* front->c_h <  back->c_h && */ T1->props.getInt("mover")))
	{
		// pegged upper
	}
	else
	{
		L->flags |= MLF_UpperUnpeg;
	}
}


static void DM_MakeLine(region_c *R, snag_c *S)
{
	region_c *N = S->partner ? S->partner->region : NULL;

	// for two-sided snags, only make one linedef from the pair
	if (S->seen)
		return;

	S->seen = true;

	if (S->partner)
		S->partner->seen = true;

	// skip snags which would become zero length linedefs
	int x1 = I_ROUND(S->x1);
	int y1 = I_ROUND(S->y1);

	int x2 = I_ROUND(S->x2);
	int y2 = I_ROUND(S->y2);

	if (x1 == x2 && y1 == y2)
	{
//		LogPrintf("WARNING: degenerate linedef @ (%1.0f %1.0f)\n", S->x1, S->y1);
		return;
	}


	doom_sector_c *front = dm_sectors[R->index];
	doom_sector_c *back  = NULL;

	if (N && N->index >= 0)
		back = dm_sectors[N->index];


	brush_vert_c *f_rail = DM_FindRail(S->partner, R, N);
	brush_vert_c *b_rail = DM_FindRail(S, N, R);

	csg_property_set_c *trig = DM_FindTrigger(S, front, back);
	csg_property_set_c *spec = DM_FindSpecial(S, R, N);

	bool use_trig = false;

	int L_special = 0;
	int L_tag     = 0;

	if (spec)
	{
		L_special = spec->getInt("special");
		L_tag     = spec->getInt("tag");
	}
	
	// trigger brushes are secondary to specials on brush verts
	// (because it is bad when a door or lift gets dudded).
	if (L_special == 0 && trig)
	{
		use_trig  = true;

		L_special = trig->getInt("special");
		L_tag     = trig->getInt("tag");
	}


	// skip the line if same on both sides, except when it has a rail or special
	if (front == back && !f_rail && !b_rail && !L_special)
		return;


	// update map's bounding box
	map_bound_x1 = MIN(map_bound_x1, MIN(x1, x2));
	map_bound_y1 = MIN(map_bound_y1, MIN(y1, y2));

	map_bound_x2 = MAX(map_bound_x2, MAX(x1, x2));
	map_bound_y2 = MAX(map_bound_y2, MAX(y1, y2));


	// create the line...

	doom_linedef_c *L = new doom_linedef_c;

	dm_linedefs.push_back(L);


	L->start = DM_MakeVertex(x1, y1);
	L->end   = DM_MakeVertex(x2, y2);

	SYS_ASSERT(L->start != L->end);

	L->start->AddLine(L);
	L->end  ->AddLine(L);

	L->CalcLength();


	// set pegging _before_ making sidedefs

	DM_DeterminePegging(L, R, N, (f_rail || b_rail) ? true : false);

	bool unpeg_L = (L->flags & MLF_LowerUnpeg) != 0;
	bool unpeg_U = (L->flags & MLF_UpperUnpeg) != 0;


	L->front = DM_MakeSidedef(L, front, back, S->partner, S, f_rail, unpeg_L, unpeg_U);
	L->back  = DM_MakeSidedef(L, back, front, S, S->partner, b_rail, unpeg_L, unpeg_U);

	SYS_ASSERT(L->front || L->back);

	if (L->ShouldFlip())
	{
		L->Flip();
	}


	// set the special and tag

	L->special = L_special;
	L->tag     = L_tag;

	if (use_trig)
		trig->getHexenArgs(L->args);
	else if (spec)
		spec->getHexenArgs(L->args);


	// flags...

	if (! L->back)
	{
		L->flags |= MLF_BlockAll;
	}
	else
	{
		L->flags |= MLF_TwoSided;
	}

	if (f_rail) L->flags |= f_rail->face.getInt("flags");
	if (b_rail) L->flags |= b_rail->face.getInt("flags");
	if (spec)   L->flags |= spec->getInt("flags");

	if (L->special == LIN_FAKE_UNPEGGED)
	{
		L->special = 0;
		L->flags |= MLF_LowerUnpeg;
	}

	if (L->front && L->back && L->front->sector->sound_area != L->back->sector->sound_area)
	{
		L->flags |= MLF_SoundBlock;
	}
}


static void DM_CreateLinedefs()
{
	map_bound_x1 = +99999;
	map_bound_y1 = +99999;
	map_bound_x2 = -99999;
	map_bound_y2 = -99999;

	for (unsigned int i = 0 ; i < all_regions.size() ; i++)
	{
		region_c *R = all_regions[i];

		if (R->index < 0)
			continue;

		for (unsigned int k = 0 ; k < R->snags.size() ; k++)
		{
			DM_MakeLine(R, R->snags[k]);
		}
	}

	// coordinate for first dummy sector
	dummy_pos_x = map_bound_x1 - (map_bound_x1 & 31);
	dummy_pos_y = map_bound_y1 - (map_bound_y1 & 31) - 128;
}


//------------------------------------------------------------------------

static bool DM_TryMergeLine(doom_vertex_c *V)
{
	doom_linedef_c *A = V->lines[0];
	doom_linedef_c *B = V->lines[1];

	SYS_ASSERT(A->isValid());
	SYS_ASSERT(B->isValid());

	return A->TryMerge(B);
}


static void DM_MergeColinearLines(bool show_count = true)
{
	int count = 0;

	for (int pass = 0 ; pass < 3 ; pass++)
		for (int i = 0 ; i < (int)dm_vertices.size() ; i++)
			if (dm_vertices[i]->getNumLines() == 2)
				if (DM_TryMergeLine(dm_vertices[i]))
					count++;

	if (show_count)
		LogPrintf("Merged %d colinear lines\n", count);
}


static doom_linedef_c * DM_FindSimilarLine(doom_linedef_c *L, doom_vertex_c *V)
{
	doom_linedef_c *best = NULL;
	int best_score = -1;

	for (int i = 0 ; i < 4 ; i++)
	{
		doom_linedef_c *M = V->lines[i];

		if (! M) break;
		if (M == L) continue;

		SYS_ASSERT(M->isValid());

		if (! L->isFrontSimilar(M))
			continue;

		int score = 0;

		if (! L->back && ! M->back)
			score += 20;

		if (L->ColinearWith(M))
			score += 10;

		if (score > best_score)
		{
			best = M;
			best_score = score;
		}
	}

	return best;
}


static void DM_AlignTextures()
{
	int i;
	int count = 0;

	for (i = 0 ; i < (int)dm_linedefs.size() ; i++)
	{
		doom_linedef_c *L = dm_linedefs[i];

		if (! L->isValid())
			continue;

		L->sim_prev = DM_FindSimilarLine(L, L->start);
		L->sim_next = DM_FindSimilarLine(L, L->end);

		if (L->front->x_offset == IVAL_NONE && ! L->sim_prev && ! L->sim_next)
			L->front->x_offset = NaturalXOffset(L, 0);

		if (L->back && L->back->x_offset == IVAL_NONE)
			L->back->x_offset = NaturalXOffset(L, 1);
	}

	// when there are line loops where every x_offset is IVAL_NONE, then
	// it's a chicken and egg problem.  Hence we perform a bunch of passes,
	// the first pass checks every 256 lines for IVAL_NONE (which will then
	// propagate to similar neighbors), the next pass checks 128, 64, etc..

	for (int pass = 12 ; pass >= 0 ; pass--)
	{
		int naturals = 0;
		int prev_count = 0;
		int next_count = 0;

		for (i = 0 ; i < (int)dm_linedefs.size() ; i++)
		{
			doom_linedef_c *L = dm_linedefs[i];

			if (! L->isValid())
				continue;

			if (L->front->x_offset == IVAL_NONE)
			{
				if (pass >= 12)
					continue;

				int mask = (1 << pass) - 1;

				if ((i & mask) == 0)
				{
					L->front->x_offset = NaturalXOffset(L, 0);
					naturals++;
				}
			}

			doom_linedef_c *P = L;
			doom_linedef_c *N = L;

			while (P->sim_prev && P->sim_prev->front->x_offset == IVAL_NONE)
			{
				P->sim_prev->front->x_offset = P->front->x_offset - I_ROUND(P->sim_prev->length);
				P = P->sim_prev;
				prev_count++;
			}

			while (N->sim_next && N->sim_next->front->x_offset == IVAL_NONE)
			{
				N->sim_next->front->x_offset = N->front->x_offset + I_ROUND(N->length);
				N = N->sim_next;
				next_count++;
			}
		}

		count += prev_count + next_count;

		//  DebugPrintf("AlignTextures pass %d : naturals:%d prevs:%d nexts:%d\n",
		//              pass, naturals, prev_count, next_count);
	}

	LogPrintf("Aligned %d textures\n", count);
}


static bool RoundWouldClobber(int cx, int cy, int ox, int oy,
                              const doom_vertex_c *ignore1,
                              const doom_vertex_c *ignore2,
                              const doom_vertex_c *ignore3)
{
	int x1 = MIN(cx, ox);
	int y1 = MIN(cy, oy);

	int x2 = MAX(cx, ox);
	int y2 = MAX(cy, oy);

	for (unsigned int i = 0 ; i < dm_vertices.size() ; i++)
	{
		const doom_vertex_c *V = dm_vertices[i];

		if (V == ignore1 || V == ignore2 || V == ignore3)
			continue;

		if (! V->lines[0])  // dud vertex?
			continue;

		// allow vertex at opposite corner
		// (this is the cheesy way -- the proper way would be to check if the
		//  vertex is within the triangle).
		if (V->x == ox && V->y == oy)
			continue;

		if (x1 <= V->x && V->x <= x2 && y1 <= V->y && V->y <= y2)
			return true;
	}

	return false;  // OK
}


static int TryRoundAtVertex(doom_vertex_c *V)
{
	if (V->getNumLines() != 2)
		return 0;

	doom_linedef_c *LX = V->lines[0];
	doom_linedef_c *LY = V->lines[1];

	// this probably cannot happen, but just in case...
	if (! LX->isValid() || ! LY->isValid())
		return 0;

	if (LX->hasRail() || LY->hasRail())
		return 0;

	SYS_ASSERT(LX->start == V || LX->end == V);
	SYS_ASSERT(LY->start == V || LY->end == V);

	if (LX->isHoriz() && LY->isVert())
	{
		/* OK */
	}
	else if (LX->isVert() && LY->isHoriz())
	{
		std::swap(LX, LY);
	}
	else
	{
		return 0;  // not a square corner
	}

	// must be two-sided lines, sectors only differ by light level
	if (! (LX->front && LX->back)) return 0;
	if (! (LY->front && LY->back)) return 0;

	// must be in a cave
	if (! LX->front->sector->is_cave) return 0;
	if (! LY->front->sector->is_cave) return 0;

	if (! LX->front->sector->MatchNoLight(LX->back->sector))
		return 0;

	// too short?
	if (LX->length < 16 || LY->length < 16)
		return 0;


	int x_len = LX->length;
	int y_len = LY->length;

	bool x_rounded_half = false;
	bool y_rounded_half = false;

	if (x_len < 32 || (x_len <= 64 && LX->OtherVert(V)->rounded_half))
	{
		/* don't add new vertex */
	}
	else if (x_len > 128)
	{
		x_len = 64;
	}
	else
	{
		x_len /= 2;
		x_rounded_half = true;
	}

	if (y_len < 32 || (y_len <= 64 && LY->OtherVert(V)->rounded_half))
	{
		/* don't add new vertex */
	}
	else if (y_len > 128)
	{
		y_len = 64;
	}
	else
	{
		y_len /= 2;
		y_rounded_half = true;
	}

	int x_dir = (LX->start->x < LX->end->x) ? +1 : -1;
	int y_dir = (LY->start->y < LY->end->y) ? +1 : -1;

	if (LX->end == V) x_dir = -x_dir;
	if (LY->end == V) y_dir = -y_dir;

	int x_other = V->x + x_dir * x_len;
	int y_other = V->y + y_dir * y_len;


	// test if any geometry would be clobbered
	// (only need to see if a used vertex lies inside the triangle)

	if (RoundWouldClobber(V->x, V->y, x_other, y_other, V, LX->OtherVert(V), LY->OtherVert(V)))
		return 0;


	/* handle four cases :
	 *   1. no new vertices
	 *   2. add one new vertex (split horizontal line)
	 *   3. add one new vertex (split vertical line)
	 *   4. add two new vertices -- split both
	 */

	if (x_len == LX->length && y_len == LY->length)
	{
		doom_vertex_c *V2 = LY->OtherVert(V);

		V2->ReplaceLine(LY, LX);

		if (LX->start == V)
			LX->start = V2;
		else
			LX->end = V2;

		SYS_ASSERT(LX->start != LX->end);

		LX->CalcLength();

		LY->Kill();
		V->Kill();

		return 1;
	}

	doom_vertex_c * VX;
	doom_vertex_c * VY;
	doom_linedef_c * L;

	/* case 2 */

	if (y_len == LY->length)
	{
		VX = DM_MakeVertex(V->x + x_dir * x_len,  V->y);
		VY = VX;

		VX->rounded_half = x_rounded_half;

		VX->AddLine(LX);
		VX->AddLine(LY);

		goto finished;
	}

	/* case 3 */

	if (x_len == LX->length)
	{
		// essentially replacing V with a new split vertex

		VY = DM_MakeVertex(V->x,  y_dir * y_len + V->y);
		VX = VY;

		VY->rounded_half = y_rounded_half;

		VY->AddLine(LX);
		VY->AddLine(LY);

		goto finished;
	}


	/* case 4 */

	VX = DM_MakeVertex(V->x + x_dir * x_len,  V->y);
	VY = DM_MakeVertex(V->x,  y_dir * y_len + V->y);

	SYS_ASSERT(VX != VY);

	VX->rounded_half = x_rounded_half;
	VY->rounded_half = y_rounded_half;


	L = new doom_linedef_c(*LX);

	if (L->front) dm_sidedefs.push_back(L->front);
	if (L->back ) dm_sidedefs.push_back(L->back );

	dm_linedefs.push_back(L);


	// orientation of L must match LX, since we copied sidedefs from LX
	L->start = VX;
	L->end   = VY;

	if (LX->start == V)
		std::swap(L->start, L->end);

	L->CalcLength();


	VX->AddLine(L);
	VX->AddLine(LX);

	VY->AddLine(L);
	VY->AddLine(LY);


finished:
	if (LX->start == V)
		LX->start = VX;
	else
		LX->end = VX;

	if (LY->start == V)
		LY->start = VY;
	else
		LY->end = VY;

	SYS_ASSERT(LX->start != LX->end);
	SYS_ASSERT(LY->start != LY->end);

	LX->CalcLength();
	LY->CalcLength();

	V->Kill();

	return 1;
}


static void DM_RoundCorners()
{
	/*
	 * Looks for corners where two (and only two) linedefs meet and
	 * the linedefs are axis-aligned, and tries to add a diagonal at
	 * the corner.
	 *
	 * This is mainly to help reduce jaggies produces by the cave
	 * lighting algorithm, and is only used in caves.
	 */

	int count = 0;

	for (int pass = 0 ; pass < 2 ; pass++)
		for (int i = 0 ; i < (int)dm_vertices.size() ; i++)
			if (dm_vertices[i]->getNumLines() == 2)
				count += TryRoundAtVertex(dm_vertices[i]);

	LogPrintf("Rounded %d square corners\n", count);

	// need this again, since we often create co-linear diagonals
	DM_MergeColinearLines(false /* show_count */);
}


//------------------------------------------------------------------------
//  DUMMY SECTORS
//------------------------------------------------------------------------

#define DUMMY_MAX_SHARE  8

class dummy_line_info_c
{
public:
	std::string tex;

	int special;
	int tag;
	int flags;

public:
	dummy_line_info_c(std::string& _tex, int _special = 0, int _tag = 0, int _flags = 0 ) :
		tex(_tex), special(_special), tag(_tag), flags(_flags)
	{ }

	~dummy_line_info_c()
	{ }
};


class dummy_sector_c
{
public:
	doom_sector_c *sector;
	doom_sector_c *pair;

	int share_count;

	dummy_line_info_c * info[DUMMY_MAX_SHARE];

public:
	dummy_sector_c(doom_sector_c *_sec = NULL, doom_sector_c *_pair = NULL) :
		sector(_sec), pair(_pair), share_count(0)
	{
		for (int i = 0 ; i < DUMMY_MAX_SHARE ; i++)
			info[i] = NULL;
	}

	~dummy_sector_c()
	{
		for (int i = 0 ; i < DUMMY_MAX_SHARE ; i++)
			delete info[i];
	}

	bool isFull() const
	{
		return (share_count >= DUMMY_MAX_SHARE);
	}

	void AddInfo(std::string& tex, int special, int tag, int flags)
	{
		SYS_ASSERT(! isFull());

		info[share_count++] = new dummy_line_info_c(tex, special, tag, flags);
	}

	/// construction ///

	doom_sidedef_c * MakeSidedef(int what, int other_what, int index)
	{
		if (what == 0)
			return NULL;

		doom_sector_c *cur_sec = (what == 2) ? pair : sector;

		SYS_ASSERT(cur_sec);


		doom_sidedef_c *SD = new doom_sidedef_c;

		dm_sidedefs.push_back(SD);

		SD->sector = cur_sec;

		if (index >= 0 && index < share_count)
		{
			SD->upper = info[index]->tex;
			SD->mid   = info[index]->tex;
			SD->lower = info[index]->tex;
		}
		else
		{
			SD->upper = dummy_wall_tex;
			SD->mid   = dummy_wall_tex;
			SD->lower = dummy_wall_tex;
		}

		// on two-sided line, don't set railing
		if (other_what > 0)
			SD->mid = std::string("-");

		SD->x_offset = 0;
		SD->y_offset = 0;

		return SD;
	}


	void MakeLine(int index, int x1, int y1, int x2, int y2,
			int front, int back, bool is_split = false)
	{
		// handle splitting via a single recurse

		if (!is_split && index >= 0 && share_count > (4 + index))
		{
			int mx = (x1 + x2) / 2;
			int my = (y1 + y2) / 2;

			MakeLine(index,   x1, y1, mx, my, front, back, true);
			MakeLine(index+4, mx, my, x2, y2, front, back, true);

			return;
		}

		// front and back are: 0 for VOID, 1 for SECTOR, 2 for PAIR

		SYS_ASSERT(sector);

		if (front == 2 && ! pair) front = 1;
		if ( back == 2 && ! pair)  back = 1;

		if (front == back)
			return;


		doom_linedef_c *L = new doom_linedef_c;

		dm_linedefs.push_back(L);


		L->start = DM_MakeVertex(x1, y1);
		L->end   = DM_MakeVertex(x2, y2);

		L->CalcLength();

		if (index >= 0 && index < share_count)
		{
			SYS_ASSERT(info[index]);

			L->special = info[index]->special;
			L->tag     = info[index]->tag;
			L->flags   = info[index]->flags;
		}

		L->flags |= MLF_BlockAll | MLF_DontDraw;

		if (front > 0 && back > 0)
			L->flags |= MLF_TwoSided;

		L->front = MakeSidedef(front, back,  index);
		L->back  = MakeSidedef(back,  front, index);

		SYS_ASSERT(L->front);
	}


	void Construct(int index)
	{
		// determine coordinate of bottom/left corner
		int x1 = dummy_pos_x;
		int y1 = dummy_pos_y;

		dummy_pos_x += 32;

		if (dummy_pos_x + 32 >= map_bound_x2)
		{
			dummy_pos_x = map_bound_x1 - (map_bound_x1 & 31);
			dummy_pos_y -= 32;
		}

		int x2 = x1 + 16;
		int y2 = y1 + 16;

		MakeLine( 0, x1,y1, x1,y2, 1,0);
		MakeLine( 1, x1,y2, x2,y2, 1,0);
		MakeLine( 2, x2,y2, x2,y1, 2,0);
		MakeLine( 3, x2,y1, x1,y1, 2,0);

		MakeLine(-1, x1,y1, x2,y2, 2,1);
	}
};


static void DM_CreateDummies()
{
	for (unsigned int i = 0 ; i < dm_dummies.size() ; i++)
	{
		dm_dummies[i]->Construct((int) i);
	}
}


static dummy_sector_c * Dummy_New(doom_sector_c *sec, doom_sector_c *pair = NULL)
{
	dummy_sector_c *dum = new dummy_sector_c(sec, pair);

	dm_dummies.push_back(dum);

	return dum;
}


static dummy_sector_c * Dummy_FindMatch(doom_sector_c *new_sec)
{
	for (unsigned int i = 0 ; i < dm_dummies.size() ; i++)
	{
		dummy_sector_c *dum = dm_dummies[i];

		if (dum->isFull())
			continue;

		if (new_sec->MatchMost(dum->sector))
		{
			// won't need the newly created sector now
			new_sec->MarkUnused();

			return dum;
		}
	}

	// create a new one
	return Dummy_New(new_sec);
}


//------------------------------------------------------------------------
//  EXTRAFLOOR STUFF
//------------------------------------------------------------------------

static void DM_SolidExtraFloor(doom_sector_c *sec, gap_c *gap1, gap_c *gap2)
{
	extrafloor_c *EF = new extrafloor_c;

	dm_exfloors.push_back(EF);

	sec->AddExtrafloor(EF);


	EF->line_special = ef_solid_type;

	EF->u_special = gap2->bottom->b.face.getInt("special");
	EF->u_light   = gap2->bottom->b.face.getInt("light", sec->light - 24);
	EF->u_tag     = gap2->bottom->b.face.getInt("tag");

	if (EF->u_light < 112) EF->u_light = 112;

	if (sec->misc_flags & SEC_FLOOR_SPECIAL)
	{
		if (ef_solid_type == 281)  // Legacy mode
		{
			EF->u_special = gap2->bottom->t.face.getInt("special");
		}
		else   // EDGE mode
		{
			EF->u_special = sec->special;
			sec->special  = gap2->bottom->t.face.getInt("special");
		}
	}

	EF->top_h    = I_ROUND(gap2->bottom->t.z);
	EF->bottom_h = I_ROUND(gap1->   top->b.z);

	EF->top    = gap2->bottom->t.face.getStr("tex", dummy_plane_tex.c_str());
	EF->bottom = gap1->   top->b.face.getStr("tex", dummy_plane_tex.c_str());


	brush_vert_c *V = gap2->bottom->verts[0];

	EF->wall = V->face.getStr("tex", dummy_wall_tex.c_str());
}


static void DM_LiquidExtraFloor(doom_sector_c *sec, csg_brush_c *liquid)
{
	// Note: we don't care if the liquid is inside the floor or ceiling,
	//       perhaps that is intentional.  The engine should not mind.

	extrafloor_c *EF = new extrafloor_c;

	dm_exfloors.push_back(EF);

	sec->AddExtrafloor(EF);


	EF->line_special = ef_liquid_type;

	EF->u_special = liquid->t.face.getInt("special");
	EF->u_light   = liquid->t.face.getInt("light", 144);
	EF->u_tag     = liquid->t.face.getInt("tag");

	if (EF->line_special == 301)  // Legacy style
	{
		EF->bottom_h = sec->f_h;
		EF->top_h    = I_ROUND(liquid->t.z);
	}
	else  // EDGE style
	{
		EF->bottom_h = I_ROUND(liquid->t.z);
		EF->top_h    = EF->bottom_h + 128;   // not significant
	}

	EF->top    = liquid->t.face.getStr("tex", dummy_plane_tex.c_str());
	EF->bottom = EF->top;


	brush_vert_c *V = liquid->verts[0];

	EF->wall = V->face.getStr("tex", dummy_wall_tex.c_str());
}


static void DM_ExtraFloors(doom_sector_c *S, region_c *R)
{
	if (ef_liquid_type)
	{
		for (unsigned int g = R->gaps.size() - 1 ; g > 0 ; g--)
		{
			if (R->gaps[g]->liquid)
			{
				DM_LiquidExtraFloor(S, R->gaps[g]->liquid);
				break;
			}
		}
	}

	if (ef_solid_type)
	{
		// Note: top-to-bottom is the most natural order, because when
		// the engine adds an extrafloor into a sector, the upper part
		// remains the same and the lower part gets the new properties
		// (lighting/special) from the extrafloor.

		for (unsigned int g = R->gaps.size() - 1 ; g > 0 ; g--)
		{
			DM_SolidExtraFloor(S, R->gaps[g-1], R->gaps[g]);
		}
	}
}


static void DM_ExtraFloorNeighbors()
{
	// Collect sector neighbors which have the same extrafloors.
	// These can use the same tag in the dummy sector (and most
	// importantly, require less dummy sectors).

	for (unsigned int i = 0 ; i < dm_linedefs.size() ; i++)
	{
		doom_linedef_c *L = dm_linedefs[i];

		if (! L->isValid())
			continue;

		if (! L->back)
			continue;

		doom_sector_c *front_S = L->front->sector;
		doom_sector_c * back_S = L-> back->sector;

		if (front_S == back_S)
			continue;

		SYS_ASSERT(! front_S->unused);
		SYS_ASSERT(!  back_S->unused);

		if (front_S->SameExtraFloors(back_S))
		{
			front_S->ef_neighbors.push_back( back_S);
			back_S->ef_neighbors.push_back(front_S);
		}
	}
}


static void EXFL_MakeDummy(extrafloor_c *EF, int tag)
{
	doom_sector_c *new_sec = new doom_sector_c;

	dm_sectors.push_back(new_sec);

	new_sec->f_h = EF->bottom_h;
	new_sec->c_h = EF->top_h;

	new_sec->f_tex = EF->bottom;
	new_sec->c_tex = EF->top;

	new_sec->light   = EF->u_light;
	new_sec->special = EF->u_special;
	new_sec->tag     = EF->u_tag;


	dummy_sector_c *dum = Dummy_FindMatch(new_sec);

	dum->AddInfo(EF->wall, EF->line_special, tag, 0);
}


static void EXFL_SpreadTag(doom_sector_c *S, int tag)
{
	std::vector<doom_sector_c *> visits;

	visits.push_back(S);

	while (! visits.empty())
	{
		S = visits.back();  visits.pop_back();

		S->tag = tag;

		for (unsigned int k = 0 ; k < S->ef_neighbors.size() ; k++)
		{
			doom_sector_c *N = S->ef_neighbors[k];

			if (N->tag == 0)
				visits.push_back(N);
		}
	}
}


static void DM_ProcessExtraFloors()
{
	DM_ExtraFloorNeighbors();

	int tag = 10001;

	for (unsigned int i = 0 ; i < dm_sectors.size() ; i++)
	{
		doom_sector_c *S = dm_sectors[i];

		if (S->unused || S->exfloors.empty())
			continue;

		if (S->tag > 0)  // SHIT
			continue;

		EXFL_SpreadTag(S, tag);

		tag++;

		for (unsigned int k = 0 ; k < S->exfloors.size() ; k++)
		{
			extrafloor_c *EF = S->exfloors[k];

			EXFL_MakeDummy(EF, S->tag);
		}
	}
}


static int DM_CalcDoorLight(const doom_sector_c *S)
{
	int l_min = 192;
	int l_max = S->light;

	for (unsigned int i = 0 ; i < dm_linedefs.size() ; i++)
	{
		const doom_linedef_c *L = dm_linedefs[i];

		if (! L->isValid())
			continue;

		if (! L->back)
			continue;

		doom_sector_c *front = L->front->sector;
		doom_sector_c * back = L-> back->sector;

		if (front == back)
			continue;

		// we ignore closed neighbors

		if (front == S && back->f_h < back->c_h)
		{
			l_min = MIN(l_min, back->light);
			l_max = MAX(l_max, back->light);
		}

		if (back == S && front->f_h < front->c_h)
		{
			l_min = MIN(l_min, front->light);
			l_max = MAX(l_max, front->light);
		}
	}

	// usually pick the maximum, but if other neighbors are lower then
	// reduce it by 16 (or 32 if very high).

	if (l_max >= l_min + 16)
	{
		l_max -= 16;
	}

	if (l_max >= l_min + 32 && l_max >= 208)
	{
		l_max -= 16;
	}

	return l_max;
}


static void DM_ProcessLightFX()
{
	for (unsigned int i = 0 ; i < dm_sectors.size() ; i++)
	{
		doom_sector_c *S = dm_sectors[i];

		if (S->unused)
			continue;

		// handle lighting for closed doors -- get level from a neighbor
		if (S->f_h == S->c_h)
		{
			S->light = DM_CalcDoorLight(S);
		}

		if (S->special > 0 && S->light2 > 0)
		{
			doom_sector_c *new_sec = new doom_sector_c;

			dm_sectors.push_back(new_sec);

			new_sec->f_h   = 0;
			new_sec->c_h   = 1;
			new_sec->light = S->light2;

			new_sec->f_tex = dummy_plane_tex.c_str();
			new_sec->c_tex = dummy_plane_tex.c_str();

			Dummy_New(new_sec, S);
		}
	}
}


static doom_sector_c * DM_FindDepotPeer()
{
	// find the sector containing the "oblige_depot" entity

	for (unsigned int i = 0 ; i < all_regions.size() ; i++)
	{
		region_c *R = all_regions[i];

		if (R->index < 0)
			continue;

		doom_sector_c *S = dm_sectors[R->index];

		for (unsigned int k = 0 ; k < R->entities.size() ; k++)
		{
			csg_entity_c *E = R->entities[k];

			if (strcmp(E->id.c_str(), "oblige_depot") == 0)
				return S;
		}
	}

	LogPrintf("WARNING: cannot find peer for monster depot.\n");
	return NULL;
}


void DM_ProcessSecrets()
{
	// find the "oblige_secret" entities and mark the sector containing
	// it as SECRET (overriding any existing special).

	for (unsigned int i = 0 ; i < all_regions.size() ; i++)
	{
		region_c *R = all_regions[i];

		if (R->index < 0)
			continue;

		doom_sector_c *S = dm_sectors[R->index];

		for (unsigned int k = 0 ; k < R->entities.size() ; k++)
		{
			csg_entity_c *E = R->entities[k];

			if (strcmp(E->id.c_str(), "oblige_secret") == 0)
				S->special = 9;
		}
	}
}


static void DM_ConvertSectorToOther(doom_sector_c *src, doom_sector_c *dest)
{
	for (unsigned int i = 0 ; i < dm_sidedefs.size() ; i++)
	{
		doom_sidedef_c *SD = dm_sidedefs[i];

		if (SD->sector == src)
			SD->sector = dest;
	}
}


static void DM_ProcessDepots()
{
	// Monster depots need to peer with a real sector so that sound
	// can travel into them and wake up the monsters.  We achieve that
	// by converting a small sector in the depot to BE the peer sector.

	for (unsigned int i = 0 ; i < dm_sectors.size() ; i++)
	{
		doom_sector_c *S = dm_sectors[i];

		if (S->unused)
			continue;

		if (S->special == SEC_DEPOT_PEER)
		{
			S->special = 0;

			doom_sector_c *peer = DM_FindDepotPeer();

			if (peer)
				DM_ConvertSectorToOther(S, peer);
		}
	}
}


//------------------------------------------------------------------------

int doom_vertex_c::Write()
{
	if (index < 0)  // not written yet?
	{
		index = DM_NumVertexes();

		DM_AddVertex(x, y);
	}

	return index;
}


int doom_sector_c::Write()
{
	if (special == SEC_GRAB_NB_FLOOR)
		special = 0;

	if (index < 0)  // not written yet?
	{
		index = DM_NumSectors();

		DM_AddSector(f_h, f_tex.c_str(),
				c_h, c_tex.c_str(),
				light, special, tag);
	}

	return index;
}


int doom_sidedef_c::Write()  
{
	if (index < 0)  // not written yet?
	{
		index = DM_NumSidedefs();

		SYS_ASSERT(sector);

		int sec_index = sector->Write();

		DM_AddSidedef(sec_index, lower.c_str(), mid.c_str(),
				upper.c_str(), x_offset & 1023, y_offset);
	}

	return index;
}


void doom_linedef_c::Write()
{
	SYS_ASSERT(start && end);

	int v1 = start->Write();
	int v2 = end  ->Write();

	int f = front ? front->Write() : -1;
	int b = back  ? back ->Write() : -1;

	DM_AddLinedef(v1, v2, f, b, special, flags, tag, args);
}


static void DM_WriteLinedefs()
{
	// this triggers everything else (Sidedefs, Sectors, Vertices) to be
	// written as well.

	for (int i = 0; i < (int)dm_linedefs.size(); i++)
		if (dm_linedefs[i]->isValid())
			dm_linedefs[i]->Write();
}


static void DM_AddThing_FraggleScript(int x, int y, int z, csg_entity_c *E,
									  int type, int angle, int options)
{
	// this is set in the Lua code (raw_add_entity)
	const char *fs_name = E->props.getStr("fs_name", NULL);

	if (! fs_name)
	{
		LogPrintf("WARNING: entity lost (no fragglescript name for type #%d)\n", type);
		return;
	}

	// merely remember the thing for now, to be processed later
	fs_thing_t thing;

	thing.x = x;
	thing.y = y;
	thing.z = z;
	thing.fs_name = fs_name;
	thing.angle = angle;
	thing.options = options;

	dm_fs_things.push_back(thing);
}


static void DM_WriteThing(doom_sector_c *S, csg_entity_c *E)
{
	// ignore light entities and boxes
	if (strcmp(E->id.c_str(), "light") == 0 ||
		strncmp(E->id.c_str(), "oblige_", 7) == 0)
		return;

	int type = atoi(E->id.c_str());

	if (type <= 0)
	{
		LogPrintf("WARNING: bad doom entity number: '%s'\n",  E->id.c_str());
		return;
	}

	int x = I_ROUND(E->x);
	int y = I_ROUND(E->y);
	int z = I_ROUND(E->z);

	int h = z - S->f_h;

	if (h < 0) h = 0;

	// parse entity properties
	int angle   = E->props.getInt("angle");
	int tid     = E->props.getInt("tid");
	int special = E->props.getInt("special");
	int options = E->props.getInt("flags", MTF_ALL_SKILLS);

	if (dm_sub_format == SUBFMT_Hexen)
	{
		if ((options & MTF_HEXEN_CLASSES) == 0)
			options |= MTF_HEXEN_CLASSES;

		if ((options & MTF_HEXEN_MODES) == 0)
			options |= MTF_HEXEN_MODES;
	}

	byte args[5] = { 0,0,0,0,0 };

	E->props.getHexenArgs(args);

	if (E->ex_floor > 0)
	{
		if (ef_thing_mode == 1)
		{
			// Use FraggleScript to place the thing on a 3D floor
			DM_AddThing_FraggleScript(x, y, z, E, type, angle, options);
			return;
		}
		else if (ef_thing_mode == 2)
		{
			// EDGE extrafloor flags
			options |= (E->ex_floor << MTF_EDGE_EXFLOOR_SHIFT);
		}
	}

	DM_AddThing(x, y, h, type, angle, options, tid, special, args);
}


static void DM_WriteThings()
{
	// iterate through regions so that we know which sector each thing
	// is in, which in turn lets us determine height above the floor.

	for (unsigned int i = 0 ; i < all_regions.size() ; i++)
	{
		region_c *R = all_regions[i];

		if (R->index < 0)
			continue;

		doom_sector_c *S = dm_sectors[R->index];

		for (unsigned int k = 0 ; k < R->entities.size() ; k++)
		{
			DM_WriteThing(S, R->entities[k]);
		}
	}
}


static void DM_WriteFraggleScript()
{
	// TODO: support [level info] stuff

	// WISHLIST: support adding script text from Lua

	if (ef_thing_mode != 1)
		return;
	
	// We create three scripts:
	//
	//   script 1 : spawns everything with easy bit set
	//   script 2 : spawns medium things (easy bit clear)
	//   script 3 : spawns things with only the hard bit set
	//
	// (this precludes several combos, like "only on EASY skill")
	//

	DM_HeaderPrintf("[scripts]\n\n");

	DM_HeaderPrintf("include(\"things.h\");\n\n");

	for (int i = 1 ; i <= 3 ; i++)
	{
		DM_HeaderPrintf("script %d\n{\n", i);

		for (unsigned int k = 0 ; k < dm_fs_things.size() ; k++)
		{
			fs_thing_t& thing = dm_fs_things[k];

			int which;  // which script to use

			if (thing.options & MTF_Easy)
				which = 1;
			else if (thing.options & MTF_Medium)
				which = 2;
			else
				which = 3;

			if (which != i)
				continue;

			DM_HeaderPrintf("  spawn(%s, %d, %d, %d, %d);\n",
							thing.fs_name, thing.x, thing.y, thing.angle, thing.z);
		}

		DM_HeaderPrintf("}\n\n");
	}

	DM_HeaderPrintf("startscript(1);\n");
	DM_HeaderPrintf("if (gameskill() >= 3) startscript(2);\n");
	DM_HeaderPrintf("if (gameskill() >= 4) startscript(3);\n");
}


//------------------------------------------------------------------------

void DM_FreeStuff()
{
	unsigned int i;

	for (i = 0 ; i < dm_vertices.size() ; i++) delete dm_vertices[i];
	for (i = 0 ; i < dm_linedefs.size() ; i++) delete dm_linedefs[i];
	for (i = 0 ; i < dm_sidedefs.size() ; i++) delete dm_sidedefs[i];
	for (i = 0 ; i < dm_sectors .size() ; i++) delete dm_sectors [i];

	for (i = 0 ; i < dm_exfloors.size() ; i++) delete dm_exfloors[i];
	for (i = 0 ; i < dm_dummies .size() ; i++) delete dm_dummies [i];

	dm_vertices.clear();
	dm_linedefs.clear();
	dm_sidedefs.clear();
	dm_sectors. clear();

	dm_exfloors.clear();
	dm_dummies .clear();

	dm_fs_things.clear();

	dm_vertex_map.clear();
}


void CSG_DOOM_Write()
{
	/// Doom_TestRegions();
	/// return;

	LogPrintf("DOOM CSG...\n");

	DM_FreeStuff();

	CSG_BSP(4.0);

	CSG_Shade();

	DM_CreateSectors();
	DM_CoalesceSectors();

	DM_CreateLinedefs();
	DM_MergeColinearLines();

	DM_RoundCorners();
	DM_AlignTextures();

	DM_ProcessSecrets();
	DM_ProcessExtraFloors();
	DM_ProcessLightFX();
	DM_ProcessDepots();

	DM_CreateDummies();

	// this writes vertices, sidedefs and sectors too
	DM_WriteLinedefs();

	DM_WriteThings();
	DM_WriteFraggleScript();

	DM_FreeStuff();
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
