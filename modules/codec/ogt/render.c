/*****************************************************************************
 * render.c : Philips OGT and CVD (VCD Subtitle) blending routines
 *****************************************************************************
 * Copyright (C) 2003, 2004 VideoLAN
 * $Id: render.c,v 1.17 2004/01/21 04:45:47 rocky Exp $
 *
 * Author: Rocky Bernstein 
 *   based on code from: 
 *          Sam Hocevar <sam@zoy.org>
 *          Rudolf Cornelissen <rag.cornelissen@inter.nl.net>
 *          Roine Gustafsson <roine@popstar.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <vlc/vlc.h>
#include <vlc/vout.h>
#include <vlc/decoder.h>

#include "pixmap.h"
#include "subtitle.h"
#include "render.h"

/* We use 4 bits for an alpha value: 0..15, 15 is completely transparent and
   0 completely opaque. Note that although SVCD allow 8-bits, pixels 
   previously should be scaled down to 4 bits to use these routines.
*/
#define ALPHA_BITS (4)
#define MAX_ALPHA  ((1<<ALPHA_BITS) - 1) 
#define ALPHA_SCALEDOWN (8-ALPHA_BITS)

/* We use a somewhat artifical factor in scaling calculations so
   that we can use integer arithmetic and still get somewhat precise
   results. 
*/
#define ASCALE 6

/* Horrible hack to get dbg_print to do the right thing */
#define p_dec p_vout

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void BlendI420( vout_thread_t *, picture_t *, const subpicture_t *,
                        vlc_bool_t );
static void BlendYUY2( vout_thread_t *p_vout, picture_t *p_pic,
                        const subpicture_t *p_spu, vlc_bool_t b_crop );
static void BlendRV16( vout_thread_t *p_vout, picture_t *p_pic,
                        const subpicture_t *p_spu, vlc_bool_t b_crop );
static void BlendRV32( vout_thread_t *p_vout, picture_t *p_pic,
                        const subpicture_t *p_spu, vlc_bool_t b_crop );
static void BlendRGB2( vout_thread_t *p_vout, picture_t *p_pic,
                        const subpicture_t *p_spu, vlc_bool_t b_crop );

/*****************************************************************************
 * BlendSPU: blend a subtitle into a picture
 *****************************************************************************
 
  This blends subtitles (a subpicture) into the underlying
  picture. Subtitle data has been preprocessed as YUV + transparancy
  or 4 bytes per pixel with interleaving of rows in the subtitle
  removed. 

 *****************************************************************************/
void VCDSubBlend( vout_thread_t *p_vout, picture_t *p_pic,
		   const subpicture_t *p_spu )
{
    struct subpicture_sys_t *p_sys = p_spu->p_sys;
  
    dbg_print( (DECODE_DBG_CALL|DECODE_DBG_RENDER), 
	       "chroma %x", p_vout->output.i_chroma );

    switch( p_vout->output.i_chroma )
    {
        /* I420 target, no scaling */
        case VLC_FOURCC('I','4','2','0'):
        case VLC_FOURCC('I','Y','U','V'):
        case VLC_FOURCC('Y','V','1','2'):
            BlendI420( p_vout, p_pic, p_spu, p_spu->p_sys->b_crop );
            break;

        /* RGB 555 - scaled */
        case VLC_FOURCC('R','V','1','6'):
            BlendRV16( p_vout, p_pic, p_spu, p_spu->p_sys->b_crop );
	    break;

        /* RV32 target, scaling */
        case VLC_FOURCC('R','V','2','4'):
        case VLC_FOURCC('R','V','3','2'):
            BlendRV32( p_vout, p_pic, p_spu, p_spu->p_sys->b_crop );
	    break;

        /* NVidia overlay, no scaling */
        case VLC_FOURCC('Y','U','Y','2'):
            BlendYUY2( p_vout, p_pic, p_spu, p_spu->p_sys->b_crop );
	    break;

        /* Palettized 8 bits per pixel (256 colors). Each
           pixel is an uint8_t index in the palette
           Used in ASCII Art. 
        */
        case VLC_FOURCC('R','G','B','2'):
            BlendRGB2( p_vout, p_pic, p_spu, p_spu->p_sys->b_crop );
          
          /*msg_Err( p_vout, "RGB2 not implemented yet" );*/
	    break;

        default:
            msg_Err( p_vout, "unknown chroma, can't render SPU" );
            break;
    }
}

/* Following functions are local */

/* 
  YV12 format:  

  All Y samples are found first in memory as an array of bytes
  (possibly with a larger stride for memory alignment), followed
  immediately by all Cr (=U) samples (with half the stride of the Y
  lines, and half the number of lines), then followed immediately by
  all Cb (=V) samples in a similar fashion.
*/

