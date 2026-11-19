/*
 * libFuzzer harness for the renderer's in-tree image decoders
 * (src/renderercommon/tr_image_{tga,bmp,pcx}.c).
 *
 * The renderer reaches these through R_LoadImage() with the bytes of a texture
 * file read from a pk3 (a server-supplied download) - the loaders' contract is
 * exactly `imageData_t {size, name, buffer}` -> RGBA pixels. Build with
 * -DIMAGE_LOADER=R_LoadTGA / R_LoadBMP / R_LoadPCX. image_tga and image_pcx are
 * libFuzzer targets; image_bmp is built as a file-input (AFL) target with
 * afl_main.c, see mayhem/build.sh.
 *
 * The loaders only touch the engine through the `ri` refimport table; the
 * harness provides the memory/print entries of it backed by libc.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renderercommon/tr_common.h"

#ifndef IMAGE_LOADER
#error "define IMAGE_LOADER"
#endif

qboolean IMAGE_LOADER(imageData_t *data, byte **pic, int *width, int *height, byte alphaByte);

refimport_t ri;

static void QDECL fz_Printf(int printLevel, const char *fmt, ...)
{
	(void)printLevel;
	(void)fmt;
}

static void QDECL _attribute((noreturn)) fz_Error(int errorLevel, const char *fmt, ...)
{
	/* The image loaders never call ri.Error; reaching here would be a new
	 * engine-level abort path, so make it visible. */
	va_list ap;
	(void)errorLevel;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	abort();
}

static void *fz_Z_Malloc(int bytes)
{
	/* Z_Malloc zero-fills (common.c Z_TagMalloc + Com_Memset) */
	return calloc(1, (size_t)(bytes > 0 ? bytes : 1));
}

static void *fz_Hunk_AllocateTempMemory(size_t size)
{
	return malloc(size ? size : 1);
}

/*
 * R_GetImageBuffer lives in the GL renderer (src/renderer/tr_image.c), which
 * hands out a shared scratch buffer of at least `size` bytes. The harness
 * allocates exactly `size` bytes per load instead, so ASan sees any write past
 * what the loader asked for.
 */
static void *imgbuf;

void *R_GetImageBuffer(int size, bufferMemType_t bufferType, const char *filename)
{
	(void)bufferType;
	(void)filename;
	free(imgbuf);
	imgbuf = malloc(size > 0 ? (size_t)size : 1);
	return imgbuf;
}

static void fz_Free(void *p)
{
	if (p == imgbuf)
	{
		imgbuf = NULL;
	}
	free(p);
}

static void fz_FS_FreeFile(void *p)
{
	/* the harness owns the input buffer */
	(void)p;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	ri.Printf                 = fz_Printf;
	ri.Error                  = fz_Error;
	ri.Z_Malloc               = fz_Z_Malloc;
	ri.Free                   = fz_Free;
	ri.Hunk_AllocateTempMemory = fz_Hunk_AllocateTempMemory;
	ri.Hunk_FreeTempMemory    = fz_Free;
	ri.FS_FreeFile            = fz_FS_FreeFile;
	return 0;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
	imageData_t img;
	byte        *pic = NULL;
	int         w    = 0, h = 0;
	byte        *copy;

	if (size > (1u << 24))
	{
		return -1; /* FS_ReadFile returns an int length */
	}
	/* exact-size heap copy so ASan sees reads past the file's end */
	copy = (byte *)malloc(size ? size : 1);
	if (size)
	{
		memcpy(copy, data, size);
	}

	img.size     = (int)size;
	img.name     = "fuzz";
	img.buffer.b = copy;

	if (IMAGE_LOADER(&img, &pic, &w, &h, 0xff) && pic)
	{
		/* touch every decoded pixel so a short allocation is caught */
		volatile unsigned sum = 0;
		size_t            i, n = (size_t)w * (size_t)h * 4;
		for (i = 0; i < n; i++)
		{
			sum += pic[i];
		}
		(void)sum;
	}
	if (pic && pic != imgbuf)
	{
		free(pic);
	}
	free(imgbuf);
	imgbuf = NULL;
	free(copy);
	return 0;
}
