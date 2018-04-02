//------------------------------------------------------------------------
//  TGA (Targa) IMAGE LOADING
//------------------------------------------------------------------------
//
//  Oblige Level Maker
//
//  Copyright (C) 2013-2016 Andrew Apted
//  Copyright (C) 1997-2001 Id Software, Inc.
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
//
//  NOTE: this is based on the TGA loading code from Quake 2.
//
//------------------------------------------------------------------------

#include "headers.h"
#include "main.h"

#include "lib_tga.h"
#include "lib_file.h"

#ifdef HAVE_PHYSFS
#include "m_addons.h"
#endif


tga_image_c::tga_image_c(int W, int H) :
	width(W), height(H),
	opacity(OPAC_UNKNOWN)
{
	pixels = new rgb_color_t[W * H];
}


tga_image_c::~tga_image_c()
{
	delete[] pixels;
	pixels = NULL;
}


typedef struct
{
	u8_t	id_length;
	u8_t	colormap_type;
	u8_t	image_type;
	u16_t	colormap_start;
	u16_t	colormap_length;
	u8_t	colormap_bits;
	u16_t	x_origin;
	u16_t	y_origin;
	u16_t	width;
	u16_t	height;
	u8_t	pixel_bits;
	u8_t	attributes;

} targa_header_t;


typedef enum
{
	TGA_INDEXED = 1,
	TGA_INDEXED_RLE = 9,

	TGA_RGB = 2,
	TGA_RGB_RLE = 10,

	TGA_BW = 3,
	TGA_BW_RLE = 11
}
tga_type_e;


