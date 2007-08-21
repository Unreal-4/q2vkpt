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
// cl.input.c  -- builds an intended movement command to send to the server

#include "cl_local.h"
#include "vid_public.h"
#include "vid_local.h"

static cvar_t	*cl_nodelta;
static cvar_t	*cl_maxpackets;
static cvar_t	*cl_packetdup;
static cvar_t	*cl_fuzzhack;
static cvar_t	*cl_showpackets;
static cvar_t	*cl_instantpacket;

static cvar_t	*m_filter;
static cvar_t	*m_accel;

static cvar_t	*cl_upspeed;
static cvar_t	*cl_forwardspeed;
static cvar_t	*cl_sidespeed;
static cvar_t	*cl_yawspeed;
static cvar_t	*cl_pitchspeed;
static cvar_t	*cl_run;
static cvar_t	*cl_anglespeedkey;

static cvar_t	*freelook;
static cvar_t	*lookspring;
static cvar_t	*lookstrafe;
static cvar_t	*sensitivity;

static cvar_t	*m_pitch;
static cvar_t	*m_yaw;
static cvar_t	*m_forward;
static cvar_t	*m_side;

static qboolean mlooking;

static uint32	frame_msec;
static uint32	old_eventTime;

inputAPI_t	input;

void CL_CenterView (void);

/*
===============================================================================

INPUT LIBRARY

===============================================================================
*/

static cvar_t	*in_driver;
static cvar_t	*in_enable;

typedef struct inputDriver_s {
	char *name;
	void	(*FillAPI)( inputAPI_t *api );
} inputDriver_t;

#ifdef USE_DINPUT
void DI_FillAPI( inputAPI_t *api );
#endif

#ifdef USE_EVDEV
void Evdev_FillAPI( inputAPI_t *api );
#endif

static inputDriver_t	in_driverTable[] = {
	/* fallback driver should be present on all systems */
	{ "video", Video_FillInputAPI },

	/* DirectInput driver */
#ifdef USE_DINPUT
	{ "dinput", DI_FillAPI },
#endif
#ifdef USE_EVDEV
	{ "evdev", Evdev_FillAPI },
#endif
};

static int in_numDrivers = sizeof( in_driverTable ) / sizeof( in_driverTable[0] );

/*
============
CL_InitInput
============
*/
void CL_InputActivate( void ) {
    qboolean active = cls.appactive;

	if( !cls.input_initialized ) {
		return;
	}

	if( ( cls.key_dest & KEY_CONSOLE ) && !( scr_glconfig.flags & QVF_FULLSCREEN ) ) {
		/* deactivate in windowed mode if console is open */
        active = qfalse;
    }

	input.ClearStates();
	input.Activate( active );
}

/*
============
CL_InitInput
============
*/
void CL_InputFrame( void ) {
	if( cls.input_initialized ) {
	    input.Frame();
    }
}

/*
============
CL_RestartInput_f
============
*/
static void CL_RestartInput_f( void ) {
	CL_ShutdownInput();
	CL_InitInput();
}

static void CL_ReleaseInput_f( void ) {
	if( cls.input_initialized ) {
	    input.ClearStates();
    	input.Activate( qfalse );
    }
}

/*
============
CL_ShutdownInput
============
*/
void CL_ShutdownInput( void ) {
	if( !cls.input_initialized ) {
		return;
	}

	Cmd_RemoveCommand( "in_restart" );
	Cmd_RemoveCommand( "in_release" );

	input.Shutdown();
	cls.input_initialized = qfalse;
}

