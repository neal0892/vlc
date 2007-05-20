/*****************************************************************************
 * libvlc.h:  libvlc_* new external API structures
 *****************************************************************************
 * Copyright (C) 1998-2007 the VideoLAN team
 * $Id $
 *
 * Authors: Filippo Carone <littlejohn@videolan.org>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef _LIBVLC_STRUCTURES_H
#define _LIBVLC_STRUCTURES_H 1

#include <vlc/vlc.h>

# ifdef __cplusplus
extern "C" {
# endif

/** This structure is opaque. It represents a libvlc instance */
typedef struct libvlc_instance_t libvlc_instance_t;

/*****************************************************************************
 * Exceptions
 *****************************************************************************/

/** defgroup libvlc_exception Exceptions
 * \ingroup libvlc
 * LibVLC Exceptions handling
 * @{
 */

typedef struct
{
    int b_raised;
    int i_code;
    char *psz_message;
} libvlc_exception_t;

/**@} */

/*****************************************************************************
 * Playlist
 *****************************************************************************/
/** defgroup libvlc_playlist Playlist
 * \ingroup libvlc
 * LibVLC Playlist handling
 * @{
 */

typedef struct {
    int i_id;
    char * psz_uri;
    char * psz_name;

} libvlc_playlist_item_t;

/**@} */


/*****************************************************************************
 * Video
 *****************************************************************************/
/** defgroup libvlc_video Video
 * \ingroup libvlc
 * LibVLC Video handling
 * @{
 */
    
/**
* Downcast to this general type as placeholder for a platform specific one, such as:
*  Drawable on X11,
*  CGrafPort on MacOSX,
*  HWND on win32
*/
typedef int libvlc_drawable_t;

/**
* Rectangle type for video geometry
*/
typedef struct
{
    int top, left;
    int bottom, right;
}
libvlc_rectangle_t;

/**@} */


/*****************************************************************************
 * Message log handling
 *****************************************************************************/

/** defgroup libvlc_log Log
 * \ingroup libvlc
 * LibVLC Message Logging
 * @{
 */

/** This structure is opaque. It represents a libvlc log instance */
typedef struct libvlc_log_t libvlc_log_t;

/** This structure is opaque. It represents a libvlc log iterator */
typedef struct libvlc_log_iterator_t libvlc_log_iterator_t;

typedef struct libvlc_log_message_t
{
    unsigned    sizeof_msg;   /* sizeof() of message structure, must be filled in by user */
    int         i_severity;   /* 0=INFO, 1=ERR, 2=WARN, 3=DBG */
    const char *psz_type;     /* module type */
    const char *psz_name;     /* module name */
    const char *psz_header;   /* optional header */
    const char *psz_message;  /* message */
} libvlc_log_message_t;

/**@} */

/*****************************************************************************
 * Callbacks handling
 *****************************************************************************/

/** defgroup libvlc_callbacks Callbacks
 * \ingroup libvlc
 * LibVLC Event Callbacks
 * @{
 */
    
/**
 * Available events:
 * - VOLUME_CHANGED
 * - INPUT_POSITION_CHANGED
 */

typedef enum {
    VOLUME_CHANGED,
    INPUT_POSITION_CHANGED,
} libvlc_event_type_t;

typedef enum {
    INT_EVENT,
    BOOLEAN_EVENT,
    FLOAT_EVENT,
    STRING_EVENT,
    ADDRESS_EVENT,
    OBJECT_EVENT,
    LIST_EVENT,
    TIME_EVENT,
    VAR_EVENT,
} libvlc_event_value_type_t;

typedef struct 
{
    libvlc_event_type_t type;
    libvlc_event_value_type_t value_type;
    vlc_value_t old_value;
    vlc_value_t new_value;
    char reserved[8]; /* For future use */
} libvlc_event_t;

/**
 * Callback function notification
 * \param p_instance the libvlc instance
 * \param p_event the event triggering the callback
 * \param p_user_data user provided data
 */

typedef void ( *libvlc_callback_t )( struct libvlc_instance_t *, libvlc_event_t *, void * );

/**@} */

# ifdef __cplusplus
}
# endif

#endif