static void BlendI420( vout_thread_t *p_vout, picture_t *p_pic,
                        const subpicture_t *p_spu, vlc_bool_t b_crop )
{
  /* Common variables */
  uint8_t *p_pixel_base_Y, *p_pixel_base_V, *p_pixel_base_U;
  ogt_yuvt_t *p_source;

  int i_x, i_y;
  vlc_bool_t even_scanline = VLC_FALSE;

  /* Crop-specific */
  int i_x_start, i_y_start, i_x_end, i_y_end;
  /* int i=0; */

  const struct subpicture_sys_t *p_sys = p_spu->p_sys;
  
  dbg_print( (DECODE_DBG_CALL|DECODE_DBG_RENDER), 
	     "spu width x height: (%dx%d), (x,y)=(%d,%d), yuv pitch (%d,%d,%d)", 
	     p_spu->i_width,  p_spu->i_height, p_spu->i_x, p_spu->i_y,
	     p_pic->Y_PITCH, p_pic->U_PITCH, p_pic->V_PITCH );

  
  p_pixel_base_Y = p_pic->p[Y_PLANE].p_pixels + p_spu->i_x
                 + p_pic->p[Y_PLANE].i_pitch * p_spu->i_y;
  
  p_pixel_base_U = p_pic->p[U_PLANE].p_pixels + p_spu->i_x/2
                 + p_pic->p[U_PLANE].i_pitch * p_spu->i_y/2;
  
  p_pixel_base_V = p_pic->p[V_PLANE].p_pixels + p_spu->i_x/2
                 + p_pic->p[V_PLANE].i_pitch * p_spu->i_y/2;
  
  i_x_start = p_sys->i_x_start;
  i_y_start = p_pic->p[Y_PLANE].i_pitch * p_sys->i_y_start;
  
  i_x_end   = p_sys->i_x_end;
  i_y_end   = p_pic->p[Y_PLANE].i_pitch * p_sys->i_y_end;
  
  p_source = (ogt_yuvt_t *)p_sys->p_data;
  
  /* Draw until we reach the bottom of the subtitle */
  for( i_y = 0; 
       i_y < p_spu->i_height * p_pic->p[Y_PLANE].i_pitch ;
       i_y += p_pic->p[Y_PLANE].i_pitch )
    {
      uint8_t *p_pixel_base_Y_y = p_pixel_base_Y + i_y;
      uint8_t *p_pixel_base_U_y = p_pixel_base_U + i_y/4;
      uint8_t *p_pixel_base_V_y = p_pixel_base_V + i_y/4;

      i_x = 0;

      if ( b_crop ) {
        if ( i_y > i_y_end ) break;
        if (i_x_start) {
          i_x = i_x_start;
          p_source += i_x_start;
        }
      }

      even_scanline = !even_scanline;

      /* Draw until we reach the end of the line */
      for( ; i_x < p_spu->i_width; i_x++, p_source++ )
	{

	  if( b_crop ) {

            /* FIXME: y cropping should be dealt with outside of this loop.*/
            if ( i_y < i_y_start) continue;

            if ( i_x > i_x_end )
	    {
	      p_source += p_spu->i_width - i_x;
              break;
	    }
          }
	  
	  switch( p_source->s.t )
	    {
	    case 0x00: 
	      /* Completely transparent. Don't change pixel. */
	      break;
	      
	    case MAX_ALPHA:
	      {
		/* Completely opaque. Completely overwrite underlying
		   pixel with subtitle pixel. */
		
		/* This is the location that's going to get changed.*/
		uint8_t *p_pixel_Y = p_pixel_base_Y_y + i_x;
		
		*p_pixel_Y = p_source->plane[Y_PLANE];

		if ( even_scanline && i_x % 2 == 0 ) {
		  uint8_t *p_pixel_U = p_pixel_base_U_y + i_x/2;
		  uint8_t *p_pixel_V = p_pixel_base_V_y + i_x/2;
		  *p_pixel_U = p_source->plane[U_PLANE];
		  *p_pixel_V = p_source->plane[V_PLANE];
		}
		
		break;
	      }
	      
	    default:
	      {
		/* Blend in underlying subtitle pixel. */
		
		/* This is the location that's going to get changed.*/
		uint8_t *p_pixel_Y = p_pixel_base_Y_y + i_x;


		/* This is the weighted part of the subtitle. The
		   color plane is 8 bits and transparancy is 4 bits so
		   when multiplied we get up to 12 bits.
		 */
		uint16_t i_sub_color_Y = 
		  (uint16_t) ( p_source->plane[Y_PLANE] *
			       (uint16_t) (p_source->s.t) );

		/* This is the weighted part of the underlying pixel.
		   For the same reasons above, the result is up to 12
		   bits.  However since the transparancies are
		   inverses, the sum of i_sub_color and i_pixel_color
		   will not exceed 12 bits.
		*/
		uint16_t i_pixel_color_Y = 
		  (uint16_t) ( *p_pixel_Y * 
			       (uint16_t) (MAX_ALPHA - p_source->s.t) ) ;
		
		/* Scale the 12-bit result back down to 8 bits. A
		   precise scaling after adding the two components,
		   would divide by one less than a power of 2. However
		   to simplify and speed things we use a power of
		   2. This means the boundaries (either all
		   transparent and all opaque) aren't handled properly.
		   But we deal with them in special cases above. */

		*p_pixel_Y = ( i_sub_color_Y + i_pixel_color_Y ) >> 4;

		if ( even_scanline && i_x % 2 == 0 ) {
		  uint8_t *p_pixel_U = p_pixel_base_U_y + i_x/2;
		  uint8_t *p_pixel_V = p_pixel_base_V_y + i_x/2;
		  uint16_t i_sub_color_U = 
		    (uint16_t) ( p_source->plane[U_PLANE] *
				 (uint16_t) (p_source->s.t) );
		  
		  uint16_t i_sub_color_V = 
		    (uint16_t) ( p_source->plane[V_PLANE] *
				 (uint16_t) (p_source->s.t) );
		  uint16_t i_pixel_color_U = 
		    (uint16_t) ( *p_pixel_U * 
				 (uint16_t) (MAX_ALPHA - p_source->s.t) ) ;
		  uint16_t i_pixel_color_V = 
		    (uint16_t) ( *p_pixel_V * 
				 (uint16_t) (MAX_ALPHA - p_source->s.t) ) ;
		  *p_pixel_U = ( i_sub_color_U + i_pixel_color_U ) >> 4;
		  *p_pixel_V = ( i_sub_color_V + i_pixel_color_V ) >> 4;
		}
		break;
	      }
	      
	    }
	}
    }
}

