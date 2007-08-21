/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "win_local.h"
#include "win_glimp.h"
#include "win_wgl.h"

void ( APIENTRY * qglDrawBuffer )(GLenum mode);
const GLubyte * ( APIENTRY * qglGetString )(GLenum name);

int   ( WINAPI * qwglChoosePixelFormat )(HDC, CONST PIXELFORMATDESCRIPTOR *);
int   ( WINAPI * qwglDescribePixelFormat) (HDC, int, UINT, LPPIXELFORMATDESCRIPTOR);
int   ( WINAPI * qwglGetPixelFormat)(HDC);
BOOL  ( WINAPI * qwglSetPixelFormat)(HDC, int, CONST PIXELFORMATDESCRIPTOR *);
BOOL  ( WINAPI * qwglSwapBuffers)(HDC);
BOOL  ( WINAPI * qwglSwapIntervalEXT)( int interval );

BOOL  ( WINAPI * qwglCopyContext)(HGLRC, HGLRC, UINT);
HGLRC ( WINAPI * qwglCreateContext)(HDC);
HGLRC ( WINAPI * qwglCreateLayerContext)(HDC, int);
BOOL  ( WINAPI * qwglDeleteContext)(HGLRC);
HGLRC ( WINAPI * qwglGetCurrentContext)(VOID);
HDC   ( WINAPI * qwglGetCurrentDC)(VOID);
PROC  ( WINAPI * qwglGetProcAddress)(LPCSTR);
BOOL  ( WINAPI * qwglMakeCurrent)(HDC, HGLRC);
BOOL  ( WINAPI * qwglShareLists)(HGLRC, HGLRC);
BOOL ( WINAPI * qwglDescribeLayerPlane)(HDC, int, int, UINT,
                                            LPLAYERPLANEDESCRIPTOR);
int  ( WINAPI * qwglSetLayerPaletteEntries)(HDC, int, int, int,
                                                CONST COLORREF *);
int  ( WINAPI * qwglGetLayerPaletteEntries)(HDC, int, int, int,
                                                COLORREF *);
BOOL ( WINAPI * qwglRealizeLayerPalette)(HDC, int, BOOL);
BOOL ( WINAPI * qwglSwapLayerBuffers)(HDC, UINT);

BOOL ( WINAPI * qwglGetDeviceGammaRampEXT)( unsigned char *, unsigned char *, unsigned char * );
BOOL ( WINAPI * qwglSetDeviceGammaRampEXT)( const unsigned char *, const unsigned char *, const unsigned char * );

void WGL_Shutdown( void ) {
    if( glw.hinstOpenGL ) {
    	FreeLibrary( glw.hinstOpenGL );
	    glw.hinstOpenGL = NULL;
    }

	qglDrawBuffer	= NULL;
	qglGetString	= NULL;

	qwglCopyContext              = NULL;
	qwglCreateContext            = NULL;
	qwglCreateLayerContext       = NULL;
	qwglDeleteContext            = NULL;
	qwglDescribeLayerPlane       = NULL;
	qwglGetCurrentContext        = NULL;
	qwglGetCurrentDC             = NULL;
	qwglGetLayerPaletteEntries   = NULL;
	qwglGetProcAddress           = NULL;
	qwglMakeCurrent              = NULL;
	qwglRealizeLayerPalette      = NULL;
	qwglSetLayerPaletteEntries   = NULL;
	qwglShareLists               = NULL;
	qwglSwapLayerBuffers         = NULL;

	qwglChoosePixelFormat        = NULL;
	qwglDescribePixelFormat      = NULL;
	qwglGetPixelFormat           = NULL;
	qwglSetPixelFormat           = NULL;
	qwglSwapBuffers              = NULL;

	qwglSwapIntervalEXT          = NULL;
	qwglGetDeviceGammaRampEXT    = NULL;
	qwglSetDeviceGammaRampEXT    = NULL;
}


#define GPA( x ) do { \
        q ## x = ( void * )GetProcAddress( glw.hinstOpenGL, #x ); \
        if( !q ## x ) { \
            return qfalse; \
        } \
    } while( 0 )

qboolean WGL_Init( const char *dllname ) {
	if( ( glw.hinstOpenGL = LoadLibrary( dllname ) ) == NULL ) {
		return qfalse;
	}

	GPA( glDrawBuffer );
	GPA( glGetString );

	GPA( wglCopyContext );
	GPA( wglCreateContext );
	GPA( wglCreateLayerContext );
	GPA( wglDeleteContext );
	GPA( wglDescribeLayerPlane );
	GPA( wglGetCurrentContext );
	GPA( wglGetCurrentDC );
	GPA( wglGetLayerPaletteEntries );
	GPA( wglGetProcAddress );
	GPA( wglMakeCurrent );
	GPA( wglRealizeLayerPalette );
	GPA( wglSetLayerPaletteEntries );
	GPA( wglShareLists );
	GPA( wglSwapLayerBuffers );

	GPA( wglChoosePixelFormat );
	GPA( wglDescribePixelFormat );
	GPA( wglGetPixelFormat );
	GPA( wglSetPixelFormat );
	GPA( wglSwapBuffers );

	qwglSwapIntervalEXT			 = NULL;
	qwglGetDeviceGammaRampEXT	 = NULL;
	qwglSetDeviceGammaRampEXT	 = NULL;

	return qtrue;
}

void *WGL_GetProcAddress( const char *symbol ) {
    return ( void * )GetProcAddress( glw.hinstOpenGL, symbol );
}

#undef GPA
