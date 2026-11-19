/*
 * File-input (AFL) harness for the server game module (qagame,
 * src/game/*.c) restoring a reconnecting client's session from its JSON
 * session file (G_ReadSessionData in g_session.c, via cJSON and
 * src/qcommon/json/json.c).
 *
 * The game module's whole interface is dllEntry(syscall) + vmMain(command,
 * ...). This harness is the engine side of that ABI: it loads the module
 * with vmMain(GAME_INIT) and then connects client 0 as a reconnect
 * (vmMain(GAME_CLIENT_CONNECT, 0, firstTime = qfalse, isBot = qfalse)),
 * which is where the server reads session/client00.json. The fuzz input
 * is that file: byte 0 picks g_gametype (the gametype decides which parts
 * of the session - campaign, restart XP / medals - are restored), the
 * rest is the JSON. Everything else the module asks the engine for comes
 * from fixed values below: cvars take their registered defaults, the map's
 * entity string is a worldspawn and a spawn point, client 0's userinfo has
 * the guid the seeds use. The game data files the module reads (character
 * and animation scripts, configs) are the repo's own etmain/ files, embedded
 * at build time (fz_files, mayhem/tools/embed_files.py), and the module's
 * PC_* script traps go to the engine's real botlib precompiler
 * (src/botlib/l_precomp.c), as in the server.
 *
 * G_Error (trap_Error) ends the process, as the engine aborts the map; so
 * this is a file-input target, one input per process.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/g_local.h"
#include "botlib/botlib.h"

/* botlib precompiler (src/botlib/l_precomp.c), the engine side of the PC traps */
botlib_import_t botimport;
int PC_LoadSourceHandle(const char *filename);
int PC_FreeSourceHandle(int handle);
int PC_ReadTokenHandle(int handle, pc_token_t *pc_token);
int PC_SourceFileAndLine(int handle, char *filename, int *line);
void PC_UnreadLastTokenHandle(int handle);

typedef struct { const char *name; const unsigned char *data; int len; } fz_file_t;
extern const fz_file_t fz_files[];
extern const int       fz_numfiles;

#define FZ_GUID "0123456789ABCDEF0123456789ABCDEF"

Q_EXPORT void dllEntry(intptr_t(QDECL *syscallptr)(intptr_t arg, ...));
Q_EXPORT intptr_t vmMain(intptr_t command, intptr_t arg0, intptr_t arg1, intptr_t arg2, intptr_t arg3, intptr_t arg4, intptr_t arg5, intptr_t arg6);

static const unsigned char *in_json;
static int                 in_json_len;
static int                 in_gametype;
static int                 fz_ms;

/* --- cvars: name -> value, set by registration (default) or by the module --- */
#define FZ_MAX_CVARS 2048
static struct
{
	char name[64];
	char value[MAX_CVAR_VALUE_STRING];
} fz_cvars[FZ_MAX_CVARS];
static int fz_numcvars;

static int fz_cvar_find(const char *name)
{
	int i;

	for (i = 0; i < fz_numcvars; i++)
	{
		if (!Q_stricmp(fz_cvars[i].name, name))
		{
			return i;
		}
	}
	return -1;
}

static int fz_cvar_set(const char *name, const char *value, qboolean onlyIfNew)
{
	int i = fz_cvar_find(name);

	if (i < 0)
	{
		if (fz_numcvars == FZ_MAX_CVARS)
		{
			abort();
		}
		i = fz_numcvars++;
		Q_strncpyz(fz_cvars[i].name, name, sizeof(fz_cvars[i].name));
	}
	else if (onlyIfNew)
	{
		return i;
	}
	Q_strncpyz(fz_cvars[i].value, value ? value : "", sizeof(fz_cvars[i].value));
	return i;
}

static const char *fz_cvar_get(const char *name)
{
	int i = fz_cvar_find(name);
	return i < 0 ? "" : fz_cvars[i].value;
}

static void fz_fill_vmcvar(vmCvar_t *vc, int idx)
{
	vc->handle            = idx;
	vc->modificationCount = 0;
	vc->value             = (float)atof(fz_cvars[idx].value);
	vc->integer           = atoi(fz_cvars[idx].value);
	Q_strncpyz(vc->string, fz_cvars[idx].value, sizeof(vc->string));
}

/* --- files: session/client00.json is the input, the embedded etmain files
 * are the game data; writes go nowhere --- */
#define FZ_MAX_HANDLES 64
static struct
{
	const unsigned char *data;
	int len, pos;
} fz_handles[FZ_MAX_HANDLES];