/*

  YUY2 Format:

  Data is found in memory as an array of bytes in which the first byte
  contains the first sample of Y, the second byte contains the first
  sample of Cb (=U), the third byte contains the second sample of Y,
  the fourth byte contains the first sample of Cr (=V); and so
  on. Each 32-bit word then contains information for two contiguous
  horizontal pixels, two 8-bit Y values plus a single Cb and Cr which
  spans the two pixels.
*/

/* 
   Should be same as p_pic->p_format.i_bits_per_pixel / 8. But since
   we know it here, why try to compute it?
*/
#define BYTES_PER_PIXEL 4

static void BlendYUY2( vout_thread_t *p_vout, picture_t *p_pic,
                        const subpicture_t *p_spu, vlc_bool_t b_crop )
{
  /* Common variables */
  uint8_t *p_pixel_base;
  ogt_yuvt_t *p_source = (ogt_yuvt_t *) p_spu->p_sys->p_data;;

#ifdef EXTRA_CHECKING
  ogt_yuvt_t *p_source_end = (ogt_yuvt_t *)p_spu->p_sys->p_data + 
    (p_spu->i_width * p_spu->i_height);
#endif

  uint16_t i_x, i_y;

  /* Make sure we start on a word (4-byte) boundary. */
  uint16_t i_spu_x = (p_spu->i_x & 0xFFFE) * 2;

  /* Crop-specific */
  int i_x_start, i_y_start, i_x_end, i_y_end;

  const struct subpicture_sys_t *p_sys = p_spu->p_sys;
  
  dbg_print( (DECODE_DBG_CALL|DECODE_DBG_RENDER), 
	     "spu width x height: (%dx%d), (x,y)=(%d,%d), pitch: %d", 
	     p_spu->i_width,  p_spu->i_height, p_spu->i_x, p_spu->i_y,
	     p_pic->p->i_pitch );

  
  p_pixel_base = p_pic->p->p_pixels + 
    + ( p_spu->i_y * p_pic->p->i_pitch ) + i_spu_x;

  i_x_start = p_sys->i_x_start;
  i_y_start = p_sys->i_y_start * p_pic->p->i_pitch;
  
  i_x_end   = p_sys->i_x_end;
  i_y_end   = p_sys->i_y_end   * p_pic->p->i_pitch;

  /* Draw until we reach the bottom of the subtitle */
  for( i_y = 0; 
       i_y < p_spu->i_height * p_pic->p[Y_PLANE].i_pitch ;
       i_y += p_pic->p[Y_PLANE].i_pitch )
    {
      uint8_t *p_pixel_base_y = p_pixel_base + i_y;

      i_x = 0;

      if ( b_crop ) {
        if ( i_y > i_y_end ) break;
        if (i_x_start) {
          i_x = i_x_start;
          p_source += (i_x_start*2);
        }
      }
  

      /* Draw until we reach the end of the line. Each output pixel
         is a combination of two source pixels. 
       */
      for( i_x = 0;  i_x < p_spu->i_width / 2; i_x++, p_source +=2 )
	{
          uint16_t i_avg_tr; /* transparancy sort of averaged over 2 pixels*/

#if EXTRA_CHECKING
          if (p_source > p_source_end-1) {
            msg_Err( p_vout, "Trying to access beyond subtitle x: %d y: %d",
                     i_x, i_y);
            return;
          }
#endif

	  if( b_crop ) {

            /* FIXME: y cropping should be dealt with outside of this loop.*/
            if ( i_y < i_y_start) continue;

            if ( i_x > i_x_end )
	    {
	      p_source += p_spu->i_width - (i_x*2);
              break;
	    }
          }
  
	  
          /* Favor opaque subtitle pixels. */
	  if ( (p_source->s.t == 0) && (p_source+1)->s.t == MAX_ALPHA )
            i_avg_tr = (p_source+1)->s.t;
          else if ( (p_source->s.t == MAX_ALPHA) && (p_source+1)->s.t == 0 )
            i_avg_tr = p_source->s.t;
          else 
            i_avg_tr = ( p_source->s.t + (p_source+1)->s.t ) / 2;
          

	  switch( i_avg_tr )
	    {
	    case 0x00: 
	      /* Completely transparent. Don't change pixel. */
	      break;
	      
	    case MAX_ALPHA:
	      {
		/* Completely opaque. Completely overwrite underlying
		   pixel with subtitle pixel. */
		
		/* This is the location that's going to get changed.*/
		uint8_t *p_pixel = p_pixel_base_y + i_x * BYTES_PER_PIXEL;
		uint8_t i_avg_u;
		uint8_t i_avg_v;

                /* Favor opaque subtitle pixel. */
                if (p_source->s.t == MAX_ALPHA ) {
                  i_avg_u = p_source->plane[U_PLANE] ;
                  i_avg_v = p_source->plane[V_PLANE] ;
                } else if ( (p_source+1)->s.t == MAX_ALPHA ) {
                  i_avg_u = (p_source+1)->plane[U_PLANE] ;
                  i_avg_v = (p_source+1)->plane[V_PLANE] ;
                } else {
                  i_avg_u = ( p_source->plane[U_PLANE] 
                              + (p_source+1)->plane[U_PLANE] ) / 2;
                  i_avg_v = ( p_source->plane[V_PLANE] 
                              + (p_source+1)->plane[V_PLANE] ) / 2;
                }

		/* draw a two contiguous pixels: 2 Y values, 1 U, and 1 V. */
		*p_pixel++ = p_source->plane[Y_PLANE] ;
                *p_pixel++ = i_avg_u;
		*p_pixel++ = (p_source+1)->plane[Y_PLANE] ;
                *p_pixel++ = i_avg_v;
                
		break;
	      }

	    default:
	      {
		/* Blend in underlying subtitle pixels. */
		
		/* This is the location that's going to get changed.*/
		uint8_t *p_pixel = p_pixel_base_y + i_x * BYTES_PER_PIXEL;
		uint8_t i_avg_u = ( p_source->plane[U_PLANE] 
                                    + (p_source+1)->plane[U_PLANE] ) / 2;
		uint8_t i_avg_v = ( p_source->plane[V_PLANE] 
                                    + (p_source+1)->plane[V_PLANE] ) / 2;

		/* This is the weighted part of the two subtitle
		   pixels. The color plane is 8 bits and transparancy
		   is 4 bits so when multiplied we get up to 12 bits.
		 */
		uint16_t i_sub_color_Y1 = 
		  (uint16_t) ( p_source->plane[Y_PLANE] *
			       (uint16_t) (p_source->s.t) );

		uint16_t i_sub_color_Y2 = 
		  (uint16_t) ( (p_source+1)->plane[Y_PLANE] *
			       (uint16_t) ((p_source+1)->s.t) );

		/* This is the weighted part of the underlying pixels.
		   For the same reasons above, the result is up to 12
		   bits.  However since the transparancies are
		   inverses, the sum of i_sub_color and i_pixel_color
		   will not exceed 12 bits.
		*/
		uint16_t i_sub_color_U = 
		  (uint16_t) ( i_avg_u * (uint16_t) i_avg_tr );
		
		uint16_t i_sub_color_V = 
		  (uint16_t) ( i_avg_v * (uint16_t) i_avg_tr );

		uint16_t i_pixel_color_Y1 = 
		  (uint16_t) ( *(p_pixel) * 
			       (uint16_t) (MAX_ALPHA - p_source->s.t) ) ;
		uint16_t i_pixel_color_Y2 = 
		  (uint16_t) ( *(p_pixel) * 
			       (uint16_t) (MAX_ALPHA - (p_source+1)->s.t) ) ;
		uint16_t i_pixel_color_U = 
		  (uint16_t) ( *(p_pixel+1) * 
			       (uint16_t) (MAX_ALPHA - i_avg_tr) ) ;
		uint16_t i_pixel_color_V = 
		  (uint16_t) ( *(p_pixel+3) * 
			       (uint16_t) (MAX_ALPHA - i_avg_tr) ) ;

		/* draw a two contiguous pixels: 2 Y values, 1 U, and 1 V. */

		/* Scale the 12-bit result back down to 8 bits. A
		   precise scaling after adding the two components,
		   would divide by one less than a power of 2. However
		   to simplify and speed things we use a power of
		   2. This means the boundaries (either all
		   transparent and all opaque) aren't handled properly.
		   But we deal with them in special cases above. */

		*p_pixel++ = ( i_sub_color_Y1 + i_pixel_color_Y1 ) >> 4;
		*p_pixel++ = ( i_sub_color_V + i_pixel_color_V ) >> 4;
		*p_pixel++ = ( i_sub_color_Y2 + i_pixel_color_Y2 ) >> 4;
		*p_pixel++ = ( i_sub_color_U + i_pixel_color_U ) >> 4;
		break;
	      }
	    }
	}

      /* For an odd width source, we'll just have to drop off a pixel. */
      if (p_spu->i_width % 2) p_source++;
    }
}