/*
============
CL_InitInput
============
*/
void CL_InitInput( void ) {
	inputDriver_t *driver;
	int i;
	in_driver = Cvar_Get( "in_driver", DEFAULT_INPUT_DRIVER, CVAR_LATCHED );
	in_driver->subsystem = CVAR_SYSTEM_INPUT;
	in_enable = Cvar_Get( "in_enable", "1", CVAR_LATCHED );
	in_enable->subsystem = CVAR_SYSTEM_INPUT;

	if( !in_enable->integer ) {
		Com_Printf( "Non-keyboard input disabled\n" );
		return;
	}

	while( 1 ) {
		if( in_driver->string[0] ) {
			for( i = 0, driver = in_driverTable; i < in_numDrivers; i++, driver++ ) {
				if( !strcmp( in_driver->string, driver->name ) ) {
					break;
				}
			}
			if( i == in_numDrivers ) {
				Com_Printf( "Input driver '%s' not found, falling back to default...\n", in_driver->string );
				Cvar_Set( "in_driver", "" );
				driver = &in_driverTable[0];
			}
		} else {
			driver = &in_driverTable[0];
		}
		
		driver->FillAPI( &input );

		if( input.Init() ) {
			break;
		}
		if( !in_driver->string[0] ) {
			Com_WPrintf( "Couldn't fall back to default input driver!\n"
				"Non-keyboard input disabled\n" );
			return;
		}

		Com_Printf( "Failed to load input driver, falling back to default...\n" );
		Cvar_Set( "in_driver", "" );
	}

	Cmd_AddCommand( "in_restart", CL_RestartInput_f );
	Cmd_AddCommand( "in_release", CL_ReleaseInput_f );

	cls.input_initialized = qtrue;

	CL_InputActivate();
}


/*
===============================================================================

KEY BUTTONS

Continuous button event tracking is complicated by the fact that two different
input sources (say, mouse button 1 and the control key) can both press the
same button, but the button should only be released when both of the
pressing key have been released.

When a key event issues a button command (+forward, +attack, etc), it appends
its key number as a parameter to the command so it can be matched up with
the release.

state bit 0 is the current state of the key
state bit 1 is edge triggered on the up to down transition
state bit 2 is edge triggered on the down to up transition


Key_Event (int key, qboolean down, unsigned time);

  +mlook src time

===============================================================================
*/

typedef struct kbutton_s {
	int			down[2];		// key nums holding it down
	uint32		downtime;		// msec timestamp
	uint32		msec;			// msec down this frame
	int			state;
} kbutton_t;

static kbutton_t	in_klook;
static kbutton_t	in_left, in_right, in_forward, in_back;
static kbutton_t	in_lookup, in_lookdown, in_moveleft, in_moveright;
static kbutton_t	in_strafe, in_speed, in_use, in_attack;
static kbutton_t	in_up, in_down;

static int			in_impulse;


static void KeyDown (kbutton_t *b)
{
	int		k;
	char	*c;
	
	c = Cmd_Argv(1);
	if (c[0])
		k = atoi(c);
	else
		k = -1;		// typed manually at the console for continuous down

	if (k == b->down[0] || k == b->down[1])
		return;		// repeating key
	
	if (!b->down[0])
		b->down[0] = k;
	else if (!b->down[1])
		b->down[1] = k;
	else
	{
		Com_WPrintf ("Three keys down for a button!\n");
		return;
	}
	
	if (b->state & 1)
		return;		// still down

	// save timestamp
	c = Cmd_Argv(2);
	b->downtime = atoi(c);
	if (!b->downtime) {
		b->downtime = com_eventTime - 100;
	}

	b->state |= 1 + 2;	// down + impulse down
}

static void KeyUp (kbutton_t *b)
{
	int		k;
	char	*c;
	uint32	uptime;

	c = Cmd_Argv(1);
	if (c[0])
		k = atoi(c);
	else
	{ // typed manually at the console, assume for unsticking, so clear all
		b->down[0] = b->down[1] = 0;
		b->state = 0;	// impulse up
		return;
	}

	if (b->down[0] == k)
		b->down[0] = 0;
	else if (b->down[1] == k)
		b->down[1] = 0;
	else
		return;		// key up without coresponding down (menu pass through)
	if (b->down[0] || b->down[1])
		return;		// some other key is still holding it down

	if (!(b->state & 1))
		return;		// still up (this should not happen)

	// save timestamp
	c = Cmd_Argv(2);
	uptime = atoi(c);
	if( !uptime ) {
		b->msec += 10;
	} else if( uptime > b->downtime ) {
		b->msec += uptime - b->downtime;
	}

	b->state = 0;		// now up
	//b->state |= 4; 		// impulse up
}