static int fz_fopen(const char *name, fileHandle_t *f)
{
	const unsigned char *data = NULL;
	int                  len  = 0, i;

	*f = 0;
	if (!Q_stricmp(name, "session/client00.json"))
	{
		data = in_json;
		len  = in_json_len;
	}
	else
	{
		for (i = 0; i < fz_numfiles; i++)
		{
			if (!Q_stricmp(name, fz_files[i].name))
			{
				data = fz_files[i].data;
				len  = fz_files[i].len;
				break;
			}
		}
		if (i == fz_numfiles)
		{
			return -1;
		}
	}
	for (i = 1; i < FZ_MAX_HANDLES; i++)
	{
		if (!fz_handles[i].data)
		{
			fz_handles[i].data = data ? data : (const unsigned char *)"";
			fz_handles[i].len  = len;
			fz_handles[i].pos  = 0;
			*f                 = i;
			return len;
		}
	}
	return -1;
}

static int fz_fread(void *buffer, int len, fileHandle_t f)
{
	int n;

	if (f < 1 || f >= FZ_MAX_HANDLES || !fz_handles[f].data)
	{
		return 0;
	}
	n = fz_handles[f].len - fz_handles[f].pos;
	if (n > len)
	{
		n = len;
	}
	memcpy(buffer, fz_handles[f].data + fz_handles[f].pos, (size_t)n);
	fz_handles[f].pos += n;
	return n;
}

static void fz_fclose(fileHandle_t f)
{
	if (f >= 1 && f < FZ_MAX_HANDLES)
	{
		fz_handles[f].data = NULL;
	}
}

/* botlib import table: memory from libc, files from the table above */
static void QDECL fz_bl_print(int type, char *fmt, ...)
{
	(void)type;
	(void)fmt;
}

static void *fz_bl_alloc(int size)
{
	return malloc(size > 0 ? (size_t)size : 1);
}

static int fz_bl_fopen(const char *qpath, fileHandle_t *f, fsMode_t mode)
{
	if (mode != FS_READ)
	{
		*f = 0;
		return -1;
	}
	return fz_fopen(qpath, f);
}

/* --- the map's entity string: a worldspawn with map coordinates (as ET maps
 * carry for the command map) and a spawn point --- */
static const char *const fz_entity_tokens[] =
{
	"{", "classname", "worldspawn", "mapcoordsmins", "-4096 4096", "mapcoordsmaxs", "4096 -4096", "}",
	"{", "classname", "info_player_deathmatch", "origin", "0 0 64", "}",
};
static int               fz_entity_tok;