/**
   Convert a YUV pixel into a 16-bit RGB 5-5-5 pixel.

   A RGB 5-5-5 pixel looks like this:
   RGB 5-5-5   bit  (MSB) 7  6   5  4  3  2  1  0 (LSB)
                 p 	  ? B4 	B3 B2 B1 B0 R4 	R3
                 q 	 R2 R1  R0 G4 G3 G2 G1 	G0

**/

static inline void
yuv2rgb555(ogt_yuvt_t *p_yuv, uint8_t *p_rgb1, uint8_t *p_rgb2 )
{

  uint8_t rgb[3];

#define RED_PIXEL   0
#define GREEN_PIXEL 1
#define BLUE_PIXEL  2

  yuv2rgb(p_yuv, rgb);
  
  /* Scale RGB from 8 bits down to 5. */
  rgb[RED_PIXEL]   >>= (8-5);
  rgb[GREEN_PIXEL] >>= (8-5);
  rgb[BLUE_PIXEL]  >>= (8-5);
  
  *p_rgb1 = ( (rgb[BLUE_PIXEL] << 2)&0x7c ) | ( (rgb[RED_PIXEL]>>3) & 0x03 );
  *p_rgb2 = ( (rgb[RED_PIXEL]  << 5)&0xe0 ) | ( rgb[GREEN_PIXEL]&0x1f );

#if 0
  printf("Y,Cb,Cr,T=(%02x,%02x,%02x,%02x), r,g,b=(%d,%d,%d), "
         "rgb1: %02x, rgb2 %02x\n",
         p_yuv->s.y, p_yuv->s.u, p_yuv->s.v, p_yuv->s.t,
         rgb[RED_PIXEL], rgb[GREEN_PIXEL], rgb[BLUE_PIXEL],
         *p_rgb1, *p_rgb2);
#endif

#undef RED_PIXEL   
#undef GREEN_PIXEL 
#undef BLUE_PIXEL  
}

/* 
   Should be same as p_pic->p_format.i_bits_per_pixel / 8. But since
   we know it here, why try to compute it?
*/
#undef BYTES_PER_PIXEL
#define BYTES_PER_PIXEL 2

