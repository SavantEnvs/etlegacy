/*
 * File-input (AFL) harness for the network message layer
 * (src/qcommon/msg.c, src/qcommon/huffman.c).
 *
 * Every packet a client gets from a server (and every client packet a server
 * gets) is a msg_t that is Huffman-decoded (Huff_Decompress, after the netchan
 * header: CL_DECODE_START / SV_DECODE_START) and then read with the MSG_Read*
 * family: delta-compressed entityState_t / playerState_t / usercmd_t, strings,
 * and raw data. This harness replays that shape: input byte 0 selects the
 * Huffman decode offset, the rest is the packet; then a stream of read
 * operations driven by the packet itself, with each delta read against the
 * previously decoded state (as snapshots are), until the message is exhausted.
 *
 * Com_Error (e.g. "Bad delta entity number", "invalid ... field count") ends
 * the process, as the engine drops the connection; so this is a file-input
 * target, one packet per process (see fz_common.c).
 */
#include <stdlib.h>
#include <string.h>

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"

cvar_t *cl_shownet = NULL;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	static byte init[16];
	msg_t       m;

	(void)argc;
	(void)argv;
	/* MSG_Init builds the static Huffman tables once per process (the engine
	 * does this at startup). That is most of the run time, so the AFL fork
	 * server is started after it. */
	MSG_Init(&m, init, sizeof(init));
#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif
	return 0;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
	static byte          buf[MAX_MSGLEN];
	static entityState_t ents[2];
	static playerState_t pss[2];
	static usercmd_t     cmds[2];
	msg_t                msg;
	int                  ctrl, cur = 0, op, n, key;
	char                 scratch[256];

	if (size < 1 || size - 1 > MAX_MSGLEN)
	{
		return -1; /* the netchan never accepts a packet larger than MAX_MSGLEN */
	}
	ctrl = data[0];

	memset(ents, 0, sizeof(ents));
	memset(pss, 0, sizeof(pss));
	memset(cmds, 0, sizeof(cmds));

	MSG_Init(&msg, buf, sizeof(buf));
	memcpy(buf, data + 1, size - 1);
	msg.cursize = (int)size - 1;

	MSG_BeginReading(&msg);
	if (ctrl & 1)
	{
		/* netchan-encoded packet: header read uncompressed, then the payload is decoded */
		int start = (ctrl & 2) ? SV_DECODE_START : CL_DECODE_START;
		MSG_ReadLong(&msg);
		if (start == SV_DECODE_START)
		{
			MSG_ReadLong(&msg);
			MSG_ReadLong(&msg);
		}
		Huff_Decompress(&msg, start);
	}
	else
	{
		MSG_BeginReadingOOB(&msg);
		if (ctrl & 2)
		{
			/* connectionless: 0xffffffff + text command */
			MSG_ReadLong(&msg);
			MSG_ReadStringLine(&msg);
			MSG_Bitstream(&msg);
		}
	}
	MSG_Bitstream(&msg);

	key = MSG_ReadLong(&msg);

	for (n = 0; n < 4096 && msg.readcount <= msg.cursize; n++)
	{
		op = MSG_ReadByte(&msg);
		if (op < 0)
		{
			break;
		}
		switch (op % 12)
		{
		case 0:
		{
			/* CL_ParsePacketEntities */
			int number = MSG_ReadBits(&msg, GENTITYNUM_BITS);
			MSG_ReadDeltaEntity(&msg, &ents[cur], &ents[cur ^ 1], number);
			cur ^= 1;
			break;
		}
		case 1:
			/* CL_ParseSnapshot */
			MSG_ReadDeltaPlayerstate(&msg, (op & 0x80) ? NULL : &pss[cur], &pss[cur ^ 1]);
			cur ^= 1;
			break;
		case 2:
			/* SV_UserMove */
			MSG_ReadDeltaUsercmdKey(&msg, key, &cmds[cur], &cmds[cur ^ 1]);
			cur ^= 1;
			break;
		case 3:
			MSG_ReadString(&msg);
			break;
		case 4:
			MSG_ReadBigString(&msg);
			break;
		case 5:
			MSG_ReadStringLine(&msg);
			break;
		case 6:
			MSG_ReadShort(&msg);
			MSG_ReadChar(&msg);
			break;
		case 7:
			MSG_ReadFloat(&msg);
			MSG_ReadAngle16(&msg);
			break;
		case 8:
		{
			int len = MSG_ReadByte(&msg);
			if (len > 0)
			{
				MSG_ReadData(&msg, scratch, len);
			}
			break;
		}
		case 9:
			MSG_ReadBits(&msg, (op >> 4) + 1);
			break;
		case 10:
		{
			/* CL_ParseGamestate: a baseline, delta'd from a zeroed state */
			entityState_t nullstate;
			int           number = MSG_ReadBits(&msg, GENTITYNUM_BITS);
			memset(&nullstate, 0, sizeof(nullstate));
			MSG_ReadDeltaEntity(&msg, &nullstate, &ents[cur ^ 1], number);
			cur ^= 1;
			break;
		}
		default:
			MSG_ReadLong(&msg);
			break;
		}
	}
	return 0;
}
