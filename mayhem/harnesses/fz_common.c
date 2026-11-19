/*
 * Engine print/error entry points for the harnesses that link engine or mod
 * code without the rest of the engine (src/qcommon/common.c).
 *
 * In the engine, Com_Error(ERR_DROP) unwinds to the frame loop with longjmp
 * and drops the server/connection; ERR_FATAL quits. A harness must not unwind
 * out of the code under test, so:
 *   - file-input (AFL) targets build this with -DFZ_ERROR_EXITS: an engine
 *     error ends the process cleanly (exit status 1), which is what the
 *     engine's error path amounts to for one input;
 *   - libFuzzer targets only link code whose Com_Error calls are internal
 *     invariants (allocation failure, a caller passing a bad size), so
 *     reaching one is reported as a crash.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "qcommon/q_shared.h"

void QDECL Com_Printf(const char *fmt, ...)
{
	(void)fmt;
}

void QDECL Com_DPrintf(const char *fmt, ...)
{
	(void)fmt;
}

void QDECL Com_Error(int code, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "Com_Error(%d): ", code);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
#ifdef FZ_ERROR_EXITS
	exit(1);
#else
	abort();
#endif
}
