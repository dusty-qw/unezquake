/*
Copyright (C) 2011 ezQuake team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
	Options Menu module
	$Id: menu_options.h,v 1.4 2007-03-19 13:23:20 johnnycz Exp $
*/

// <interface for menu.c>
// initializes the "Demo" menu on client startup
void Menu_Options_Init(void);

// process key press that belongs to demo menu
void Menu_Options_Key(int key, wchar unichar);

// process request to draw the demo menu
void Menu_Options_Draw (void);

qbool Menu_Options_IsBindingKey (void);

// process mouse move
qbool Menu_Options_Mouse_Event(const mouse_state_t *);
// </interface>

// <interface for menu_ui_bridge.c>
// the settings pages, in the order the options menu shows them as tabs
int Menu_Options_PageCount(void);
const char *Menu_Options_PageName(int index);
struct settings_page_s *Menu_Options_Page(int index);

// directories used by the "Import config" and "Load Script" actions
const char *Menu_Options_ConfigsDir(void);
const char *Menu_Options_ScriptsDir(void);

void MOpt_ImportConfig(void);
void MOpt_ExportConfig(void);
void MOpt_LoadScript(void);
// </interface>