static void IN_KLookDown( void ) { KeyDown( &in_klook ); }
static void IN_KLookUp( void ) { KeyUp( &in_klook ); }
static void IN_UpDown( void ) { KeyDown( &in_up ); }
static void IN_UpUp( void ) { KeyUp( &in_up ); }
static void IN_DownDown( void ) { KeyDown( &in_down ); }
static void IN_DownUp( void ) { KeyUp( &in_down ); }
static void IN_LeftDown( void ) { KeyDown( &in_left ); }
static void IN_LeftUp( void ) { KeyUp( &in_left ); }
static void IN_RightDown( void ) { KeyDown( &in_right ); }
static void IN_RightUp( void ) { KeyUp( &in_right ); }
static void IN_ForwardDown( void ) { KeyDown( &in_forward ); }
static void IN_ForwardUp( void ) { KeyUp( &in_forward ); }
static void IN_BackDown( void ) { KeyDown( &in_back ); }
static void IN_BackUp( void ) { KeyUp( &in_back ); }
static void IN_LookupDown( void ) { KeyDown( &in_lookup ); }
static void IN_LookupUp( void ) { KeyUp( &in_lookup ); }
static void IN_LookdownDown( void ) { KeyDown( &in_lookdown ); }
static void IN_LookdownUp( void ) { KeyUp( &in_lookdown ); }
static void IN_MoveleftDown( void ) { KeyDown( &in_moveleft ); }
static void IN_MoveleftUp( void ) { KeyUp( &in_moveleft ); }
static void IN_MoverightDown( void ) { KeyDown( &in_moveright ); }
static void IN_MoverightUp( void ) { KeyUp( &in_moveright ); }
static void IN_SpeedDown( void ) { KeyDown( &in_speed ); }
static void IN_SpeedUp( void ) { KeyUp( &in_speed ); }
static void IN_StrafeDown( void ) { KeyDown( &in_strafe ); }
static void IN_StrafeUp( void ) { KeyUp( &in_strafe ); }
static void IN_AttackDown( void ) {
    KeyDown( &in_attack );
    if( cl_instantpacket->integer ) {
        cl.sendPacketNow = qtrue;
    }
}
static void IN_AttackUp( void ) { KeyUp( &in_attack ); }
static void IN_UseDown ( void ) {
    KeyDown( &in_use );
    if( cl_instantpacket->integer ) {
        cl.sendPacketNow = qtrue;
    }
}
static void IN_UseUp ( void ) { KeyUp( &in_use ); }

static void IN_Impulse ( void ) {
	in_impulse = atoi( Cmd_Argv( 1 ) );
}

static void IN_MLookDown( void ) {
	mlooking = qtrue;
}

static void IN_MLookUp( void ) {
	mlooking = qfalse;

	if( !freelook->integer && lookspring->integer )
		CL_CenterView();
}

/*
===============
CL_KeyState

Returns the fraction of the frame that the key was down
===============
*/
float CL_KeyState( kbutton_t *key ) {
	float		val;
	uint32		msec;

	key->state &= ~2;		// clear impulses

	msec = key->msec;
	key->msec = 0;

	if( key->state ) {
		// still down
		if( com_eventTime > key->downtime ) {
			msec += com_eventTime - key->downtime;
		}
		key->downtime = com_eventTime;
	}

	if( !frame_msec ) {
		return 0;
	}
	val = ( float )msec / frame_msec;

	clamp( val, 0, 1 );
    
	return val;
}

// FIXME: always discrete?
float CL_ImmKeyState( kbutton_t *key ) {
	if( key->state ) {
		return 1;
	}

	return 0;
}


//==========================================================================

static int mouse_x;
static int mouse_y;
static int old_mouse_x;
static int old_mouse_y;

/*
================
CL_MouseEvent
================
*/
void CL_MouseEvent( int dx, int dy ) {
	if( cls.key_dest & KEY_MENU ) {
		UI_MouseMove( dx, dy );
		return;
	}

	mouse_x += dx;
	mouse_y += dy;
}

/*
================
CL_MouseMove
================
*/
static void CL_MouseMove( void ) {
	float		mx, my;
	float	speed;

	if( m_filter->integer ) {
		mx = ( mouse_x + old_mouse_x ) * 0.5f;
		my = ( mouse_y + old_mouse_y ) * 0.5f;
	} else {
		mx = mouse_x;
		my = mouse_y;
	}

	old_mouse_x = mouse_x;
	old_mouse_y = mouse_y;
	mouse_x = 0;
	mouse_y = 0;

	if( !mx && !my ) {
		return;
	}

	Cvar_ClampValue( m_accel, 0, 1 );

	speed = sqrt( mx * mx + my * my );
	speed = sensitivity->value + speed * m_accel->value;

	mx *= speed;
	my *= speed;

// add mouse X/Y movement to cmd
	if( ( in_strafe.state & 1 ) || ( lookstrafe->integer && mlooking ) ) {
		cl.cmd.sidemove += m_side->value * mx;
	} else {
		cl.viewangles[YAW] -= m_yaw->value * mx;
	}

	if( ( mlooking || freelook->integer ) && !( in_strafe.state & 1 ) ) {
		cl.viewangles[PITCH] += m_pitch->value * my;
	} else {
		cl.cmd.forwardmove -= m_forward->value * my;
	}
}


