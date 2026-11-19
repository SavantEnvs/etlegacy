/*
 * Known-answer tests for the ET: Legacy code the fuzz targets exercise
 * (upstream has no unit tests). Built by mayhem/build.sh with the normal
 * flags, run by mayhem/test.sh, which checks that every named test printed
 * "PASS <name>".
 *
 *   md4_*        Com_BlockChecksum against the RFC 1320 test vectors
 *   huff_*       MSG_* writes, Huff_Compress / Huff_Decompress round trip
 *   delta_*      entityState_t / playerState_t / usercmd_t delta round trips
 *   tga_* bmp_* pcx_*  decoded pixels of the committed seed images
 *   pc_*         botlib precompiler: macros, #if, $evalint, strings
 *   cm_*         BSP collision map: contents, traces, checksum of box.bsp
 *
 * usage: etl_selftest <dir with the seed files>
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "qcommon/cm_public.h"
#include "game/surfaceflags.h"
#include "renderercommon/tr_common.h"
#include "botlib/botlib.h"

qboolean R_LoadTGA(imageData_t *data, byte **pic, int *width, int *height, byte alphaByte);
qboolean R_LoadBMP(imageData_t *data, byte **pic, int *width, int *height, byte alphaByte);
qboolean R_LoadPCX(imageData_t *data, byte **pic, int *width, int *height, byte alphaByte);
int PC_LoadSourceHandle(const char *filename);
int PC_FreeSourceHandle(int handle);
int PC_ReadTokenHandle(int handle, pc_token_t *pc_token);

static int failures;

static void check(const char *name, int ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok)
	{
		failures++;
	}
}

static unsigned char *slurp(const char *dir, const char *name, int *len)
{
	char          path[1024];
	FILE          *f;
	unsigned char *b;
	long          n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (!f)
	{
		*len = 0;
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	if (fread(b, 1, (size_t)n, f) != (size_t)n)
	{
		n = 0;
	}
	fclose(f);
	b[n] = 0;
	*len = (int)n;
	return b;
}

/* ---------------------------------------------------------------- engine services */
cvar_t        *cl_shownet = NULL;
static cvar_t fz_sv_running;
cvar_t        *com_sv_running = &fz_sv_running;
static cvar_t fz_cvars[32];
static int    fz_numcvars;

cvar_t *Cvar_Get(const char *name, const char *value, cvarFlags_t flags)
{
	int i;

	for (i = 0; i < fz_numcvars; i++)
	{
		if (!strcmp(fz_cvars[i].name, name))
		{
			return &fz_cvars[i];
		}
	}
	fz_cvars[i].name    = (char *)name;
	fz_cvars[i].string  = (char *)value;
	fz_cvars[i].flags   = flags;
	fz_cvars[i].value   = (float)atof(value);
	fz_cvars[i].integer = atoi(value);
	fz_numcvars++;
	return &fz_cvars[i];
}

static const unsigned char *file_data;
static int                 file_len;
static const char          *file_name;

int FS_ReadFile(const char *qpath, void **buffer)
{
	byte *b;

	if (!file_name || Q_stricmp(qpath, file_name))
	{
		if (buffer)
		{
			*buffer = NULL;
		}
		return -1;
	}
	b = malloc((size_t)file_len + 1);
	memcpy(b, file_data, (size_t)file_len);
	b[file_len] = 0;
	*buffer     = b;
	return file_len;
}

void FS_FreeFile(void *buffer)
{
	free(buffer);
}

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