static void 
BlendRV16( vout_thread_t *p_vout, picture_t *p_pic,
            const subpicture_t *p_spu, vlc_bool_t b_crop )
{
    /* Common variables */
    uint8_t *p_pixel_base;
    ogt_yuvt_t *p_src_start = (ogt_yuvt_t *)p_spu->p_sys->p_data;
    ogt_yuvt_t *p_src_end   = &p_src_start[p_spu->i_height * p_spu->i_width];
    ogt_yuvt_t *p_source;

    int i_x, i_y;
    int i_y_src;

    /* RGB-specific */
    int i_xscale, i_yscale, i_width, i_height, i_ytmp, i_ynext;

    /* Crop-specific */
    int i_x_start, i_y_start, i_x_end, i_y_end;

    struct subpicture_sys_t *p_sys = p_spu->p_sys;

    i_xscale = ( p_vout->output.i_width << ASCALE ) / p_vout->render.i_width;
    i_yscale = ( p_vout->output.i_height << ASCALE ) / p_vout->render.i_height;

    dbg_print( (DECODE_DBG_CALL|DECODE_DBG_RENDER), 
	       "spu: %dx%d, scaled: %dx%d, vout render: %dx%d, scale %dx%d", 
	       p_spu->i_width,  p_spu->i_height, 
	       p_vout->output.i_width, p_vout->output.i_height,
	       p_vout->render.i_width, p_vout->render.i_height,
	       i_xscale, i_yscale
	       );

    i_width  = p_spu->i_width  * i_xscale;
    i_height = p_spu->i_height * i_yscale;

    /* Set where we will start blending subtitle from using
       the picture coordinates subtitle offsets
    */
    p_pixel_base = p_pic->p->p_pixels 
              + ( (p_spu->i_x * i_xscale) >> ASCALE ) * BYTES_PER_PIXEL
              + ( (p_spu->i_y * i_yscale) >> ASCALE ) * p_pic->p->i_pitch;

    i_x_start = p_sys->i_x_start;
    i_y_start = i_yscale * p_sys->i_y_start;
    i_x_end   = p_sys->i_x_end;
    i_y_end   = i_yscale * p_sys->i_y_end;

    p_source = (ogt_yuvt_t *)p_sys->p_data;
  
    /* Draw until we reach the bottom of the subtitle */
    i_y = 0;
    for( i_y_src = 0 ; i_y_src < p_spu->i_height * p_spu->i_width; 
         i_y_src += p_spu->i_width )
    {
	uint8_t *p_pixel_base_y;
        i_ytmp = i_y >> ASCALE;
        i_y += i_yscale;
	p_pixel_base_y = p_pixel_base + (i_ytmp * p_pic->p->i_pitch);
	i_x = 0;

        if ( b_crop ) {
          if ( i_y > i_y_end ) break;
          if (i_x_start) {
            i_x = i_x_start;
            p_source += i_x_start;
          }
        }

        /* Check whether we need to draw one line or more than one */
        if( i_ytmp + 1 >= ( i_y >> ASCALE ) )
        {
          /* Draw until we reach the end of the line */
          for( ; i_x < p_spu->i_width;  i_x++, p_source++ )
            {

#if 0              
              uint8_t *p=(uint8_t *) p_source;
              printf("+++ %02x %02x %02x %02x\n", 
                     p[0], p[1], p[2], p[3]);
#endif
    
              if( b_crop ) {
                
                /* FIXME: y cropping should be dealt with outside of this 
                   loop.*/
                if ( i_y < i_y_start) continue;
                
                if ( i_x > i_x_end )
                  {
                    p_source += p_spu->i_width - i_x;
                    break;
                  }
              }

	      if (p_source >= p_src_end) {
		msg_Err( p_vout, "Trying to access beyond subtitle %dx%d %d",
			 i_x, i_y / i_yscale, i_height);
		return;
	      }
	      
	      switch( p_source->s.t )
                {
                case 0x00:
		  /* Completely transparent. Don't change pixel. */
		  break;
		  
                default:
                case MAX_ALPHA:
		  {
		    /* Completely opaque. Completely overwrite underlying
		       pixel with subtitle pixel. */
		
		    /* This is the location that's going to get changed.
		     */
		    uint8_t *p_dest = p_pixel_base_y + i_x * BYTES_PER_PIXEL;
                    uint8_t i_rgb1;
                    uint8_t i_rgb2;
                    yuv2rgb555(p_source, &i_rgb1, &i_rgb2);
                    *p_dest++ = i_rgb1;
                    *p_dest++ = i_rgb2;
		    break;
		  }

#ifdef TRANSPARENCY_FINISHED
                default:
		  {
		    /* Blend in underlying pixel subtitle pixel. */
		    
		    /* To be able to scale correctly for full opaqueness, we
		       add 1 to the alpha.  This means alpha value 0 won't
		       be completely transparent and is not correct, but
		       that's handled in a special case above anyway. */
		
		    uint8_t *p_dest = p_pixel_base_y + i_x * BYTES_PER_PIXEL;
		    uint8_t i_destalpha = MAX_ALPHA - p_source->s.t;
                    uint8_t rgb[3];

                    yuv2rgb(p_source, rgb);
                    rv16_pack_blend(p_dest, rgb, dest_alpha, ALPHA_SCALEDOWN);
		    break;
		  }
#endif /*TRANSPARENCY_FINISHED*/
                }
            }
        }
        else
        {
            i_ynext = p_pic->p->i_pitch * i_y >> ASCALE;


            /* Draw until we reach the end of the line */
            for( ; i_x < p_spu->i_width; i_x++, p_source++ )
            {

              if( b_crop ) {
                
                /* FIXME: y cropping should be dealt with outside of this 
                   loop.*/
                if ( i_y < i_y_start) continue;
                
                if ( i_x > i_x_end )
                  {
                    p_source += p_spu->i_width - i_x;
                    break;
                  }
              }
	      
	      if (p_source >= p_src_end) {
		msg_Err( p_vout, "Trying to access beyond subtitle %dx%d %d",
			 i_x, i_y / i_yscale, i_height);
		return;
	      }
	      
	      switch( p_source->s.t )
                {
                case 0x00:
		    /* Completely transparent. Don't change pixel. */
                    break;

                default:
                case MAX_ALPHA: 
		  {
		    /* Completely opaque. Completely overwrite underlying
		       pixel with subtitle pixel. */

		    /* This is the location that's going to get changed.
		     */
		    uint8_t *p_pixel_base_x = p_pixel_base 
                                            + i_x * BYTES_PER_PIXEL;

                    for(  ; i_ytmp < i_ynext ; i_ytmp += p_pic->p->i_pitch )
                    {
		      /* This is the location that's going to get changed.  */
		      uint8_t *p_dest = p_pixel_base_x + i_ytmp;
                      uint8_t i_rgb1;
                      uint8_t i_rgb2;
                      yuv2rgb555(p_source, &i_rgb1, &i_rgb2);
                      *p_dest++ = i_rgb1;
                      *p_dest++ = i_rgb2;
                    }
                    break;
		  }
#ifdef TRANSPARENCY_FINISHED
                default:
                    for(  ; i_ytmp < i_ynext ; y_ytmp += p_pic->p->i_pitch )
                    {
		      /* Blend in underlying pixel subtitle pixel. */
		      
		      /* To be able to scale correctly for full opaqueness, we
			 add 1 to the alpha.  This means alpha value 0 won't
			 be completely transparent and is not correct, but
			 that's handled in a special case above anyway. */
		      
		      uint8_t *p_dest = p_pixel_base + i_ytmp;
                      uint8_t i_destalpha = MAX_ALPHA - p_source->s.t;
                      uint8_t rgb[3];

                      yuv2rgb(p_source, rgb);
                      rv16_pack_blend(p_dest, rgb, dest_alpha,ALPHA_SCALEDOWN);
                    }
                    break;
#endif /*TRANSPARENCY_FINISHED*/
		}
	    }
	}
    }
}