/*
================
CL_AdjustAngles

Moves the local angle positions
================
*/
static void CL_AdjustAngles (void)
{
	float	speed;
	float	up, down;
	
	if (in_speed.state & 1)
		speed = cls.frametime * cl_anglespeedkey->value;
	else
		speed = cls.frametime;

	if (!(in_strafe.state & 1))
	{
		cl.viewangles[YAW] -= speed*cl_yawspeed->value*CL_ImmKeyState (&in_right);
		cl.viewangles[YAW] += speed*cl_yawspeed->value*CL_ImmKeyState (&in_left);
	}
	if (in_klook.state & 1)
	{
		cl.viewangles[PITCH] -= speed*cl_pitchspeed->value * CL_ImmKeyState (&in_forward);
		cl.viewangles[PITCH] += speed*cl_pitchspeed->value * CL_ImmKeyState (&in_back);
	}
	
	up = CL_ImmKeyState (&in_lookup);
	down = CL_ImmKeyState(&in_lookdown);
	
	cl.viewangles[PITCH] -= speed*cl_pitchspeed->value * up;
	cl.viewangles[PITCH] += speed*cl_pitchspeed->value * down;
}

/*
================
CL_BaseMove

Send the intended movement message to the server
================
*/
static void CL_BaseMove( void ) {
	vec3_t move;
	
	VectorClear( move );
	if( in_strafe.state & 1 ) {
		move[1] += cl_sidespeed->value * CL_KeyState( &in_right );
		move[1] -= cl_sidespeed->value * CL_KeyState( &in_left );
	}

	move[1] += cl_sidespeed->value * CL_KeyState( &in_moveright );
	move[1] -= cl_sidespeed->value * CL_KeyState( &in_moveleft );

	move[2] += cl_upspeed->value * CL_KeyState( &in_up );
	move[2] -= cl_upspeed->value * CL_KeyState( &in_down );

	if( !( in_klook.state & 1 ) ) {
		move[0] += cl_forwardspeed->value * CL_KeyState( &in_forward );
		move[0] -= cl_forwardspeed->value * CL_KeyState( &in_back );
	}	

//
// adjust for speed key / running
//
	if( ( in_speed.state & 1 ) ^ cl_run->integer ) {
		VectorScale( move, 2, move );
	}

	cl.cmd.forwardmove = move[0];
	cl.cmd.sidemove = move[1];
	cl.cmd.upmove = move[2];
}

/*
================
CL_ImmBaseMove

Builds intended movement message for
local pmove sampling
================
*/
static void CL_ImmBaseMove( void ) {
	VectorClear( cl.move );
	if( in_strafe.state & 1 ) {
		cl.move[1] += cl_sidespeed->value * CL_ImmKeyState( &in_right );
		cl.move[1] -= cl_sidespeed->value * CL_ImmKeyState( &in_left );
	}

	cl.move[1] += cl_sidespeed->value * CL_ImmKeyState( &in_moveright );
	cl.move[1] -= cl_sidespeed->value * CL_ImmKeyState( &in_moveleft );

	cl.move[2] += cl_upspeed->value * CL_ImmKeyState( &in_up );
	cl.move[2] -= cl_upspeed->value * CL_ImmKeyState( &in_down );

	if( !( in_klook.state & 1 ) ) {
		cl.move[0] += cl_forwardspeed->value * CL_ImmKeyState( &in_forward );
		cl.move[0] -= cl_forwardspeed->value * CL_ImmKeyState( &in_back );
	}	

//
// adjust for speed key / running
//
	if( ( in_speed.state & 1 ) ^ cl_run->integer ) {
		VectorScale( cl.move, 2, cl.move );
	}
}

