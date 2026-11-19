/*
 * libFuzzer harness for the botlib script precompiler and lexer
 * (src/botlib/l_precomp.c, l_script.c).
 *
 * This is the lexer behind every .menu/.hud/.weap/.campaign/.arena/bot script
 * the UI, cgame and game modules read: they open a file through the engine
 * trap PC_LoadSourceHandle and pull tokens with PC_ReadTokenHandle, i.e. the
 * same botlib_export_t entry points used here. The script is a file a server
 * can ship in a pk3, so its bytes are untrusted.
 *
 * The fuzz input is served as the file "fuzz.menu" through the botlib import
 * table (botimport); any other file (an #include) does not exist.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcommon/q_shared.h"
#include "botlib/botlib.h"
#include "botlib/be_interface.h"
#include "botlib/l_memory.h"
#include "botlib/l_script.h"
#include "botlib/l_precomp.h"

botlib_import_t botimport;

#define FUZZ_FILE "fuzz.menu"

static const unsigned char *in_data;
static int                 in_size;
static int                 in_pos[64];
static int                 in_open[64];

static void QDECL fz_Print(int type, char *fmt, ...)
{
	(void)type;
	(void)fmt;
}

static void *fz_GetMemory(int size)
{
	return malloc(size > 0 ? (size_t)size : 1);
}

static void fz_FreeMemory(void *p)
{
	free(p);
}

static int fz_FS_FOpenFile(const char *qpath, fileHandle_t *f, fsMode_t mode)
{
	int i;

	*f = 0;
	if (mode != FS_READ || Q_stricmp(qpath, FUZZ_FILE))
	{
		return -1;
	}
	for (i = 1; i < 64; i++)
	{
		if (!in_open[i])
		{
			in_open[i] = 1;
			in_pos[i]  = 0;
			*f         = i;
			return in_size;
		}
	}
	return -1;
}

static int fz_FS_Read(void *buffer, int len, fileHandle_t f)
{
	int n;

	if (f < 1 || f >= 64 || !in_open[f])
	{
		return 0;
	}
	n = in_size - in_pos[f];
	if (n > len)
	{
		n = len;
	}
	memcpy(buffer, in_data + in_pos[f], (size_t)n);
	in_pos[f] += n;
	return n;
}

static void fz_FS_FCloseFile(fileHandle_t f)
{
	if (f >= 1 && f < 64)
	{
		in_open[f] = 0;
	}
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	botimport.Print         = fz_Print;
	botimport.GetMemory     = fz_GetMemory;
	botimport.FreeMemory    = fz_FreeMemory;
	botimport.HunkAlloc     = fz_GetMemory;
	botimport.FS_FOpenFile  = fz_FS_FOpenFile;
	botimport.FS_Read       = fz_FS_Read;
	botimport.FS_FCloseFile = fz_FS_FCloseFile;
	return 0;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
	pc_token_t tok;
	int        handle, n = 0;

	if (size > (1u << 20))
	{
		return -1;
	}
	in_data = data;
	in_size = (int)size;
	memset(in_open, 0, sizeof(in_open));

	handle = PC_LoadSourceHandle(FUZZ_FILE);
	if (!handle)
	{
		return 0;
	}
	/* read the whole token stream, exercising unread like the menu parser does */
	while (PC_ReadTokenHandle(handle, &tok))
	{
		if ((++n & 7) == 0)
		{
			PC_UnreadLastTokenHandle(handle);
			if (!PC_ReadTokenHandle(handle, &tok))
			{
				break;
			}
		}
		{
			char fname[MAX_QPATH];
			int  line;
			PC_SourceFileAndLine(handle, fname, &line);
		}
	}
	PC_FreeSourceHandle(handle);
	return 0;
}
