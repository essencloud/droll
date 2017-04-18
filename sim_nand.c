/********************************************************************

                        Droll - SSD simulator

     Copyright (C) 2017 Hyunmo Chung (hyunmo@essencloud.com)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*********************************************************************/


#include "droll.h"

// 3D TLC NAND �ùķ��̼� ��


enum
{
	// block�� ��� �ִ� ������

	BLK_ERASED = 0,
	BLK_METADATA,
	BLK_USERDATA,
};

enum
{
	// current operation
	// SP = single plane, MP = multiplane (all-plane)

	OP_IDLE,
	OP_WRITE_USERDATA_MP,
	OP_WRITE_METADATA_SP,
	OP_WRITE_METADATA_MP,
	OP_READ,
	OP_ERASE_SP,
	OP_ERASE_MP,
};

typedef union
{
	struct	// userdata
	{
		UINT8	main[WLS_PER_BLK][PAGES_PER_WL][SECTORS_PER_SMALL_PAGE];	// �Ѽ��ʹ� �ѹ���Ʈ��
		UINT32	extra[WLS_PER_BLK][PAGES_PER_WL][SLICES_PER_SMALL_PAGE];
	};

	UINT8* metadata;

} sim_nand_blk_t;

typedef struct
{
	UINT16		usage;			// BLK_XXX
	UINT16		erase_count;
	UINT16		num_programmed_wls;
	UINT16		unused;

} blk_info_t;

typedef struct
{
	blk_info_t	blk_info[SMALL_BLKS_PER_DIE];
	UINT32		plane;
	UINT32		blk_index[PLANES_PER_DIE];
	UINT32		wl_index;
	UINT32		current_operation;			// OP_XXX
	UINT32		unused;

	sim_nand_blk_t* blk_array;				// sim_nand_blk_t  blk_array[SMALL_BLKS_PER_DIE];

	void*		data;

} sim_die_t;


static sim_die_t g_nand_die[NUM_DIES];


static void set_finish_time(UINT32 die, UINT64 time)
{
	UINT64 current_time = g_sim_context.current_time;

	sim_message_t* msg = sim_new_message();
	msg->code = SIM_EVENT_DELAY;
	msg->when = current_time + time;
	msg->arg_32 = die;
	sim_send_message(msg, SIM_ENTITY_NAND);
}

void sim_nand_erase_sp(UINT32 die_index, UINT16 small_blk_index)
{
	// FIL thread�� ���ؼ� ���� ȣ��Ǵ� �Լ�

	sim_die_t* die = g_nand_die + die_index;

	ASSERT(die->current_operation == OP_IDLE);
	die->current_operation = OP_ERASE_SP;

	blk_info_t* blk_info = die->blk_info + small_blk_index;

	UINT32 plane = small_blk_index % PLANES_PER_DIE;
	die->plane = plane;
	die->blk_index[plane] = small_blk_index;

	set_finish_time(die_index, NAND_T_BERS);

	NAND_STAT(g_nand_stat.erase_count[die_index]++);
	NAND_STAT(g_nand_stat.cell_time[die_index] += NAND_T_BERS);
}

void sim_nand_erase_mp(UINT32 die_index, UINT16* small_blk_index)
{
	sim_die_t* die = g_nand_die + die_index;

	ASSERT(die->current_operation == OP_IDLE);
	die->current_operation = OP_ERASE_MP;

	for (UINT32 i = 0; i < PLANES_PER_DIE; i++)
	{
		UINT32 s = small_blk_index[i];
		die->blk_index[i] = s;
	}

	set_finish_time(die_index, NAND_T_BERS);

	NAND_STAT(g_nand_stat.erase_count[die_index]++);
	NAND_STAT(g_nand_stat.cell_time[die_index] += NAND_T_BERS);
}

static void do_erase(sim_die_t* die, UINT32 plane)
{
	// NAND_T_BERS �� ����� �ڿ� NAND thread�� ���ؼ� ȣ��Ǵ� �Լ�

	UINT32 small_blk_index = die->blk_index[plane];
	blk_info_t* blk_info = die->blk_info + small_blk_index;

	if (blk_info->usage == BLK_METADATA)
	{
		sim_nand_blk_t* blk = die->blk_array + small_blk_index;
		VirtualFree(blk->metadata, 0, MEM_RELEASE);
	}

	blk_info->usage = BLK_ERASED;
	blk_info->erase_count++;
	blk_info->num_programmed_wls = 0;
}