static void CL_ClampPitch( void ) {
	float	pitch;

	pitch = SHORT2ANGLE( cl.frame.ps.pmove.delta_angles[PITCH] );
	if (pitch > 180)
		pitch -= 360;

	if (cl.viewangles[PITCH] + pitch < -360)
		cl.viewangles[PITCH] += 360; // wrapped
	if (cl.viewangles[PITCH] + pitch > 360)
		cl.viewangles[PITCH] -= 360; // wrapped

	if (cl.viewangles[PITCH] + pitch > 89)
		cl.viewangles[PITCH] = 89 - pitch;
	if (cl.viewangles[PITCH] + pitch < -89)
		cl.viewangles[PITCH] = -89 - pitch;
}

/*
=================
CL_UpdateCmd
=================
*/
void CL_UpdateCmd( int msec ) {
	// adjust viewangles
	CL_AdjustAngles();
	
	// get basic movement from keyboard
	CL_ImmBaseMove();

	// allow mice or other external controllers to add to the move
	CL_MouseMove();

	// send milliseconds of time to apply the move
	cl.cmd.msec += msec;

	CL_ClampPitch();
	
	cl.cmd.angles[0] = ANGLE2SHORT( cl.viewangles[0] );
	cl.cmd.angles[1] = ANGLE2SHORT( cl.viewangles[1] );
	cl.cmd.angles[2] = ANGLE2SHORT( cl.viewangles[2] );
}


void CL_CenterView( void ) {
	cl.viewangles[PITCH] = -SHORT2ANGLE( cl.frame.ps.pmove.delta_angles[PITCH] );
}

/*
============
CL_RegisterInput
============
*/
void CL_RegisterInput( void ) {
	Cmd_AddCommand ("centerview",CL_CenterView);

	Cmd_AddCommand ("+moveup",IN_UpDown);
	Cmd_AddCommand ("-moveup",IN_UpUp);
	Cmd_AddCommand ("+movedown",IN_DownDown);
	Cmd_AddCommand ("-movedown",IN_DownUp);
	Cmd_AddCommand ("+left",IN_LeftDown);
	Cmd_AddCommand ("-left",IN_LeftUp);
	Cmd_AddCommand ("+right",IN_RightDown);
	Cmd_AddCommand ("-right",IN_RightUp);
	Cmd_AddCommand ("+forward",IN_ForwardDown);
	Cmd_AddCommand ("-forward",IN_ForwardUp);
	Cmd_AddCommand ("+back",IN_BackDown);
	Cmd_AddCommand ("-back",IN_BackUp);
	Cmd_AddCommand ("+lookup", IN_LookupDown);
	Cmd_AddCommand ("-lookup", IN_LookupUp);
	Cmd_AddCommand ("+lookdown", IN_LookdownDown);
	Cmd_AddCommand ("-lookdown", IN_LookdownUp);
	Cmd_AddCommand ("+strafe", IN_StrafeDown);
	Cmd_AddCommand ("-strafe", IN_StrafeUp);
	Cmd_AddCommand ("+moveleft", IN_MoveleftDown);
	Cmd_AddCommand ("-moveleft", IN_MoveleftUp);
	Cmd_AddCommand ("+moveright", IN_MoverightDown);
	Cmd_AddCommand ("-moveright", IN_MoverightUp);
	Cmd_AddCommand ("+speed", IN_SpeedDown);
	Cmd_AddCommand ("-speed", IN_SpeedUp);
	Cmd_AddCommand ("+attack", IN_AttackDown);
	Cmd_AddCommand ("-attack", IN_AttackUp);
	Cmd_AddCommand ("+use", IN_UseDown);
	Cmd_AddCommand ("-use", IN_UseUp);
	Cmd_AddCommand ("impulse", IN_Impulse);
	Cmd_AddCommand ("+klook", IN_KLookDown);
	Cmd_AddCommand ("-klook", IN_KLookUp);
	Cmd_AddCommand( "+mlook", IN_MLookDown );
	Cmd_AddCommand( "-mlook", IN_MLookUp );

	cl_nodelta = Cvar_Get ("cl_nodelta", "0", 0);
	cl_maxpackets = Cvar_Get( "cl_maxpackets", "30", 0 );
	cl_fuzzhack = Cvar_Get( "cl_fuzzhack", "0", 0 );
	cl_packetdup = Cvar_Get( "cl_packetdup", "1", 0 );
	cl_showpackets = Cvar_Get( "cl_showpackets", "0", 0 );
	cl_instantpacket = Cvar_Get( "cl_instantpacket", "1", 0 );

    cl_upspeed = Cvar_Get ( "cl_upspeed", "200", 0 );
    cl_forwardspeed = Cvar_Get ( "cl_forwardspeed", "200", 0 );
    cl_sidespeed = Cvar_Get ( "cl_sidespeed", "200", 0 );
    cl_yawspeed = Cvar_Get ( "cl_yawspeed", "140", 0 );
    cl_pitchspeed = Cvar_Get ( "cl_pitchspeed", "150", CVAR_CHEAT );
    cl_anglespeedkey = Cvar_Get ( "cl_anglespeedkey", "1.5", CVAR_CHEAT );
    cl_run = Cvar_Get( "cl_run", "1", CVAR_ARCHIVE );

    freelook = Cvar_Get( "freelook", "0", CVAR_ARCHIVE );
    lookspring = Cvar_Get ( "lookspring", "0", CVAR_ARCHIVE );
    lookstrafe = Cvar_Get ( "lookstrafe", "0", CVAR_ARCHIVE );
    sensitivity = Cvar_Get ( "sensitivity", "3", CVAR_ARCHIVE );

    m_pitch = Cvar_Get ( "m_pitch", "0.022", CVAR_ARCHIVE );
    m_yaw = Cvar_Get ( "m_yaw", "0.022", 0 );
    m_forward = Cvar_Get ( "m_forward", "1", 0 );
    m_side = Cvar_Get ( "m_side", "1", 0 );
	m_filter = Cvar_Get( "m_filter", "0", 0 );
	m_accel = Cvar_Get( "m_accel", "0", 0 );
}

