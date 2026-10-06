/*
Copyright (C) 2026 unezQuake team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

// Main menu background: renders a map (aerowalk by default) client-side while
// disconnected, with a slow camera tour between the map's spawn points.

#ifndef __MENU_SCENE_H__
#define __MENU_SCENE_H__

void MenuScene_Init(void);

// Called once per frame before rendering, loads the background map when needed.
void MenuScene_Frame(void);

// True if cl.worldmodel belongs to the background scene (also true while it is being loaded).
qbool MenuScene_HasWorld(void);

// True while the background map is loaded and should be drawn instead of the console background.
qbool MenuScene_Active(void);

// Sets up r_refdef for the background view. Returns false if the scene is not active.
qbool MenuScene_SetupView(void);

// Name of the background map ("" if none is loaded).
const char *MenuScene_MapName(void);

// Draws 2D elements belonging to the scene (fade between camera shots).
void MenuScene_Draw2D(void);

// Called when client state is cleared (connecting, loading another map...).
void MenuScene_Invalidate(void);

#endif // __MENU_SCENE_H__