tga_image_c * TGA_LoadImage (const char *path)
{
// load the file

	int length;

#ifdef HAVE_PHYSFS
	byte * buffer = VFS_LoadFile(path, &length);
#else
	byte * buffer = FileLoad(path, &length);
#endif

	if (! buffer)
		return NULL;

	byte * buf_p   = buffer;
///	byte * buf_end = buffer + length;


// decode the TGA header

	targa_header_t	targa_header;

	targa_header.id_length = *buf_p++;
	targa_header.colormap_type = *buf_p++;
	targa_header.image_type = *buf_p++;

	targa_header.colormap_start = (buf_p[0]) | (buf_p[1] << 8); buf_p += 2;
	targa_header.colormap_length = (buf_p[0]) | (buf_p[1] << 8); buf_p += 2;
	targa_header.colormap_bits = *buf_p++;
	targa_header.x_origin = (buf_p[0]) | (buf_p[1] << 8); buf_p += 2;
	targa_header.y_origin = (buf_p[0]) | (buf_p[1] << 8); buf_p += 2;
	targa_header.width = (buf_p[0]) | (buf_p[1] << 8); buf_p += 2;
	targa_header.height = (buf_p[0]) | (buf_p[1] << 8); buf_p += 2;
	targa_header.pixel_bits = *buf_p++;
	targa_header.attributes = *buf_p++;

	if (targa_header.id_length != 0)
		buf_p += targa_header.id_length;  // skip TARGA image comment

	if (targa_header.image_type != TGA_INDEXED &&
	    targa_header.image_type != TGA_INDEXED_RLE &&
	    targa_header.image_type != TGA_RGB &&
		targa_header.image_type != TGA_RGB_RLE)
	{
		Main_FatalError("Bad tga file: type %d is not supported\n", targa_header.image_type);
	}

	int width  = targa_header.width;
	int height = targa_header.height;

	if (width == 0 || height == 0)
		Main_FatalError("Bad tga file: width or height is zero\n");


	tga_image_c * img = new tga_image_c(width, height);

	img->width  = width;
	img->height = height;

	bool is_masked  = false;  // opacity testing
	bool is_complex = false;  //


// decode the palette, if any

	rgb_color_t palette[256];

	if (targa_header.image_type == TGA_INDEXED ||
		targa_header.image_type == TGA_INDEXED_RLE)
	{
		if (targa_header.colormap_type != 1)
			Main_FatalError("Bad tga file: colormap type != 1\n");

		if (targa_header.colormap_length > 256)
			Main_FatalError("Bad tga file: too many colors (over 256)\n");

		if (targa_header.pixel_bits != 8 || targa_header.colormap_bits < 24)
			Main_FatalError("Bad tga file: unsupported colormap size\n");

		memset(palette, 255, sizeof(palette));

		int cm_start = targa_header.colormap_start;
		int cm_end   = cm_start + targa_header.colormap_length;

		for (int n = cm_start ; n < cm_end ; n++)
		{
			byte b = *buf_p++;
			byte g = *buf_p++;
			byte r = *buf_p++;
			byte a = 255;

			if (targa_header.colormap_bits == 32)
				a = *buf_p++;

			palette[n] = MAKE_RGBA(r, g, b, a);
		}
	}


// decode the pixel stream

	rgb_color_t	*dest = img->pixels;
	rgb_color_t	*p;

	if (targa_header.image_type == TGA_RGB)   // Uncompressed, RGB images
	{
		if (targa_header.pixel_bits != 24 && targa_header.pixel_bits != 32)
			Main_FatalError("Bad tga file: only 24 or 32 bit images supported\n");

		for (int y = height-1 ; y >= 0 ; y--)
		{
			p = dest + y * width;

			for (int x = 0 ; x < width ; x++)
			{
				byte b = *buf_p++;
				byte g = *buf_p++;
				byte r = *buf_p++;
				byte a = 255;

				if (targa_header.pixel_bits == 32)
					a = *buf_p++;

				*p++ = MAKE_RGBA(r, g, b, a);

				if (a == 0)
					is_masked = true;
				else if (a != 255)
					is_complex = true;
			}
		}
	}
	else if (targa_header.image_type == TGA_RGB_RLE)   // Runlength encoded RGB images
	{
		if (targa_header.pixel_bits != 24 && targa_header.pixel_bits != 32)
			Main_FatalError("Bad tga file: only 24 or 32 bit images supported\n");

		byte r=0, g=0, b=0, a=0;

		byte packet_header, packet_size;

		for (int y = height-1 ; y >= 0 ; y--)
		{
			p = dest + y * width;

			for (int x = 0 ; x < width ; )
			{
				packet_header = *buf_p++;
				packet_size = 1 + (packet_header & 0x7f);

				if (packet_header & 0x80)    // run-length packet
				{
					b = *buf_p++;
					g = *buf_p++;
					r = *buf_p++;
					a = 255;

					if (targa_header.pixel_bits == 32)
						a = *buf_p++;

					if (a == 0)
						is_masked = true;
					else if (a != 255)
						is_complex = true;

					for (int j = 0 ; j < packet_size ; j++)
					{
						*p++ = MAKE_RGBA(r, g, b, a);

						x++;

						if (x == width)  // run spans across edge
						{
							x = 0;
							if (y > 0)
								y--;
							else
								goto breakOut;

							p = dest + y*width;
						}
					}
				}
				else        // not a run-length packet
				{
					for (int j = 0 ; j < packet_size; j++)
					{
						b = *buf_p++;
						g = *buf_p++;
						r = *buf_p++;
						a = 255;

						if (targa_header.pixel_bits == 32)
							a = *buf_p++;

						*p++ = MAKE_RGBA(r, g, b, a);

						if (a == 0)
							is_masked = true;
						else if (a != 255)
							is_complex = true;

						x++;

						if (x == width)  // pixel packet run spans across edge
						{
							x = 0;
							if (y > 0)
								y--;
							else
								goto breakOut;

							p = dest + y * width;
						}
					}
				}
			}
			breakOut: ;
		}
	}
	else if (targa_header.image_type == TGA_INDEXED)   // Uncompressed, colormapped images
	{
		for (int y = height-1 ; y >= 0 ; y--)
		{
			p = dest + y * width;

			for (int x = 0 ; x < width ; x++)
			{
				rgb_color_t col = palette[*buf_p++];

				*p++ = col;

				byte a = RGB_ALPHA(col);

				if (a == 0)
					is_masked = true;
				else if (a != 255)
					is_complex = true;
			}
		}
	}
	else if (targa_header.image_type == TGA_INDEXED_RLE)   // Runlength encoded colormapped image
	{
		byte packet_header, packet_size;

		for (int y = height-1 ; y >= 0 ; y--)
		{
			p = dest + y * width;

			for (int x = 0 ; x < width ; )
			{
				packet_header = *buf_p++;
				packet_size = 1 + (packet_header & 0x7f);

				if (packet_header & 0x80)    // run-length packet
				{
					rgb_color_t col = palette[*buf_p++];

					byte a = RGB_ALPHA(col);

					if (a == 0)
						is_masked = true;
					else if (a != 255)
						is_complex = true;

					for (int j = 0 ; j < packet_size ; j++)
					{
						*p++ = col;

						x++;

						if (x == width)  // run spans across edge
						{
							x = 0;
							if (y > 0)
								y--;
							else
								goto breakOut2;

							p = dest + y*width;
						}
					}
				}
				else        // not a run-length packet
				{
					for (int j = 0 ; j < packet_size; j++)
					{
						rgb_color_t col = palette[*buf_p++];

						*p++ = col;

						byte a = RGB_ALPHA(col);

						if (a == 0)
							is_masked = true;
						else if (a != 255)
							is_complex = true;

						x++;

						if (x == width)  // pixel packet run spans across edge
						{
							x = 0;
							if (y > 0)
								y--;
							else
								goto breakOut2;

							p = dest + y * width;
						}
					}
				}
			}
			breakOut2: ;
		}
	}

#ifdef HAVE_PHYSFS
	VFS_FreeFile(buffer);
#else
	FileFree(buffer);
#endif

	img->opacity =	is_complex ? OPAC_Complex :
					is_masked  ? OPAC_Masked  : OPAC_Solid;

	return img;
}


//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