void sim_nand_write_userdata(UINT32 die_index, UINT16* small_blk_index, UINT32 wl_index, nand_packet_t* nand_pkt)
{
	// userdata�� single plane program ������ �������� ����

	sim_die_t* die = g_nand_die + die_index;

	ASSERT(die->current_operation == OP_IDLE);
	die->current_operation = OP_WRITE_USERDATA_MP;

	for (UINT32 i = 0; i < PLANES_PER_DIE; i++)
	{
		UINT32 s = small_blk_index[i];
		die->blk_index[i] = s;
	}

	die->wl_index = wl_index;
	die->data = (void*) nand_pkt;

	set_finish_time(die_index, NAND_T_PROGO);

	NAND_STAT(g_nand_stat.write_count[die_index]++);
	NAND_STAT(g_nand_stat.cell_time[die_index] += NAND_T_PROGO);
}

static void do_write_userdata(sim_die_t* die)
{
	UINT32 wl_index = die->wl_index;						// sim_nand_write_userdata() ���� ����� �ξ��� ����
	nand_packet_t* nand_pkt = (nand_packet_t*) die->data;

	for (UINT32 plane = 0; plane < PLANES_PER_DIE; plane++)
	{
		UINT32 small_blk_index = die->blk_index[plane];
		sim_nand_blk_t* blk = die->blk_array + small_blk_index;
		blk_info_t* blk_info = die->blk_info + small_blk_index;

		if (wl_index == 0)
		{
			ASSERT(blk_info->usage == BLK_ERASED);
			blk_info->usage = BLK_USERDATA;
		}
		else
		{
			ASSERT(wl_index < WLS_PER_BLK && blk_info->usage == BLK_USERDATA);
		}

		ASSERT(blk_info->num_programmed_wls == wl_index);		// �� ������ wordline�� ���������� ��� �Ѵ�.
		blk_info->num_programmed_wls++;

		for (UINT32 lcm = 0; lcm < PAGES_PER_WL; lcm++)			// loop �ѹ� �� ������ small page �ϳ��� ó��
		{
			#if SECTORS_PER_SMALL_PAGE == 16
			*(UINT64*)(blk->main[wl_index][lcm]) = *(UINT64*)(nand_pkt->main_data[lcm][plane]);
			*(UINT64*)(blk->main[wl_index][lcm] + sizeof(UINT64)) = *(UINT64*)(nand_pkt->main_data[lcm][plane] + sizeof(UINT64));
			#else
			memcpy(blk->main[wl_index][lcm], nand_pkt->main_data[lcm][plane], sizeof(UINT8) * SECTORS_PER_SMALL_PAGE);
			#endif

			#if SLICES_PER_SMALL_PAGE == 2
			*(UINT64*)(blk->extra[wl_index][lcm]) = *(UINT64*)(nand_pkt->extra_data[lcm][plane]);
			#else
			memcpy(blk->extra[wl_index][lcm], nand_pkt->extra_data[lcm][plane], sizeof(UINT32) * SLICES_PER_SMALL_PAGE);
			#endif
		}
	}
}

void sim_nand_write_metadata_sp(UINT32 die_index, UINT16 small_blk_index, UINT32 wl_index, UINT8* buf)
{
	sim_die_t* die = g_nand_die + die_index;

	ASSERT(die->current_operation == OP_IDLE);
	die->current_operation = OP_WRITE_METADATA_SP;

	UINT32 plane = small_blk_index % PLANES_PER_DIE;

	die->plane = plane;							// ���߿� do_write_metadata()���� ���� ������ ó���ϱ� ���� ����
	die->blk_index[plane] = small_blk_index;
	die->wl_index = wl_index;
	die->data = (void*) buf;

	set_finish_time(die_index, NAND_T_PROGO);	// ������ write ������ �ð��� �帥 �ڿ� do_write_metadata()���� ó��

	NAND_STAT(g_nand_stat.write_count[die_index]++);
	NAND_STAT(g_nand_stat.cell_time[die_index] += NAND_T_PROGO);
}

void sim_nand_write_metadata_mp(UINT32 die_index, UINT16* small_blk_index, UINT32 wl_index, UINT8* buf)
{
	sim_die_t* die = g_nand_die + die_index;

	ASSERT(die->current_operation == OP_IDLE);
	die->current_operation = OP_WRITE_METADATA_SP;

	for (UINT32 i = 0; i < PLANES_PER_DIE; i++)
	{
		UINT32 s = small_blk_index[i];
		die->blk_index[i] = s;
	}

	die->wl_index = wl_index;
	die->data = (void*) buf;

	set_finish_time(die_index, NAND_T_PROGO);

	NAND_STAT(g_nand_stat.write_count[die_index]++);
	NAND_STAT(g_nand_stat.cell_time[die_index] += NAND_T_PROGO);
}

