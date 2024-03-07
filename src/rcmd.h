/*
Copyright (C) 2024 Oscar Linderholm

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

#include "hash.h"
#include "quakedef.h"

#define RCMD_WILDCARD	0x01
#define RCMD_WITH_ARGS	0x02

typedef struct rcmd {
	int type;
	int argc;
	char **argv;
} rcmd_t;

void Rcmd_HashInit(void);
rcmd_t *Rcmd_New(void);
void Rcmd_Add(rcmd_t *rcmd, const char *cmd, const char *args);
void Rcmd_Remove(rcmd_t *rcmd);
qbool Rcmd_IsValidArgs(const rcmd_t *rcmd, const char *args);

// rcmd_hash contains the remote command lookup table used by the whitelist
// function, which determines whether the command should be allowed or not.
extern hashtable_t *rcmd_hash;

// rcmd_count keeps track of how many elements there are in rcmd_hash.
extern int rcmd_count;
