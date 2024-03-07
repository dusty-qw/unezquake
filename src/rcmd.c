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

#include <string.h>

#include "rcmd.h"

hashtable_t *rcmd_hash;
int rcmd_count;

void Rcmd_HashInit(void)
{
	if (rcmd_hash)
	{
		Hash_Flush(rcmd_hash);
	}
	else
	{
		// 512 commands should be enough for everyone.
		rcmd_hash = Hash_InitTable(512);
	}

	rcmd_count = 0;
}

rcmd_t *Rcmd_New(void)
{
	rcmd_t *rcmd = Q_malloc(sizeof(rcmd_t));
	rcmd->argc = 0;
	return rcmd;
}

void Rcmd_Add(rcmd_t *rcmd, const char *cmd, const char *args)
{
	rcmd->type = args == NULL && rcmd->type != RCMD_WITH_ARGS ? RCMD_WILDCARD : RCMD_WITH_ARGS;

	// A wildcard doesn't have any additional arguments, so we can just
	// return early.
	if (rcmd->type == RCMD_WILDCARD)
	{
		return;
	}

	// If we have a command with args, we will _not_ accept a command
	// without arguments because that defeats the purpose of more
	// granular verification.
	if (rcmd->type == RCMD_WITH_ARGS && !args)
	{
		return;
	}

	// Increment argc and re-allocate argv to make room for the command.
	rcmd->argc++;
	rcmd->argv = Q_realloc(rcmd->argv, rcmd->argc * sizeof(char *));
	rcmd->argv[rcmd->argc - 1] = Q_malloc(strlen(args) + 1);
	rcmd->argv[rcmd->argc - 1] = Q_strdup(args);
}

void Rcmd_Remove(rcmd_t *rcmd)
{
	int i;

	for (i = 0; i < rcmd->argc; i++)
	{
		free(rcmd->argv[i]);
	}

	free(rcmd);
}

qbool Rcmd_IsValidArgs(const rcmd_t *rcmd, const char *args)
{
	int i, arglen, len = strlen(args);

	for (i = 0; i < rcmd->argc; i++)
	{
		arglen = strlen(rcmd->argv[i]);

		if (arglen > len)
		{
			continue;
		}

		if (strncmp(args, rcmd->argv[i], arglen) == 0)
		{
			return true;
		}
	}

	return false;
}