static void do_write_metadata_sp(sim_die_t* die)
{
	// metadata�� extra data�� �������� �ʴ´�.

	UINT32 plane = die->plane;
	UINT32 small_blk_index = die->blk_index[plane];
	UINT32 wl_index = die->wl_index;
	UINT64* data = (UINT64*) die->data;
	blk_info_t* blk_info = die->blk_info + small_blk_index;
	sim_nand_blk_t* blk = die->blk_array + small_blk_index;

	if (wl_index == 0)
	{
		ASSERT(blk_info->usage == BLK_ERASED);
		blk_info->usage = BLK_METADATA;

		blk->metadata = (UINT8*) VirtualAlloc(NULL, BYTES_PER_SMALL_BLK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		CHECK(blk->metadata != NULL);
	}
	else
	{
		ASSERT(wl_index < WLS_PER_BLK && blk_info->usage == BLK_METADATA);
	}

	memcpy(blk->metadata + wl_index * BYTES_PER_SMALL_WL, data, BYTES_PER_SMALL_WL);

	ASSERT(blk_info->num_programmed_wls == wl_index);
	blk_info->num_programmed_wls++;
}

static void do_write_metadata_mp(sim_die_t* die)
{
	UINT32 wl_index = die->wl_index;

	if (wl_index == 0)
	{
		for (UINT32 plane = 0; plane < PLANES_PER_DIE; plane++)
		{
			UINT32 small_blk_index = die->blk_index[plane];
			blk_info_t* blk_info = die->blk_info + small_blk_index;
			ASSERT(blk_info->usage == BLK_ERASED);
			blk_info->usage = BLK_METADATA;

			die->blk_array[small_blk_index].metadata = (UINT8*) VirtualAlloc(NULL, BYTES_PER_SMALL_BLK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			CHECK(die->blk_array[small_blk_index].metadata != NULL);
		}
	}
	else
	{
		ASSERT(wl_index < WLS_PER_BLK);
	}

	UINT8* data = (UINT8*) die->data;

	// �־��� �����͸� ���� plane�� L, C, M�� �й��ϴ� ����: �� plane ������ L, C, M�� ���� ä��� ���� �ƴϰ�, ��� plane�� L�� ���� ä���.

	for (UINT32 plane = 0; plane < PLANES_PER_DIE; plane++)
	{
		UINT32 small_blk_index = die->blk_index[plane];

		blk_info_t* blk_info = die->blk_info + small_blk_index;
		ASSERT(blk_info->usage == BLK_METADATA && blk_info->num_programmed_wls == wl_index);
		blk_info->num_programmed_wls++;

		UINT8* destination = die->blk_array[small_blk_index].metadata + wl_index * BYTES_PER_SMALL_WL;

		for (UINT32 lcm = 0; lcm < BIT_PER_CELL; lcm++)
		{
			memcpy(destination, data + lcm*BYTES_PER_BIG_PAGE + plane*BYTES_PER_SMALL_PAGE, BYTES_PER_SMALL_PAGE);

			destination += BYTES_PER_SMALL_PAGE;
		}
	}
}

UINT64 sim_nand_read_userdata(UINT8 die_index, UINT16 small_blk_index, UINT16 wl_index, UINT8 lcm, UINT8 slice, UINT32* extra_data)
{
	// �� �� ȣ�⿡ �ϳ��� �����̽��� ����

	sim_die_t* die = g_nand_die + die_index;
	blk_info_t* blk_info = die->blk_info + small_blk_index;

	ASSERT(blk_info->usage == BLK_USERDATA || blk_info->usage == BLK_ERASED);

	if (die->current_operation == OP_IDLE)
	{
		die->current_operation = OP_READ;

		set_finish_time(die_index, NAND_T_R);

		NAND_STAT(g_nand_stat.read_count[die_index]++);
		NAND_STAT(g_nand_stat.cell_time[die_index] += NAND_T_R);
	}
	else
	{
		// �� big page���� �� �̻��� slice�� �д� ��쿡�� �� �Լ��� �� �� �̻� ȣ��ȴ�.

		ASSERT(die->current_operation == OP_READ);
	}

	UINT64 main_data;

	if (wl_index >= blk_info->num_programmed_wls)
	{
		main_data = FF64;
	}
	else
	{
		sim_nand_blk_t* blk = die->blk_array + small_blk_index;

		SANITY_CHECK(SECTORS_PER_SLICE == 8);
		main_data = *(UINT64*)(blk->main[wl_index][lcm] + slice*SECTORS_PER_SLICE);

		*extra_data = blk->extra[wl_index][lcm][slice];
	}

	return main_data;
}

void sim_nand_read_metadata(UINT8 die_index, UINT16 small_blk_index, UINT16 wl_index, UINT8 lcm, UINT8 slice, UINT8* buf)
{
	// �� �� ȣ�⿡ �ϳ��� �����̽��� ����

	sim_die_t* die = g_nand_die + die_index;
	blk_info_t* blk_info = die->blk_info + small_blk_index;

	ASSERT(blk_info->usage == BLK_METADATA || blk_info->usage == BLK_ERASED);

	if (wl_index >= blk_info->num_programmed_wls)
	{
		STOSQ(buf, FF64, BYTES_PER_SLICE);
	}
	else
	{
		UINT32 s = lcm*SLICES_PER_SMALL_PAGE + slice;	// slice = small page �������� slice ��ȣ, s = small wordline �������� slice ��ȣ

		UINT8* source = die->blk_array[small_blk_index].metadata + wl_index*BYTES_PER_SMALL_WL + s*BYTES_PER_SLICE;

		memcpy(buf, source, BYTES_PER_SLICE);
	}
}

BOOL8 sim_nand_busy(UINT32 die)
{
	return g_nand_die[die].current_operation != OP_IDLE;
}

static void begin_simulation(void)
{
	g_random = new mt19937(g_sim_context.random_seed);

	for (UINT32 d = 0; d < NUM_DIES; d++)
	{
		sim_die_t* die = g_nand_die + d;

		UINT32 num_bytes = ROUND_UP(sizeof(sim_nand_blk_t) * SMALL_BLKS_PER_DIE, 4096);
		die->blk_array = (sim_nand_blk_t*) VirtualAlloc(NULL, num_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		CHECK(die->blk_array != NULL);	// PC �޸𸮰� �����ؼ� ����� �����̹Ƿ� ���� �޸𸮸� �ÿ��� �ذ�
	}
}

static void end_simulation(void)
{
	for (UINT32 d = 0; d < NUM_DIES; d++)
	{
		sim_die_t* die = g_nand_die + d;

		for (UINT32 small_blk_index = 0; small_blk_index < SMALL_BLKS_PER_DIE; small_blk_index++)
		{
			sim_nand_blk_t* blk = die->blk_array + small_blk_index;
			VirtualFree(blk->metadata, 0, MEM_RELEASE);
		}

		VirtualFree((void*) die->blk_array, 0, MEM_RELEASE);
	}
}

static void begin_session(void)
{
	for (UINT32 d = 0; d < NUM_DIES; d++)
	{
		sim_die_t* die = g_nand_die + d;
		die->current_operation = OP_IDLE;
	}

	NAND_STAT(reset_nand_statistics());
}

static void end_session(void)
{

}

static void finish_nand_operation(UINT32 die_index)
{
	sim_die_t* die = g_nand_die + die_index;

	switch (die->current_operation)
	{
		case OP_WRITE_USERDATA_MP:
		{
			do_write_userdata(die);
			break;
		}
		case OP_WRITE_METADATA_SP:
		{
			do_write_metadata_sp(die);
			break;
		}
		case OP_WRITE_METADATA_MP:
		{
			do_write_metadata_mp(die);
			break;
		}
		case OP_READ:
		{
			// �� �� ����
			break;
		}
		case OP_ERASE_SP:
		{
			do_erase(die, die->plane);
			break;
		}
		case OP_ERASE_MP:
		{
			for (UINT32 i = 0; i < PLANES_PER_DIE; i++)
			{
				do_erase(die, i);
			}

			break;
		}
		default:
			CHECK(FAIL);
	}

	die->current_operation = OP_IDLE;
}

static void nand_main(void)
{
	while (1)
	{
		sim_message_t* msg = sim_receive_message(SIM_ENTITY_NAND);

		UINT16 msg_code = msg->code;
		UINT32 arg = msg->arg_32;
		sim_release_message_slot(msg);

		if (msg_code == SIM_EVENT_PRINT_STAT)
		{
			NAND_STAT(print_nand_statistics());
			NAND_STAT(reset_nand_statistics());
		}
		else
		{
			ASSERT(msg_code == SIM_EVENT_DELAY);			// set_finish_time()�� ���ؼ� ������ �޽���

			finish_nand_operation(arg);
		}
	}
}

void sim_nand_thread(void* arg_list)
{
	UNREFERENCED_PARAMETER(arg_list);

	begin_simulation();

	setjmp(g_jump_buf[SIM_ENTITY_NAND]);

	while (1)
	{
		sim_message_t* msg = sim_receive_message(SIM_ENTITY_NAND);
		UINT32 msg_code = msg->code;

		if (msg_code == SIM_EVENT_POWER_ON)
		{
			begin_session();							// �ʱ�ȭ
			sim_send_message(msg, SIM_ENTITY_FIL);		// SIM_EVENT_POWER_ON �޽����� FIL���� ����
			nand_main();
			end_session();
		}
		else
		{
			sim_release_message_slot(msg);

			if (msg_code == SIM_EVENT_END_SIMULATION)
			{
				break;
			}
		}
	}

	end_simulation();
}

