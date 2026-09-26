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

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifndef EZQUAKE_CL_SESSION_WIN_H
#define EZQUAKE_CL_SESSION_WIN_H

/* Private Windows transport and native-window operations. */
qbool Session_WinInit(void);
qbool Session_WinValidateWorker(intptr_t pipe, uintptr_t parent, uintptr_t wake);
qbool Session_WinStart(int slot, uintptr_t parent, SDL_Window *window, intptr_t *pipe, intptr_t *process);
qbool Session_WinSend(intptr_t pipe, const void *message, size_t size);
int Session_WinReceive(intptr_t pipe, void *message, size_t size);
void Session_WinClose(intptr_t pipe);
void Session_WinTerminate(intptr_t process);
qbool Session_WinReap(intptr_t process, qbool wait);
uintptr_t Session_WinWindowHandle(SDL_Window *window);
SDL_Window *Session_WinCreateWindow(uintptr_t parent);
void Session_WinGeometry(SDL_Window *window, int width, int height);
void Session_WinSetActive(SDL_Window *window, qbool active);
void Session_WinWait(const intptr_t *processes, int count, unsigned int timeout);

#endif