static intptr_t QDECL fz_syscall(intptr_t cmd, ...)
{
	va_list  ap;
	intptr_t a[12];
	int      i;

	va_start(ap, cmd);
	for (i = 0; i < 12; i++)
	{
		a[i] = va_arg(ap, intptr_t);
		if (a[i] == VM_CALL_END)
		{
			break;
		}
	}
	va_end(ap);

	switch (cmd)
	{
	case G_PRINT:
		return 0;
	case G_ERROR:
		fprintf(stderr, "G_Error: %s", (const char *)a[0]);
		exit(1);
	case G_MILLISECONDS:
		return fz_ms += 50;
	case G_CVAR_REGISTER:
	{
		int idx = fz_cvar_set((const char *)a[1], (const char *)a[2], qtrue);
		if (a[0])
		{
			fz_fill_vmcvar((vmCvar_t *)a[0], idx);
		}
		return 0;
	}
	case G_CVAR_UPDATE:
	{
		vmCvar_t *vc = (vmCvar_t *)a[0];
		if (vc->handle >= 0 && vc->handle < fz_numcvars)
		{
			fz_fill_vmcvar(vc, vc->handle);
		}
		return 0;
	}
	case G_CVAR_SET:
		fz_cvar_set((const char *)a[0], (const char *)a[1], qfalse);
		return 0;
	case G_CVAR_VARIABLE_INTEGER_VALUE:
		return atoi(fz_cvar_get((const char *)a[0]));
	case G_CVAR_VARIABLE_STRING_BUFFER:
	case G_CVAR_LATCHEDVARIABLESTRINGBUFFER:
		Q_strncpyz((char *)a[1], fz_cvar_get((const char *)a[0]), (int)a[2]);
		return 0;
	case G_ARGC:
		return 0;
	case G_ARGV:
		if (a[2] > 0)
		{
			((char *)a[1])[0] = 0;
		}
		return 0;
	case G_FS_FOPEN_FILE:
		if ((int)a[2] == FS_READ)
		{
			return fz_fopen((const char *)a[0], (fileHandle_t *)a[1]);
		}
		*(fileHandle_t *)a[1] = FZ_MAX_HANDLES; /* a sink for written files */
		return 0;
	case G_FS_READ:
		return fz_fread((void *)a[0], (int)a[1], (fileHandle_t)a[2]);
	case G_FS_WRITE:
		return (int)a[1];
	case G_FS_FCLOSE_FILE:
		fz_fclose((fileHandle_t)a[0]);
		return 0;
	case G_FS_RENAME:
		return 0;
	case BOTLIB_PC_LOAD_SOURCE:
		return PC_LoadSourceHandle((const char *)a[0]);
	case BOTLIB_PC_FREE_SOURCE:
		return PC_FreeSourceHandle((int)a[0]);
	case BOTLIB_PC_READ_TOKEN:
		return PC_ReadTokenHandle((int)a[0], (pc_token_t *)a[1]);
	case BOTLIB_PC_SOURCE_FILE_AND_LINE:
		return PC_SourceFileAndLine((int)a[0], (char *)a[1], (int *)a[2]);
	case BOTLIB_PC_UNREAD_TOKEN:
		PC_UnreadLastTokenHandle((int)a[0]);
		return 0;
	case G_FS_GETFILELIST:
		return 0;
	case G_GET_CONFIGSTRING:
	case G_GET_SERVERINFO:
	{
		char *buf = (char *)(cmd == G_GET_SERVERINFO ? a[0] : a[1]);
		int  size = (int)(cmd == G_GET_SERVERINFO ? a[1] : a[2]);
		if (size > 0)
		{
			Q_strncpyz(buf, cmd == G_GET_SERVERINFO ? "\\mapname\\fuzz\\sv_hostname\\fuzz" : "", size);
		}
		return 0;
	}
	case G_GET_USERINFO:
		Q_strncpyz((char *)a[1],
		           "\\name\\fuzzer\\cl_guid\\" FZ_GUID "\\ip\\localhost\\protocol\\84\\rate\\25000\\snaps\\20",
		           (int)a[2]);
		return 0;
	case G_GET_ENTITY_TOKEN:
		if (fz_entity_tok < (int)ARRAY_LEN(fz_entity_tokens))
		{
			Q_strncpyz((char *)a[0], fz_entity_tokens[fz_entity_tok++], (int)a[1]);
			return qtrue;
		}
		return qfalse;
	case G_REAL_TIME:
		memset((void *)a[0], 0, sizeof(qtime_t));
		return 0;
	case G_TRACE:
	case G_TRACECAPSULE:
	{
		trace_t *tr = (trace_t *)a[0];
		memset(tr, 0, sizeof(*tr));
		tr->fraction = 1.0f;
		VectorCopy((const float *)a[4], tr->endpos);
		tr->entityNum = ENTITYNUM_NONE;
		return 0;
	}
	case G_GET_USERCMD:
		memset((void *)a[1], 0, sizeof(usercmd_t));
		return 0;
	default:
		return 0;
	}
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
	char gt[16];

	if (size < 1 || size > 5242880)
	{
		return -1; /* Q_FSReadJsonFrom refuses files over 5 MiB */
	}
	{
		/* the gametypes a dedicated server accepts without campaign files:
		 * objective, stopwatch, last man standing, map voting */
		static const int gts[] = { GT_WOLF, GT_WOLF_STOPWATCH, GT_WOLF_LMS, GT_WOLF_MAPVOTE };
		in_gametype = gts[data[0] % ARRAY_LEN(gts)];
	}
	in_json     = data + 1;
	in_json_len = (int)size - 1;

	Com_sprintf(gt, sizeof(gt), "%d", in_gametype);
	fz_cvar_set("g_gametype", gt, qfalse);
	fz_cvar_set("sv_maxclients", "8", qfalse);
	fz_cvar_set("mapname", "fuzz", qfalse);

	botimport.Print         = fz_bl_print;
	botimport.GetMemory     = fz_bl_alloc;
	botimport.FreeMemory    = free;
	botimport.HunkAlloc     = fz_bl_alloc;
	botimport.FS_FOpenFile  = fz_bl_fopen;
	botimport.FS_Read       = fz_fread;
	botimport.FS_FCloseFile = fz_fclose;

	dllEntry(fz_syscall);
	/* as SV_InitGameVM: levelTime, seed, restart, legacy server, server version */
	vmMain(GAME_INIT, 0, 0, qfalse, qtrue, etlegacy_version_int, 0, 0);
	/* SV_DirectConnect of a client that was on the server before the map change */
	vmMain(GAME_CLIENT_CONNECT, 0, qfalse, qfalse, 0, 0, 0, 0);
	/* the restored session is written back at the next map change */
	vmMain(GAME_SHUTDOWN, qtrue, 0, 0, 0, 0, 0, 0);
	return 0;
}
