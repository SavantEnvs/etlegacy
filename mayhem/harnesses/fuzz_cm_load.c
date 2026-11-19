/*
 * File-input (AFL) harness for the collision-map BSP loader
 * (src/qcommon/cm_load.c, cm_patch.c, cm_polylib.c, cm_trace.c, cm_test.c,
 * and the MD4 map checksum in md4.c).
 *
 * The server loads maps/<name>.bsp with CM_LoadMap when it spawns a map, and
 * a client does the same for the map a server tells it to load (the .bsp
 * arrives in a downloaded pk3). This harness serves the input as that file,
 * loads it the way SV_SpawnServer / CL_InitCGame do, then queries the world
 * as the server's world code and the game do: inline models, bounds, point
 * contents, box traces, PVS and areas.
 *
 * Com_Error(ERR_DROP) on a malformed map ends the process (the engine aborts
 * the map load), so this is a file-input target; see fz_common.c.
 */
#include <stdlib.h>
#include <string.h>

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "qcommon/cm_public.h"
#include "game/surfaceflags.h"

#define FUZZ_MAP "maps/fuzz.bsp"

static const unsigned char *in_data;
static int                 in_size;

/* --- the engine services CM_* uses (normally common.c, files.c, cvar.c) --- */

static cvar_t fz_cvars[32];
static int    fz_numcvars;
static cvar_t fz_sv_running;
cvar_t        *com_sv_running = &fz_sv_running;

cvar_t *Cvar_Get(const char *var_name, const char *var_value, cvarFlags_t flags)
{
	int i;

	for (i = 0; i < fz_numcvars; i++)
	{
		if (!strcmp(fz_cvars[i].name, var_name))
		{
			return &fz_cvars[i];
		}
	}
	if (fz_numcvars == 32)
	{
		abort();
	}
	fz_cvars[i].name    = (char *)var_name;
	fz_cvars[i].string  = (char *)var_value;
	fz_cvars[i].flags   = flags;
	fz_cvars[i].value   = (float)atof(var_value);
	fz_cvars[i].integer = atoi(var_value);
	fz_numcvars++;
	return &fz_cvars[i];
}

int FS_ReadFile(const char *qpath, void **buffer)
{
	byte *b;

	if (Q_stricmp(qpath, FUZZ_MAP))
	{
		if (buffer)
		{
			*buffer = NULL;
		}
		return -1;
	}
	if (!buffer)
	{
		return in_size;
	}
	/* FS_ReadFile allocates len + 1 and NUL-terminates */
	b = (byte *)malloc((size_t)in_size + 1);
	memcpy(b, in_data, (size_t)in_size);
	b[in_size] = 0;
	*buffer    = b;
	return in_size;
}

/* an optional maps/<name>.ent entity override: there is none */
long FS_FOpenFileRead(const char *fileName, fileHandle_t *file, qboolean uniqueFILE)
{
	(void)fileName;
	(void)uniqueFILE;
	*file = 0;
	return -1;
}

int FS_Read(void *buffer, int len, fileHandle_t f)
{
	(void)buffer;
	(void)len;
	(void)f;
	return 0;
}

void FS_FCloseFile(fileHandle_t f)
{
	(void)f;
}

void FS_FreeFile(void *buffer)
{
	free(buffer);
}

void *Hunk_Alloc(size_t size, ha_pref preference)
{
	(void)preference;
	/* the hunk is zero-filled; one process per input, so nothing is freed */
	return calloc(1, size ? size : 1);
}

void *Z_Malloc(size_t size)
{
	return calloc(1, size ? size : 1);
}

void Z_Free(void *ptr)
{
	free(ptr);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
	unsigned int checksum = 0;
	int          i, n;
	vec3_t       mins, maxs, start, end, origin = { 0, 0, 0 };
	trace_t      tr;

	if (size > (64u << 20))
	{
		return -1;
	}
	in_data = data;
	in_size = (int)size;

	CM_LoadMap(FUZZ_MAP, qfalse, &checksum);

	/* SV_SpawnServer / SV_ClearWorld */
	n = CM_NumInlineModels();
	for (i = 0; i < n && i < 64; i++)
	{
		clipHandle_t h = CM_InlineModel(i);
		CM_ModelBounds(h, mins, maxs);
		CM_TransformedPointContents(origin, h, origin, origin);
	}
	CM_EntityString();

	/* G_/SV_ traces and contents across the world bounds */
	if (n > 0)
	{
		CM_ModelBounds(CM_InlineModel(0), mins, maxs);
		for (i = 0; i < 8; i++)
		{
			vec3_t bmins = { -16, -16, -24 }, bmaxs = { 16, 16, 32 };
			start[0] = (i & 1) ? mins[0] : maxs[0];
			start[1] = (i & 2) ? mins[1] : maxs[1];
			start[2] = (i & 4) ? mins[2] : maxs[2];
			end[0]   = mins[0] + maxs[0] - start[0];
			end[1]   = mins[1] + maxs[1] - start[1];
			end[2]   = mins[2] + maxs[2] - start[2];
			CM_PointContents(start, 0);
			CM_BoxTrace(&tr, start, end, NULL, NULL, 0, CONTENTS_SOLID, qfalse);
			CM_BoxTrace(&tr, start, end, bmins, bmaxs, 0, (CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_BODY), qtrue);
			{
				int leaf    = CM_PointLeafnum(start);
				int cluster = CM_LeafCluster(leaf);
				int area    = CM_LeafArea(leaf);
				CM_ClusterPVS(cluster);
				CM_AreasConnected(area, 0);
			}
		}
	}
	(void)checksum;
	return 0;
}