void CL_FinalizeCmd( void ) {
    // command buffer ticks in sync with cl_maxfps
    if( cmd_buffer.waitCount > 0 ) {
        cmd_buffer.waitCount--;
    }

	if( cls.state < ca_active ) {
		return; // not talking to a server
	}

	if( sv_paused->integer ) {
		return;
	}

	frame_msec = com_eventTime - old_eventTime;
	clamp( frame_msec, 1, 200 );

//
// figure button bits
//	
	if ( in_attack.state & 3 )
		cl.cmd.buttons |= BUTTON_ATTACK;
	if (in_use.state & 3)
		cl.cmd.buttons |= BUTTON_USE;

	in_attack.state &= ~2;
	in_use.state &= ~2;

	if( !( cls.key_dest & (KEY_CONSOLE|KEY_MENU|KEY_MESSAGE) ) &&
            Key_AnyKeyDown() )
		cl.cmd.buttons |= BUTTON_ANY;

	if( cl.cmd.msec > 250 ) {
		cl.cmd.msec = 100;		// time was unreasonable
	}

	CL_BaseMove();

	cl.cmd.impulse = in_impulse;
	in_impulse = 0;

	// save this command off for prediction
	cl.cmdNumber++;
	cl.cmds[cl.cmdNumber & CMD_MASK] = cl.cmd;

	// clear pending cmd
	memset( &cl.cmd, 0, sizeof( cl.cmd ) );

	old_eventTime = com_eventTime;
}

/*
=================
CL_ReadyToSend
=================
*/
static inline qboolean CL_ReadyToSend( void ) {
	int msec;
	
	if( cl.sendPacketNow ) {
		return qtrue;
	}
	if( cls.netchan->message.cursize || cls.netchan->reliable_ack_pending ) {
		return qtrue;
	}
	if( cls.serverProtocol != PROTOCOL_VERSION_Q2PRO && !cl_fuzzhack->integer ) 
    {
		return qtrue;
	}
	if( !cl_maxpackets->integer ) {
		return qtrue;
	}

	if( cl_maxpackets->integer < 10 ) {
		Cvar_SetInteger( "cl_maxpackets", 10 );
	}
	if( cl.lastTransmitTime > cls.realtime ) {
		cl.lastTransmitTime = cls.realtime;
	}

	msec = 1000 / cl_maxpackets->integer;
	if( msec ) {
		msec = 100 / ( 100 / msec );
	}
	//Com_Printf( "%d\n", msec );
	if( cls.realtime - cl.lastTransmitTime < msec ) {
		return qfalse;
	}

	return qtrue;
}