void *Hunk_Alloc(size_t size, ha_pref preference)
{
	(void)preference;
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

/* renderer refimport */
refimport_t ri;

static void QDECL ri_printf(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

static void *ri_zmalloc(int bytes)
{
	return calloc(1, (size_t)(bytes > 0 ? bytes : 1));
}

static void *ri_temp(size_t size)
{
	return malloc(size ? size : 1);
}

static void *imgbuf;

void *R_GetImageBuffer(int size, bufferMemType_t bufferType, const char *filename)
{
	(void)bufferType;
	(void)filename;
	free(imgbuf);
	imgbuf = malloc((size_t)size);
	return imgbuf;
}

static void ri_free(void *p)
{
	if (p == imgbuf)
	{
		imgbuf = NULL;
	}
	free(p);
}

static void ri_fsfree(void *p)
{
	(void)p;
}

/* botlib import */
botlib_import_t botimport;

static void QDECL bl_print(int type, char *fmt, ...)
{
	(void)type;
	(void)fmt;
}

static void *bl_alloc(int size)
{
	return malloc((size_t)(size > 0 ? size : 1));
}

static int bl_pos;

static int bl_fopen(const char *qpath, fileHandle_t *f, fsMode_t mode)
{
	*f = 0;
	if (mode != FS_READ || !file_name || Q_stricmp(qpath, file_name))
	{
		return -1;
	}
	*f     = 1;
	bl_pos = 0;
	return file_len;
}

static int bl_read(void *buffer, int len, fileHandle_t f)
{
	int n = file_len - bl_pos;

	(void)f;
	if (n > len)
	{
		n = len;
	}
	memcpy(buffer, file_data + bl_pos, (size_t)n);
	bl_pos += n;
	return n;
}

static void bl_fclose(fileHandle_t f)
{
	(void)f;
}

/* ---------------------------------------------------------------- tests */
static void test_md4(void)
{
	char v80[81];

	check("md4_empty", Com_BlockChecksum((void *)"", 0) == 0xc6f640b7u);
	check("md4_abc", Com_BlockChecksum((void *)"abc", 3) == 0x5da10e2eu);
	memcpy(v80, "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890", 80);
	check("md4_80", Com_BlockChecksum(v80, 80) == 0xe5c1f1acu);
}

static void test_huff(void)
{
	static byte buf[MAX_MSGLEN], raw[256];
	msg_t       msg;
	int         ok, i;

	/* message coding (static Huffman tables for non-OOB messages) */
	MSG_Init(&msg, buf, sizeof(buf));
	MSG_Bitstream(&msg);
	MSG_WriteLong(&msg, 0x12345678);
	MSG_WriteByte(&msg, 200);
	MSG_WriteShort(&msg, -1234);
	MSG_WriteString(&msg, "et: legacy");
	MSG_WriteBits(&msg, 5, 3);
	MSG_WriteFloat(&msg, 3.5f);

	MSG_BeginReading(&msg);
	ok = MSG_ReadLong(&msg) == 0x12345678;
	ok = ok && MSG_ReadByte(&msg) == 200;
	ok = ok && MSG_ReadShort(&msg) == -1234;
	ok = ok && !strcmp(MSG_ReadString(&msg), "et: legacy");
	ok = ok && MSG_ReadBits(&msg, 3) == 5;
	ok = ok && MSG_ReadFloat(&msg) == 3.5f;
	check("msg_roundtrip", ok);
	MSG_ReadLong(&msg);
	check("msg_overread", msg.readcount > msg.cursize);

	/* adaptive Huffman of a whole packet (netchan payload) */
	for (i = 0; i < (int)sizeof(raw); i++)
	{
		raw[i] = (byte)((i * i) % 7 + 'a');
	}
	MSG_InitOOB(&msg, buf, sizeof(buf));
	MSG_WriteData(&msg, raw, sizeof(raw));
	Huff_Compress(&msg, 0);
	check("huff_compresses", msg.cursize > 0 && msg.cursize < (int)sizeof(raw));
	Huff_Decompress(&msg, 0);
	check("huff_roundtrip", msg.cursize == (int)sizeof(raw) && !memcmp(buf, raw, sizeof(raw)));
}

static void test_delta(void)
{
	static byte   buf[MAX_MSGLEN];
	msg_t         msg;
	entityState_t from, to, out;
	playerState_t pf, pt, po;
	usercmd_t     cf, ct, co;

	memset(&from, 0, sizeof(from));
	memset(&to, 0, sizeof(to));
	to.number    = 42;
	to.eType     = 3;
	to.pos.trBase[0] = 128.f;
	to.pos.trBase[2] = -64.f;
	to.modelindex    = 7;
	to.angles[1]     = 90.f;

	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteDeltaEntity(&msg, &from, &to, qfalse);
	MSG_BeginReading(&msg);
	memset(&out, 0xff, sizeof(out));
	MSG_ReadDeltaEntity(&msg, &from, &out, MSG_ReadBits(&msg, GENTITYNUM_BITS));
	check("delta_entity", out.number == 42 && out.eType == 3 && out.pos.trBase[0] == 128.f &&
	      out.pos.trBase[2] == -64.f && out.modelindex == 7 && out.angles[1] == 90.f);

	MSG_Init(&msg, buf, sizeof(buf));
	to.number = 42;
	MSG_WriteBits(&msg, to.number, GENTITYNUM_BITS);
	MSG_WriteBits(&msg, 1, 1); /* remove */
	MSG_BeginReading(&msg);
	MSG_ReadDeltaEntity(&msg, &from, &out, MSG_ReadBits(&msg, GENTITYNUM_BITS));
	check("delta_entity_remove", out.number == MAX_GENTITIES - 1);

	memset(&pf, 0, sizeof(pf));
	memset(&pt, 0, sizeof(pt));
	pt.commandTime = 5000;
	pt.origin[0]   = 10.f;
	pt.velocity[2] = 270.f;
	pt.weapon      = 3;
	pt.stats[0]    = 100;
	pt.ammo[3]     = 30;
	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteDeltaPlayerstate(&msg, &pf, &pt);
	MSG_BeginReading(&msg);
	MSG_ReadDeltaPlayerstate(&msg, &pf, &po);
	check("delta_playerstate", po.commandTime == 5000 && po.origin[0] == 10.f && po.velocity[2] == 270.f &&
	      po.weapon == 3 && po.stats[0] == 100 && po.ammo[3] == 30);

	memset(&cf, 0, sizeof(cf));
	memset(&ct, 0, sizeof(ct));
	ct.serverTime  = 1000;
	ct.angles[0]   = 1234;
	ct.forwardmove = 127;
	ct.buttons     = 1;
	ct.weapon      = 8;
	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteDeltaUsercmdKey(&msg, 0x5a5a, &cf, &ct);
	MSG_BeginReading(&msg);
	memset(&co, 0, sizeof(co));
	MSG_ReadDeltaUsercmdKey(&msg, 0x5a5a, &cf, &co);
	check("delta_usercmd", co.serverTime == 1000 && co.angles[0] == 1234 && co.forwardmove == 127 &&
	      co.buttons == 1 && co.weapon == 8);
}

static void test_image(const char *dir, const char *name, const char *seed,
                       qboolean (*loader)(imageData_t *, byte **, int *, int *, byte),
                       int ew, int eh, int px, int py, const byte expect[4])
{
	imageData_t img;
	byte        *pic = NULL;
	int         w = 0, h = 0, len, ok;
	char        tname[64];
	byte        *file = slurp(dir, seed, &len);

	img.buffer.b = file; /* the loaders advance img.buffer.b */
	img.size     = len;
	img.name     = seed;
	ok           = img.buffer.b && loader(&img, &pic, &w, &h, 0xff) && pic;
	snprintf(tname, sizeof(tname), "%s_decode", name);
	check(tname, ok && w == ew && h == eh);
	snprintf(tname, sizeof(tname), "%s_pixel", name);
	check(tname, ok && w == ew && h == eh && !memcmp(pic + 4 * (py * w + px), expect, 4));
	if (ok && getenv("SELFTEST_VERBOSE"))
	{
		byte *q = pic + 4 * (py * w + px);
		printf("  %s (%d,%d) = %d %d %d %d\n", name, px, py, q[0], q[1], q[2], q[3]);
	}
	if (pic && pic != imgbuf)
	{
		free(pic);
	}
	free(imgbuf);
	imgbuf = NULL;
	free(file);
}

static void test_pc(void)
{
	static const char script[] =
		"#define TWO 2\n"
		"#define ADD(a,b) ((a)+(b))\n"
		"#if ADD(TWO, 3) == 5\n"
		"itemDef { rect $evalint(ADD(TWO,3) * 4) \"quoted text\" 1.5 }\n"
		"#else\n"
		"wrong\n"
		"#endif\n";
	const char *expect[] = { "itemDef", "{", "rect", "20", "quoted text", "1.5", "}" };
	pc_token_t tok;
	int        h, n = 0, ok = 1;

	file_name = "test.menu";
	file_data = (const unsigned char *)script;
	file_len  = (int)strlen(script);
	h         = PC_LoadSourceHandle("test.menu");
	check("pc_load", h > 0);
	while (h > 0 && PC_ReadTokenHandle(h, &tok))
	{
		if (n >= (int)ARRAY_LEN(expect) || strcmp(tok.string, expect[n]))
		{
			ok = 0;
		}
		if (n == 3 && tok.intvalue != 20)
		{
			ok = 0;
		}
		n++;
	}
	check("pc_tokens", ok && n == (int)ARRAY_LEN(expect));
	if (h > 0)
	{
		PC_FreeSourceHandle(h);
	}
	file_name = NULL;
}

static void test_cm(const char *dir)
{
	unsigned int checksum = 0;
	int          len;
	trace_t      tr;
	vec3_t       origin = { 0, 0, 0 }, out = { 200, 0, 0 }, start = { -200, 0, 0 }, mins, maxs;
	unsigned char *b = slurp(dir, "box.bsp", &len);

	file_name = "maps/box.bsp";
	file_data = b;
	file_len  = len;
	CM_LoadMap("maps/box.bsp", qfalse, &checksum);
	check("cm_checksum", b && checksum == (unsigned int)LittleLong(Com_BlockChecksum(b, (size_t)len)));
	check("cm_models", CM_NumInlineModels() == 1);
	CM_ModelBounds(CM_InlineModel(0), mins, maxs);
	check("cm_bounds", mins[0] == -513.f && maxs[2] == 513.f); /* CMod_LoadSubmodels spreads by 1 */
	check("cm_contents_inside", CM_PointContents(origin, 0) == CONTENTS_SOLID);
	check("cm_contents_outside", CM_PointContents(out, 0) == 0);
	CM_BoxTrace(&tr, start, out, NULL, NULL, 0, CONTENTS_SOLID, qfalse);
	check("cm_trace", tr.fraction > 0.33f && tr.fraction < 0.35f && fabsf(tr.endpos[0] + 64.f) < 0.5f &&
	      tr.plane.normal[0] == -1.f);
	check("cm_entities", strstr(CM_EntityString(), "info_player_deathmatch") != NULL);
	file_name = NULL;
	free(b);
}

int main(int argc, char **argv)
{
	static const byte tga_px[4] = { 94, 54, 14, 134 };
	static const byte bmp_px[4] = { 172, 141, 110, 255 }; /* bottom-up rows, BGR -> RGBA */
	static const byte pcx_px[4] = { 54, 55, 56, 255 };   /* index 18 -> palette[54..56] */
	const char        *seeds    = argc > 1 ? argv[1] : "mayhem";
	char              d[1024];

	ri.Printf                  = ri_printf;
	ri.Z_Malloc                = ri_zmalloc;
	ri.Free                    = ri_free;
	ri.Hunk_AllocateTempMemory = ri_temp;
	ri.Hunk_FreeTempMemory     = ri_free;
	ri.FS_FreeFile             = ri_fsfree;
	botimport.Print            = bl_print;
	botimport.GetMemory        = bl_alloc;
	botimport.FreeMemory       = free;
	botimport.HunkAlloc        = bl_alloc;
	botimport.FS_FOpenFile     = bl_fopen;
	botimport.FS_Read          = bl_read;
	botimport.FS_FCloseFile    = bl_fclose;

	test_md4();
	test_huff();
	test_delta();
	snprintf(d, sizeof(d), "%s/image_tga/testsuite", seeds);
	test_image(d, "tga", "rgba32.tga", R_LoadTGA, 4, 3, 0, 0, tga_px);
	snprintf(d, sizeof(d), "%s/image_bmp/testsuite", seeds);
	test_image(d, "bmp", "rgb24.bmp", R_LoadBMP, 4, 2, 1, 0, bmp_px);
	snprintf(d, sizeof(d), "%s/image_pcx/testsuite", seeds);
	test_image(d, "pcx", "rle8.pcx", R_LoadPCX, 4, 3, 1, 1, pcx_px);
	test_pc();
	snprintf(d, sizeof(d), "%s/cm_load/testsuite", seeds);
	test_cm(d);

	printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