/* 
   Should be Same as p_pic->p_format.i_bits_per_pixel / 8. But since
   we know it here, why try to compute it?
*/
#undef  BYTES_PER_PIXEL
#define BYTES_PER_PIXEL 4

static void 
BlendRV32( vout_thread_t *p_vout, picture_t *p_pic,
            const subpicture_t *p_spu, vlc_bool_t b_crop )
{
    /* Common variables */
    uint8_t *p_pixel_base;
    ogt_yuvt_t *p_src_start = (ogt_yuvt_t *)p_spu->p_sys->p_data;
    ogt_yuvt_t *p_src_end   = &p_src_start[p_spu->i_height * p_spu->i_width];
    ogt_yuvt_t *p_source;

    int i_x, i_y;
    int i_y_src;

    /* RGB-specific */
    int i_xscale, i_yscale, i_width, i_height, i_ytmp, i_ynext;

    /* Crop-specific */
    int i_x_start, i_y_start, i_x_end, i_y_end;

    struct subpicture_sys_t *p_sys = p_spu->p_sys;

    i_xscale = ( p_vout->output.i_width << ASCALE ) / p_vout->render.i_width;
    i_yscale = ( p_vout->output.i_height << ASCALE ) / p_vout->render.i_height;

    dbg_print( (DECODE_DBG_CALL|DECODE_DBG_RENDER), 
	       "spu: %dx%d, scaled: %dx%d, vout render: %dx%d, scale %dx%d", 
	       p_spu->i_width,  p_spu->i_height, 
	       p_vout->output.i_width, p_vout->output.i_height,
	       p_vout->render.i_width, p_vout->render.i_height,
	       i_xscale, i_yscale
	       );

    i_width  = p_spu->i_width  * i_xscale;
    i_height = p_spu->i_height * i_yscale;

    /* Set where we will start blending subtitle from using
       the picture coordinates subtitle offsets
    */
    p_pixel_base = p_pic->p->p_pixels 
              + ( (p_spu->i_x * i_xscale) >> ASCALE ) * BYTES_PER_PIXEL
              + ( (p_spu->i_y * i_yscale) >> ASCALE ) * p_pic->p->i_pitch;

    i_x_start = p_sys->i_x_start;
    i_y_start = i_yscale * p_sys->i_y_start;
    i_x_end   = p_sys->i_x_end;
    i_y_end   = i_yscale * p_sys->i_y_end;

    p_source = (ogt_yuvt_t *)p_sys->p_data;
  
    /* Draw until we reach the bottom of the subtitle */
    i_y = 0;
    for( i_y_src = 0 ; i_y_src < p_spu->i_height * p_spu->i_width; 
         i_y_src += p_spu->i_width )
    {
	uint8_t *p_pixel_base_y;
        i_ytmp = i_y >> ASCALE;
        i_y += i_yscale;
	p_pixel_base_y = p_pixel_base + (i_ytmp * p_pic->p->i_pitch);
	i_x = 0;

        if ( b_crop ) {
          if ( i_y > i_y_end ) break;
          if (i_x_start) {
            i_x = i_x_start;
            p_source += i_x_start;
          }
        }

        /* Check whether we need to draw one line or more than one */
        if( i_ytmp + 1 >= ( i_y >> ASCALE ) )
        {
          /* Draw until we reach the end of the line */
          for( ; i_x < p_spu->i_width;  i_x++, p_source++ )
            {

#if 0              
              uint8_t *p=(uint8_t *) p_source;
              printf("+++ %02x %02x %02x %02x\n", 
                     p[0], p[1], p[2], p[3]);
#endif
    
              if( b_crop ) {
                
                /* FIXME: y cropping should be dealt with outside of this 
                   loop.*/
                if ( i_y < i_y_start) continue;
                
                if ( i_x > i_x_end )
                  {
                    p_source += p_spu->i_width - i_x;
                    break;
                  }
              }

	      if (p_source >= p_src_end) {
		msg_Err( p_vout, "Trying to access beyond subtitle %dx%d %d",
			 i_x, i_y / i_yscale, i_height);
		return;
	      }
	      
	      switch( p_source->s.t )
                {
                case 0x00:
		  /* Completely transparent. Don't change pixel. */
		  break;
		  
                default:
                case MAX_ALPHA:
		  {
		    /* Completely opaque. Completely overwrite underlying
		       pixel with subtitle pixel. */
		
		    /* This is the location that's going to get changed.
		     */
		    uint8_t *p_dest = p_pixel_base_y 
                      + ((i_x * i_xscale ) >> ASCALE) * BYTES_PER_PIXEL;
                    uint8_t rgb[4];

                    yuv2rgb(p_source, rgb);
                    *p_dest++ = rgb[2];
                    *p_dest++ = rgb[1];
                    *p_dest++ = rgb[0];
		    break;
		  }

#ifdef TRANSPARENCY_FINISHED
                default:
		  {
		    /* Blend in underlying pixel subtitle pixel. */
		    
		    /* To be able to scale correctly for full opaqueness, we
		       add 1 to the alpha.  This means alpha value 0 won't
		       be completely transparent and is not correct, but
		       that's handled in a special case above anyway. */
		
		    uint8_t *p_dest = p_pixel_base_y 
                      + ((i_x * i_xscale ) >> ASCALE) * BYTES_PER_PIXEL;
		    uint8_t i_destalpha = MAX_ALPHA - p_source->s.t;
                    uint8_t rgb[3];

                    yuv2rgb(p_source, rgb);
                    rv32_pack_blend(p_dest, rgb, dest_alpha, ALPHA_SCALEDOWN);
		    break;
		  }
#endif /*TRANSPARENCY_FINISHED*/
                }
            }
        }
        else
        {
            i_ynext = p_pic->p->i_pitch * i_y >> ASCALE;


            /* Draw until we reach the end of the line */
            for( ; i_x < p_spu->i_width; i_x++, p_source++ )
            {

              if( b_crop ) {
                
                /* FIXME: y cropping should be dealt with outside of this 
                   loop.*/
                if ( i_y < i_y_start) continue;
                
                if ( i_x > i_x_end )
                  {
                    p_source += p_spu->i_width - i_x;
                    break;
                  }
              }
	      
	      if (p_source >= p_src_end) {
		msg_Err( p_vout, "Trying to access beyond subtitle %dx%d %d",
			 i_x, i_y / i_yscale, i_height);
		return;
	      }
	      
	      switch( p_source->s.t )
                {
                case 0x00:
		    /* Completely transparent. Don't change pixel. */
                    break;

                default:
                case MAX_ALPHA: 
		  {
		    /* Completely opaque. Completely overwrite underlying
		       pixel with subtitle pixel. */

		    /* This is the location that's going to get changed.
		     */
		    uint8_t *p_pixel_base_x = p_pixel_base
                      + ((i_x * i_xscale ) >> ASCALE) * BYTES_PER_PIXEL;
                    uint8_t rgb[4];
                    yuv2rgb(p_source, rgb); 

                    for(  ; i_ytmp < i_ynext ; i_ytmp += p_pic->p->i_pitch )
                    {
		      /* This is the location that's going to get changed.  */
		      uint8_t *p_dest = p_pixel_base_x + i_ytmp;
                      memcpy(p_dest, rgb, 4);
                    }
                    break;
		  }
#ifdef TRANSPARENCY_FINISHED
                default: 
                  {
                    
                    uint8_t rgb[4];
                    yuv2rgb(p_source, rgb);

                    for(  ; i_ytmp < i_ynext ; y_ytmp += p_pic->p->i_pitch )
                    {
		      /* Blend in underlying pixel subtitle pixel. */
		      
		      /* To be able to scale correctly for full opaqueness, we
			 add 1 to the alpha.  This means alpha value 0 won't
			 be completely transparent and is not correct, but
			 that's handled in a special case above anyway. */
		      uint8_t *p_dest = p_pixel_base + i_ytmp;
                      uint8_t i_destalpha = MAX_ALPHA - p_source->s.t;
                      rv32_pack_blend(p_dest, rgb, dest_alpha,ALPHA_SCALEDOWN);
                    }
                    break;
#endif /*TRANSPARENCY_FINISHED*/
		}
	    }
	}
    }
}

/* 
   Should be same as p_pic->p_format.i_bits_per_pixel / 8. But since
   we know it here, why try to compute it?
*/
#undef  BYTES_PER_PIXEL
#define BYTES_PER_PIXEL 1

static void 
BlendRGB2( vout_thread_t *p_vout, picture_t *p_pic,
            const subpicture_t *p_spu, vlc_bool_t b_crop )
{
    /* Common variables */
    uint8_t *p_pixel_base;
    uint8_t *p_src_start = (uint8_t *)p_spu->p_sys->p_data;
    uint8_t *p_src_end   = &p_src_start[p_spu->i_height * p_spu->i_width];
    uint8_t *p_source;

    int i_x, i_y;
    int i_y_src;

    /* RGB-specific */
    int i_xscale, i_yscale, i_width, i_height, i_ytmp;

    /* Crop-specific */
    int i_x_start, i_y_start, i_x_end, i_y_end;

    struct subpicture_sys_t *p_sys = p_spu->p_sys;

    i_xscale = ( p_vout->output.i_width << ASCALE ) / p_vout->render.i_width;
    i_yscale = ( p_vout->output.i_height << ASCALE ) / p_vout->render.i_height;

    dbg_print( (DECODE_DBG_CALL|DECODE_DBG_RENDER), 
	       "spu: %dx%d, scaled: %dx%d, vout render: %dx%d, scale %dx%d", 
	       p_spu->i_width,  p_spu->i_height, 
	       p_vout->output.i_width, p_vout->output.i_height,
	       p_vout->render.i_width, p_vout->render.i_height,
	       i_xscale, i_yscale
	       );

    i_width  = p_spu->i_width  * i_xscale;
    i_height = p_spu->i_height * i_yscale;

    /* Set where we will start blending subtitle from using
       the picture coordinates subtitle offsets
    */
    p_pixel_base = p_pic->p->p_pixels 
              + ( (p_spu->i_x * i_xscale) >> ASCALE ) * BYTES_PER_PIXEL
              + ( (p_spu->i_y * i_yscale) >> ASCALE ) * p_pic->p->i_pitch;

    i_x_start = p_sys->i_x_start;
    i_y_start = i_yscale * p_sys->i_y_start;
    i_x_end   = p_sys->i_x_end;
    i_y_end   = i_yscale * p_sys->i_y_end;

    p_source = (uint8_t *)p_sys->p_data;
  
    /* Draw until we reach the bottom of the subtitle */
    i_y = 0;
    for( i_y_src = 0 ; i_y_src < p_spu->i_height * p_spu->i_width; 
         i_y_src += p_spu->i_width )
      {
	uint8_t *p_pixel_base_y;
        i_ytmp = i_y >> ASCALE;
        i_y += i_yscale;
	p_pixel_base_y = p_pixel_base + (i_ytmp * p_pic->p->i_pitch);
	i_x = 0;

#if 0
        /* Remove this */        
        *p_pixel_base_y = 0xff;         /*+++++*/
        *(p_pixel_base_y+1) = 0x0;
        *(p_pixel_base_y+2) = 0x0;
        *(p_pixel_base_y+3) = 0x0;
        *(p_pixel_base_y+4) = 0x0;
        *(p_pixel_base_y+5) = 0x0;
        *(p_pixel_base_y+6) = 0x0;
        *(p_pixel_base_y+7) = 0x0;
        *(p_pixel_base_y+8) = 0xFF;
        *(p_pixel_base_y+9) = 0xFF;
        *(p_pixel_base_y+10) = 0xFF;
        *(p_pixel_base_y+11) = 0xFF;
        *(p_pixel_base_y+12) = 0xFF;
        *(p_pixel_base_y+13) = 0xFF;
        *(p_pixel_base_y+14) = 0xFF;
        *(p_pixel_base_y+15) = 0xFF;
#endif

        if ( b_crop ) {
          if ( i_y > i_y_end ) break;
          if (i_x_start) {
            i_x = i_x_start;
            p_source += i_x_start;
          }
        }
        
        /* Check whether we need to draw one line or more than one */
        if( i_ytmp + 1 >= ( i_y >> ASCALE ) )
        {

          /* Draw until we reach the end of the line */
          for( ; i_x < p_spu->i_width; i_x ++, p_source++ )
            {
              if( b_crop ) {
                
                /* FIXME: y cropping should be dealt with outside of this 
                   loop.*/
                if ( i_y < i_y_start) continue;
                
                if ( i_x > i_x_end )
                  {
                    p_source += p_spu->i_width - i_x;
                    break;
                  }
              }
              
              if (p_source >= p_src_end) {
                msg_Err( p_vout, "Trying to access beyond subtitle %dx%d %d",
                         i_x, i_y / i_yscale, i_height);
                return;
              }
              
              if (*p_source == 0) {
#if 0
                printf(" "); /*++++*/
#endif
              } else {
                uint8_t *p_dest = p_pixel_base_y + ((i_x*i_xscale) >> 7);
                *p_dest++ = 0xff;
#if 0
                printf("%1d", *p_source); /*++++*/
#endif
              }
              
            }
#if 0
          printf("\n"); /*++++*/
#endif
        } else {
          /* Have to scale over many lines. */
          int i_yreal = p_pic->p->i_pitch * i_ytmp;
          int i_ynext = p_pic->p->i_pitch * i_y >> ASCALE;

           /* Draw until we reach the end of the line */
           for( ; i_x < p_spu->i_width; i_x ++, p_source++ )
             {
              if( b_crop ) {
                
                /* FIXME: y cropping should be dealt with outside of this 
                   loop.*/
                if ( i_y < i_y_start) continue;
                
                if ( i_x > i_x_end )
                  {
                    p_source += p_spu->i_width - i_x;
                    break;
                  }
              }
              
              if (p_source >= p_src_end) {
                msg_Err( p_vout, "Trying to access beyond subtitle %dx%d %d",
                         i_x, i_y / i_yscale, i_height);
                return;
              }
              
              if (*p_source != 0) {
                printf("_"); /*++++*/
              } else {
                printf("%1d", *p_source); /*++++*/
                for( i_ytmp = i_yreal ; i_ytmp < i_ynext ;
                     i_ytmp += p_pic->p->i_pitch ) {
                  uint8_t *p_dest = p_pixel_base + i_ytmp 
                    + i_x * BYTES_PER_PIXEL;
                  *p_dest++ = 0xff;
                }
              }
            }

        }
      }
}


/* 
 * Local variables:
 *  c-file-style: "gnu"
 *  tab-width: 8
 *  indent-tabs-mode: nil
 * End:
 */