/*
=================
CL_SendDefaultCmd
=================
*/
static void CL_SendDefaultCmd( void ) {
	int			i;
	int			checksumIndex;
	usercmd_t	*cmd, *oldcmd;
	client_history_t	*history;

	// archive this packet
	history = &cl.history[cls.netchan->outgoing_sequence & CMD_MASK];
	history->cmdNumber = cl.cmdNumber;
	history->realtime = cls.realtime;	// for ping calculation

	cl.lastTransmitCmdNumber = cl.cmdNumber;

	// see if we are ready to send this packet
	if( !CL_ReadyToSend() ) {
		if( cl.cmdNumber - cl.lastTransmitCmdNumber < 3 ) {
			cls.netchan->outgoing_sequence++; // HACK: just drop the packet
			return;
		}
	}

	cl.lastTransmitTime = cls.realtime;

	// begin a client move command
	MSG_WriteByte( clc_move );

	// save the position for a checksum byte
	checksumIndex = 0;
	if( cls.serverProtocol <= PROTOCOL_VERSION_DEFAULT ) {
		checksumIndex = msg_write.cursize;
		SZ_GetSpace( &msg_write, 1 );
	}

	// let the server know what the last frame we
	// got was, so the next message can be delta compressed
	if( cl_nodelta->integer || !cl.frame.valid /*|| cls.demowaiting*/ ) {
		MSG_WriteLong( -1 ); // no compression
	} else {
		MSG_WriteLong( cl.frame.number );
	}

	// send this and the previous cmds in the message, so
	// if the last packet was dropped, it can be recovered
	cmd = &cl.cmds[( cl.cmdNumber - 2 ) & CMD_MASK];
	MSG_WriteDeltaUsercmd( NULL, cmd );
	MSG_WriteByte( cl.lightlevel );
	oldcmd = cmd;

	cmd = &cl.cmds[( cl.cmdNumber - 1 ) & CMD_MASK];
	MSG_WriteDeltaUsercmd( oldcmd, cmd );
	MSG_WriteByte( cl.lightlevel );
	oldcmd = cmd;

	cmd = &cl.cmds[cl.cmdNumber & CMD_MASK];
	MSG_WriteDeltaUsercmd( oldcmd, cmd );
	MSG_WriteByte( cl.lightlevel );

	if( cls.serverProtocol <= PROTOCOL_VERSION_DEFAULT ) {
		// calculate a checksum over the move commands
		msg_write.data[checksumIndex] = COM_BlockSequenceCRCByte(
			msg_write.data + checksumIndex + 1,
            msg_write.cursize - checksumIndex - 1,
			cls.netchan->outgoing_sequence );
	}

	//
	// deliver the message
	//
	i = cls.netchan->Transmit( cls.netchan, msg_write.cursize, msg_write.data );
	if( i == -1 ) {
		Com_Error( ERR_DISCONNECT, "Connection reset by peer" );
	}

	SCR_AddLagometerOutPacketInfo( i );

	if( cl_showpackets->integer ) {
		Com_Printf( "%i ", i );
	}

	SZ_Clear( &msg_write );
}

/*
=================
CL_SendBatchedCmd
=================
*/
static void CL_SendBatchedCmd( void ) {
	int			i, j, seq, bits;
	int			numCmds, numDups;
	int			totalCmds, totalMsec;
	usercmd_t	*cmd, *oldcmd;
	client_history_t	*history, *oldest;
	byte *patch;

	// see if we are ready to send this packet
	if( !CL_ReadyToSend() ) {
		return;
	}

	// archive this packet
	seq = cls.netchan->outgoing_sequence;
	history = &cl.history[seq & CMD_MASK];
	history->cmdNumber = cl.cmdNumber;
	history->realtime = cls.realtime;	// for ping calculation

	cl.lastTransmitTime = cls.realtime;
	cl.lastTransmitCmdNumber = cl.cmdNumber;

	// begin a client move command
	patch = SZ_GetSpace( &msg_write, 1 );

	// let the server know what the last frame we
	// got was, so the next message can be delta compressed
	if( cl_nodelta->integer || !cl.frame.valid /*|| cls.demowaiting*/ ) {
		*patch = clc_move_nodelta; // no compression
	} else {
		*patch = clc_move_batched;
		MSG_WriteLong( cl.frame.number );
	}

	Cvar_ClampInteger( cl_packetdup, 0, MAX_PACKET_FRAMES - 1 );
	numDups = cl_packetdup->integer;

	*patch |= numDups << SVCMD_BITS;

	// send lightlevel
	MSG_WriteByte( cl.lightlevel );

	// send this and the previous cmds in the message, so
	// if the last packet was dropped, it can be recovered
	oldcmd = NULL;
	totalCmds = 0;
	totalMsec = 0;
	for( i = seq - numDups; i <= seq; i++ ) {
		oldest = &cl.history[( i - 1 ) & CMD_MASK];
		history = &cl.history[i & CMD_MASK];

		numCmds = history->cmdNumber - oldest->cmdNumber;
		if( numCmds >= MAX_PACKET_USERCMDS ) {
			Com_WPrintf( "CL_SendCmd: MAX_PACKET_USERCMDS exceeded\n" );
			SZ_Clear( &msg_write );
			break;
		}
		totalCmds += numCmds;
		MSG_WriteBits( numCmds, 5 );
		for( j = oldest->cmdNumber + 1; j <= history->cmdNumber; j++ ) {
			cmd = &cl.cmds[j & CMD_MASK];
			totalMsec += cmd->msec;
			bits = MSG_WriteDeltaUsercmd_Enhanced( oldcmd, cmd );
			if( cl_showpackets->integer == 3 ) {
				MSG_ShowDeltaUsercmdBits_Enhanced( bits );
			}
			oldcmd = cmd;
		}
	}

	//
	// deliver the message
	//
	i = cls.netchan->Transmit( cls.netchan, msg_write.cursize, msg_write.data );
	if( i == -1 ) {
		Com_Error( ERR_DISCONNECT, "Connection reset by peer" );
	}

	SCR_AddLagometerOutPacketInfo( i );

	if( cl_showpackets->integer == 1 ) {
		Com_Printf( "%i(%i) ", i, totalCmds );
	} else if( cl_showpackets->integer == 2 ) {
		Com_Printf( "%i(%i) ", i, totalMsec );
	} else if( cl_showpackets->integer == 3 ) {
		Com_Printf( " | " );
	}

	SZ_Clear( &msg_write );

}

static void CL_SendUserinfo( void ) {
	cvar_t *var;
	int i;

	if( !cls.userinfo_modified ) {
		return;
	}

	if( cls.userinfo_modified == MAX_PACKET_USERINFOS ) {
		MSG_WriteByte( clc_userinfo );
		MSG_WriteString( Cvar_Userinfo() );
		MSG_FlushTo( &cls.netchan->message );
	} else if( cls.serverProtocol == PROTOCOL_VERSION_Q2PRO ) {
		Com_DPrintf( "Sending %d userinfo updates at frame %u\n",
			cls.userinfo_modified, com_framenum );
		for( i = 0; i < cls.userinfo_modified; i++ ) { 
			var = cls.userinfo_updates[i];
			MSG_WriteByte( clc_userinfo_delta );
			MSG_WriteString( var->name );
			if( var->flags & CVAR_USERINFO ) {
				MSG_WriteString( var->string );
			} else {
				// no longer in userinfo
				MSG_WriteString( NULL );
			}
		}
		MSG_FlushTo( &cls.netchan->message );
	} else {
		Com_WPrintf( "Userinfo update count is %d, should not happen.\n",
			cls.userinfo_modified );
	}
	cls.userinfo_modified = 0;
	
}

void CL_SendCmd( void ) {
	if( cls.state < ca_connected ) {
		return; // not talking to a server
	}

	if( sv_paused->integer ) {
		return;
	}

	// generate usercmds while playing a demo,
	// but do not send them
	if( !cls.netchan ) {
		return;
	}

	if( cls.state < ca_active ) {
		CL_SendUserinfo();

		// just keepalive or update reliable
		if( cls.netchan->ShouldUpdate( cls.netchan ) ) {
			cls.netchan->Transmit( cls.netchan, 0, NULL );
		}
		cl.lastframe = -1;
	    cl.lastTransmitCmdNumber = cl.cmdNumber;
		return;
	}

	if( cl.lastTransmitCmdNumber == cl.cmdNumber ) {
		return; // nothing to send
	}

	// send a userinfo update if needed
	CL_SendUserinfo();

	if( cls.serverProtocol == PROTOCOL_VERSION_Q2PRO ) {
		CL_SendBatchedCmd();
	} else {
		CL_SendDefaultCmd();
	}

    cl.sendPacketNow = qfalse;
}
