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
#include "script.h"


/////////////////
// Host context
/////////////////

typedef enum
{
	// SATA Host State

	SHS_INIT,			// Register FIS D2H (hello) ��ٸ��� ��
	SHS_IDLE,			// SATA IDLE ����
	SHS_CMD,			// Register FIS H2D (command) ������
	SHS_DATA_H2D,		// DATA FIS H2D ������
	SHS_DATA_D2H,		// ����̽��κ��� DATA FIS D2H ������
	SHS_ACK,			// ����̽��κ��� Register FIS D2H (ack) ������
	SHS_DMA_SETUP,		// ����̽��κ��� DMA SETUP FIS ������
	SHS_DMA_ACTIVATE,	// ����̽��κ��� DMA ACTIVATE FIS ������
	SHS_WAIT_ACK,		// Register FIS D2H (ack) ��ٸ��� ��
	SHS_WAIT_DMA_ACT,	// DMA ACTIVATE FIS ��ٸ��� ��
	SHS_WAIT_DATA, 		// DATA FIS D2H ��ٸ��� ��
	SHS_SDB,			// ����̽��κ��� Set Device Bits FIS ������
} shs_t;


#define NUM_HOT_SPOTS 	2
#define TEMPERATURE_MAX	128

typedef struct
{
	sim_sata_cmd_t cmd_table[NCQ_SIZE];
	sim_sata_cmd_t* free_slot_ptr;
	sim_sata_cmd_t* current_cmd;

	UINT32* loop_begin;
	UINT64	loop_end;
	UINT64	loop_cycle_count;
	UINT64	loop_sector_count;

	UINT16	num_pending_cmds;
	UINT16	loop_mode;
	UINT16	aligned_read_p;			// read ����� ����/�� LBA�� 4KB ��輱�� ���� Ȯ�� (1024 = 100%)
	UINT16	aligned_write_p;		// write ����� ����/�� LBA�� 4KB ��輱�� ���� Ȯ�� (1024 = 100%)

	shs_t	sata_state;
	UINT32	prev_count;
	UINT32	prev_lba;
	UINT32	seq_lba;

	UINT32	hot_spot[NUM_HOT_SPOTS];
	UINT32	temperature[NUM_HOT_SPOTS];

	UINT32	cmd_p_sum;
	UINT32	cmd_p[RANDOM_CMD];

	UINT32	random_lba_min;
	UINT32	random_lba_max;

	UINT8	nop_period_min;
	UINT8	nop_period_max;
	UINT16	max_queue_depth;

	UINT64	idle_begin;		// SATA link layer�� idle ���°� ���۵� �ð�
	sim_message_t* last_cmd;

	UINT32	data_seq;

	UINT8	last_ncq_tag;	// ���� �ֱٿ� ���´� ����� ncq_tag ��
	BOOL8	collision;
	BOOL8	delay_pending;
	BOOL8	hot_spot_enable;

} sim_host_t;

static sim_host_t g_host;
static UINT8* g_local_mem;


#if OPTION_VERIFY_DATA

static void generate_write_data(UINT8* data_packet, UINT32 lba, UINT32 num_sectors)
{
	while (1)
	{
		#if 0
		UINT8 old_data = g_local_mem[lba];
		UINT8 new_data = old_data + 1;
		#endif

		#if 0
		UINT8 new_data = lba % SECTORS_PER_SLICE;
		#endif

		#if 1
		UINT8 new_data = (UINT8) g_host.data_seq++;
		#endif

		g_local_mem[lba] = new_data;

		*data_packet++ = new_data;

		if (--num_sectors == 0)
			break;

		lba++;
	}
}

static void verify_read_data(UINT8* data_packet, UINT32 lba, UINT32 num_sectors)
{
	for (UINT32 i = 0; i < num_sectors; i++, lba++)
	{
		if (data_packet[i] != g_local_mem[lba])
		{
			printf("data mismatch: lba=%u lsa=%u read=%02X correct=%02X\t\t\t\t\n", lba, lba / SECTORS_PER_SLICE, data_packet[i], g_local_mem[lba]);
			CHECK(FAIL);
		}
	}
}

#endif	// OPTION_VERIFY_DATA

static void send_write_data(UINT8* data_packet, sim_sata_cmd_t* cmd)
{
	// �� �Լ��� ȣ���� ������ �ϳ��� data FIS (�ִ� 8KB)�� ����̽����� ����

	UINT64 current_time = g_sim_context.current_time;

	sim_message_t* msg = sim_new_message();
	msg->code = SIM_EVENT_WDATA_S;
	msg->when = current_time + SIM_SATA_WDATA_DELAY;
	PRINT_SATA_HOST("HOST %llu: SIM_EVENT_WDATA_S, %llu\n", current_time, msg->when);
	sim_send_message(msg, SIM_ENTITY_HIL);

	UINT32 num_sectors_remaining = cmd->num_sectors_requested - cmd->num_sectors_completed;
	UINT32 num_sectors_to_send = MIN(DATA_FIS_SIZE_MAX, num_sectors_remaining);

	#if OPTION_VERIFY_DATA
	generate_write_data(data_packet, cmd->lba + cmd->num_sectors_completed, num_sectors_to_send);
	#else
	UNREFERENCED_PARAMETER(data_packet);
	#endif

	msg = sim_new_message();
	msg->code = SIM_EVENT_WDATA_E;
	msg->when = current_time + SIM_SATA_WDATA_DELAY + num_sectors_to_send * SIM_SATA_NANOSEC_PER_SECTOR;
	msg->arg_32 = num_sectors_to_send;
	PRINT_SATA_HOST("HOST %llu: SIM_EVENT_WDATA_E, %llu\n", current_time, msg->when);
 	sim_send_message(msg, SIM_ENTITY_HOST);
}


static void remove_command(UINT32 ncq_tag)
{
	// ����� ����� ���̺��� ����

	ASSERT(ncq_tag < NCQ_SIZE);

	sim_sata_cmd_t* cmd = g_host.cmd_table + ncq_tag;

	#if VERBOSE_HOST_STATISTICS
	{
		UINT64 time_delta = g_sim_context.current_time - cmd->submit_time;

		switch (cmd->code)
		{
			case REQ_HOST_READ:
			{
				g_host_stat.read_latency_sum += time_delta;
				g_host_stat.read_latency_min = MIN(time_delta, g_host_stat.read_latency_min);
				g_host_stat.read_latency_max = MAX(time_delta, g_host_stat.read_latency_max);
				g_host_stat.num_read_sectors += cmd->num_sectors_completed;
				g_host_stat.num_read_commands++;
				break;
			}
			case REQ_HOST_WRITE:
			{
				g_host_stat.write_latency_sum += time_delta;
				g_host_stat.write_latency_min = MIN(time_delta, g_host_stat.write_latency_min);
				g_host_stat.write_latency_max = MAX(time_delta, g_host_stat.write_latency_max);
				g_host_stat.num_write_sectors += cmd->num_sectors_completed;
				g_host_stat.num_write_commands++;
				break;
			}
			case REQ_TRIM:
			{
				g_host_stat.trim_latency_sum += time_delta;
				g_host_stat.trim_latency_min = MIN(time_delta, g_host_stat.trim_latency_min);
				g_host_stat.trim_latency_max = MAX(time_delta, g_host_stat.trim_latency_max);
				g_host_stat.num_trim_sectors += cmd->num_sectors_completed;
				g_host_stat.num_trim_commands++;
				break;
			}
			case REQ_FAST_FLUSH:
			{
				g_host_stat.fast_flush_latency_sum += time_delta;
				g_host_stat.fast_flush_latency_min = MIN(time_delta, g_host_stat.fast_flush_latency_min);
				g_host_stat.fast_flush_latency_max = MAX(time_delta, g_host_stat.fast_flush_latency_max);
				g_host_stat.num_fast_flush_commands++;
				break;
			}
			case REQ_SLOW_FLUSH:
			{
				g_host_stat.slow_flush_latency_sum += time_delta;
				g_host_stat.slow_flush_latency_min = MIN(time_delta, g_host_stat.slow_flush_latency_min);
				g_host_stat.slow_flush_latency_max = MAX(time_delta, g_host_stat.slow_flush_latency_max);
				g_host_stat.num_slow_flush_commands++;
				break;
			}
		}

		g_host_stat.num_commands++;
	}
	#endif

	ASSERT(cmd->code != FF8);
	cmd->code = FF8;

	cmd->link_ptr = g_host.free_slot_ptr;
	g_host.free_slot_ptr = cmd;

	ASSERT(g_host.num_pending_cmds != 0);
	g_host.num_pending_cmds--;
}

static void message_handler(void)
{
	sim_message_t* msg = sim_receive_message(SIM_ENTITY_HOST);

	shs_t current_sata_state = g_host.sata_state;
	shs_t next_sata_state = current_sata_state;

	se_t msg_code = (se_t) msg->code;
	UINT64 current_time = g_sim_context.current_time;

	switch (msg_code)
	{
		case SIM_EVENT_SATA_CMD_E:	// Register FIS H2D�� ��
		{
			if (current_sata_state == SHS_CMD && msg->arg_64 != NULL)
			{
				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SATA_CMD_E good, %llu, %u\n", current_time, msg->when, msg->seq_number);
				INSIGHT(g_insight.link_busy = FALSE);
				HOST_STAT(g_host.idle_begin = current_time);

				sim_send_message(msg, SIM_ENTITY_HIL);
				msg = NULL;
				next_sata_state = SHS_WAIT_ACK;
			}
			else
			{
				// PCIe/NVMe�� �޸� SATA�� half duplex�̴�.
				// SATA IDLE ���¿� �����ϴ� ���� ȣ��Ʈ�� ���ο� ����� ���� �� �ְ�,
				// ����̽��� ���� ��ɿ� ���� DMA SETUP FIS �Ǵ� Set Device Bits FIS�� ���� �� �ֱ� ������ ��ȣ �浹 ���ɼ��� �ִ�.
				// SATA bus collision�� �߻��ϸ� ȣ��Ʈ�� �纸�ϵ��� SATA �Ծ࿡ ��õǾ� �����Ƿ�, ���߿� SHS_IDLE ���°� �Ǹ� ��õ� �� ���̴�.

				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SATA_CMD_E collision, %llu, %u\n", current_time, msg->when, msg->seq_number);

				ASSERT(msg->arg_64 == NULL || g_host.collision == TRUE);
			}

			break;
		}
		case SIM_EVENT_SATA_ACK_S:	// Register FIS D2H�� ����
		{
			PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SATA_ACK_S, %llu, %u\n", current_time, msg->when, msg->seq_number);

			ASSERT(current_sata_state == SHS_WAIT_ACK);
			next_sata_state = SHS_ACK;
			INSIGHT(g_insight.link_busy = TRUE);
			HOST_STAT(g_host_stat.link_idle_time += current_time - g_host.idle_begin);
			break;
		}
		case SIM_EVENT_SATA_ACK_E:	// Register FIS D2H�� ��
		{
			PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SATA_ACK_E, %llu, %u\n", current_time, msg->when, msg->seq_number);

			ASSERT(current_sata_state == SHS_ACK);
			next_sata_state = SHS_IDLE;

			INSIGHT(g_insight.cmd_busy = FALSE, g_insight.link_busy = FALSE);
			g_host.idle_begin = current_time;

			if (msg->arg_32 == FALSE)	// non-NCQ ����� �Ϸ�Ǿ���, send_sata_command()�� ���� ����
			{
				remove_command(g_host.last_ncq_tag);
			}

			break;
		}
		case SIM_EVENT_SDB_S:		// Set Device Bits FIS�� ����
		{
			if (current_sata_state == SHS_IDLE)
			{
				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SDB_S good, %llu, %u\n", current_time, msg->when, msg->seq_number);
			}
			else
			{
				ASSERT(current_sata_state == SHS_CMD);
				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SDB_S collision, %llu, %u\n", current_time, msg->when, msg->seq_number);
				g_host.collision = TRUE;
			}

			next_sata_state = SHS_SDB;
			INSIGHT(g_insight.cmd_busy = TRUE, g_insight.link_busy = TRUE);
			HOST_STAT(g_host_stat.link_idle_time += current_time - g_host.idle_begin);

			break;
		}
		case SIM_EVENT_SDB_E:		// Set Device Bits FIS�� ��
		{
			if (next_sata_state == SHS_SDB)
			{
				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SDB_E good, %llu, %u\n", current_time, msg->when, msg->seq_number);
			}
			else
			{
				ASSERT(current_sata_state == SHS_CMD);
				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SDB_E collision, %llu, %u\n", current_time, msg->when, msg->seq_number);
				g_host.collision = TRUE;
			}

			next_sata_state = SHS_IDLE;

			INSIGHT(g_insight.cmd_busy = FALSE, g_insight.link_busy = FALSE);
			g_host.idle_begin = current_time;

			UINT32 sdb_bitmap = msg->arg_32;

			while (sdb_bitmap != 0)
			{
				UINT32 ncq_tag;
				_BitScanForward((DWORD*) &ncq_tag, sdb_bitmap);
				sdb_bitmap &= ~BIT(ncq_tag);

				remove_command(ncq_tag);	// NCQ ����� �Ϸ�Ǿ���, send_sata_command()�� ���� ����
			}

			break;
		}
		case SIM_EVENT_DMA_SETUP_S:	// DMA SETUP FIS�� ���� (read or write)
		{
			if (current_sata_state == SHS_IDLE)
			{
				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_DMA_SETUP_S good, %llu, %u\n", current_time, msg->when, msg->seq_number);
			}
			else
			{
				ASSERT(current_sata_state == SHS_CMD);
				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_DMA_SETUP_S collision, %llu, %u\n", current_time, msg->when, msg->seq_number);
				g_host.collision = TRUE;
			}

			next_sata_state = SHS_DMA_SETUP;
			INSIGHT(g_insight.cmd_busy = TRUE, g_insight.link_busy = TRUE);
			HOST_STAT(g_host_stat.link_idle_time += current_time - g_host.idle_begin);

			break;
		}
		case SIM_EVENT_DMA_SETUP_E:	// DMA SETUP FIS�� �� (read or write)
		{
			// �� ȣ��Ʈ ���� non-zero buffer offset�� �������� �ʴ´�.
			// ����̽��� Ŀ�ǵ带 �����ϴ� ������ ��������, �ϴ� �ϳ��� ��� DMA SETUP FIS�� �������� �ش� Ŀ�ǵ��� ������ ������ �ѹ濡 �Ϸ�Ǿ�� �Ѵ�.

			if (current_sata_state == SHS_DMA_SETUP)
			{
				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_DMA_SETUP_E good, %llu, %u\n", current_time, msg->when, msg->seq_number);
			}
			else
			{
				ASSERT(current_sata_state == SHS_CMD);
				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_DMA_SETUP_E collision, %llu, %u\n", current_time, msg->when, msg->seq_number);
				g_host.collision = TRUE;
			}

			UINT32 ncq_tag = msg->arg_32;
			ASSERT(ncq_tag < NCQ_SIZE);
			sim_sata_cmd_t* cmd = g_host.cmd_table + ncq_tag;
			ASSERT(cmd->num_sectors_completed == 0 && cmd->num_sectors_requested != 0);

			ASSERT(g_host.current_cmd == NULL);
			g_host.current_cmd = cmd;

			if (cmd->code == REQ_HOST_READ)
			{
				next_sata_state = SHS_WAIT_DATA;
				INSIGHT(g_insight.link_busy = FALSE);
				HOST_STAT(g_host.idle_begin = current_time);
			}
			else
			{
				ASSERT(cmd->code == REQ_HOST_WRITE);
				next_sata_state = SHS_DATA_H2D;

				send_write_data((UINT8*) msg->arg_64, cmd);
			}

			break;
		}
		case SIM_EVENT_DMA_ACTV_S:	// DMA ACTIVATE FIS�� ���� (write)
		{
			ASSERT(current_sata_state == SHS_WAIT_DMA_ACT);
			next_sata_state = SHS_DMA_ACTIVATE;
			INSIGHT(g_insight.link_busy = TRUE);
			HOST_STAT(g_host_stat.link_idle_time += current_time - g_host.idle_begin);
			break;
		}
		case SIM_EVENT_DMA_ACTV_E:	// DMA ACTIVATE FIS�� �� (write)
		{
			ASSERT(current_sata_state == SHS_DMA_ACTIVATE);
			next_sata_state = SHS_DATA_H2D;

			ASSERT(g_host.current_cmd != NULL);	// DMA SETUP FIS�� ���� ���� �����Ǿ���
			sim_sata_cmd_t* cmd = g_host.current_cmd;
			ASSERT(cmd->num_sectors_completed > 0 && cmd->num_sectors_completed < cmd->num_sectors_requested);
			ASSERT(cmd->code == REQ_HOST_WRITE);

			send_write_data((UINT8*) msg->arg_64, cmd);

			break;
		}
		case SIM_EVENT_RDATA_S:		// DATA FIS�� ���� (read)
		{
			ASSERT(current_sata_state == SHS_WAIT_DATA);
			next_sata_state = SHS_DATA_D2H;

			INSIGHT(g_insight.link_busy = TRUE);
			HOST_STAT(g_host_stat.link_idle_time += current_time - g_host.idle_begin);

			ASSERT(g_host.current_cmd != NULL);	// DMA SETUP FIS�� ���� ���� �����Ǿ���
			sim_sata_cmd_t* cmd = g_host.current_cmd;

			ASSERT(cmd->code == REQ_HOST_READ);

			UINT32 num_sectors = msg->arg_32;
			ASSERT(num_sectors > 0 && num_sectors <= DATA_FIS_SIZE_MAX);

			#if OPTION_VERIFY_DATA
			verify_read_data((UINT8*) msg->arg_64, cmd->lba + cmd->num_sectors_completed, num_sectors);
			#endif

			cmd->num_sectors_completed += num_sectors;

			break;
		}
		case SIM_EVENT_RDATA_E:		// DATA FIS�� �� (read)
		{
			ASSERT(current_sata_state == SHS_DATA_D2H);

			sim_sata_cmd_t* cmd = g_host.current_cmd;

			if (cmd->num_sectors_completed == cmd->num_sectors_requested)
			{
				// �� Ŀ�ǵ�� ��ǻ� �Ϸ�Ǿ�����, ����̽��� Set Device Bits�� ������ ��μ� �Ϸ�� ������ ����

				next_sata_state = SHS_IDLE;

				INSIGHT(g_insight.cmd_busy = FALSE; g_insight.link_busy = FALSE);
				g_host.idle_begin = current_time;

				g_host.current_cmd = NULL;
			}
			else
			{
				next_sata_state = SHS_WAIT_DATA;
				INSIGHT(g_insight.link_busy = FALSE);
				HOST_STAT(g_host.idle_begin = current_time);
			}

			break;
		}
		case SIM_EVENT_WDATA_E:		// DATA FIS�� �� (write)
		{
			ASSERT(current_sata_state == SHS_DATA_H2D);

			ASSERT(g_host.current_cmd != NULL);	// DMA SETUP FIS�� ���� ���� �����Ǿ���
			sim_sata_cmd_t* cmd = g_host.current_cmd;

			ASSERT(cmd->code == REQ_HOST_WRITE);

			UINT32 num_sectors = msg->arg_32;
			ASSERT(num_sectors > 0 && num_sectors <= DATA_FIS_SIZE_MAX);
			cmd->num_sectors_completed += num_sectors;

			if (cmd->num_sectors_completed == cmd->num_sectors_requested)
			{
				// �� ����� ��ǻ� �Ϸ�Ǿ�����, ����̽��� Set Device Bits�� ������ ��μ� �Ϸ�� ������ ����

				next_sata_state = SHS_IDLE;

				INSIGHT(g_insight.cmd_busy = FALSE; g_insight.link_busy = FALSE);
				g_host.idle_begin = current_time;

				g_host.current_cmd = NULL;
			}
			else
			{
				next_sata_state = SHS_WAIT_DMA_ACT;
				INSIGHT(g_insight.link_busy = FALSE);
				HOST_STAT(g_host.idle_begin = current_time);
			}

			sim_send_message(msg, SIM_ENTITY_HIL);
			msg = NULL;

			break;
		}
		case SIM_EVENT_HELLO:		// Register FIS D2H�� ���۰� ���ÿ� �� (power on reset ���� SSD�� �غ�Ǿ��ٴ� �ǹ�)
		{
			ASSERT(current_sata_state == SHS_INIT);
			next_sata_state = SHS_IDLE;
			INSIGHT(g_insight.cmd_busy = FALSE; g_insight.link_busy = FALSE);
			g_host.idle_begin = current_time;
			break;
		}
		case SIM_EVENT_DELAY:
		{
			g_host.delay_pending = FALSE;
			break;
		}
		default:
		{
			ASSERT(FAIL);
		}
	}

	if (msg != NULL)
	{
		sim_release_message_slot(msg);
	}

	g_host.sata_state = next_sata_state;
}

static void purge_messages(void)
{
	V32(g_sim_context.thread_sync) = TRUE;

	while (1)
	{
		sim_message_t* msg = sim_receive_message(SIM_ENTITY_HOST);

		if (msg == NULL)
		{
			if (V32(g_sim_context.thread_sync) == FALSE)
			{
				// ���̻� �߻��� �̺�Ʈ�� ���� ���� ��Ȳ���� sim_main()�� SIM_ENTITY_HOST �� �����־���.
				break;
			}
		}
		else
		{
			// ó������ ���� ����
			sim_release_message_slot(msg);
		}
	}

	ASSERT(V32(g_sim_context.num_free_msg_slots) == MESSAGE_POOL_SIZE && V32(g_sim_context.num_waiting_entities) == NUM_ENTITIES - 1);
}

static void host_begin_simulation(void)
{
	STOSQ(&g_host, 0, sizeof(g_host));

	#if OPTION_VERIFY_DATA
	{
		UINT32 num_bytes = ROUND_UP(NUM_LSECTORS, sizeof(UINT64));	// �� ���ʹ� �� ����Ʈ; STOSQ�� ����ϱ� ���� 8 ����Ʈ�� ��� Ȯ��

		g_local_mem = (UINT8*) VirtualAlloc(NULL, num_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

		CHECK(g_local_mem != NULL);									// PC �޸𸮰� �����ؼ� ����� �����̹Ƿ� ���� �޸𸮸� �ÿ��� �ذ�

		STOSQ(g_local_mem, 0, NUM_LSECTORS);
	}
	#endif

	g_random = new mt19937(g_sim_context.random_seed);
}

static void host_end_simulation(void)
{
	for (UINT32 entity = 0; entity < NUM_ENTITIES; entity++)
	{
		if (entity != SIM_ENTITY_HOST)
		{
			sim_message_t* msg = sim_new_message();
			msg->code = (UINT16) SIM_EVENT_END_SIMULATION;
			msg->when = g_sim_context.current_time;
			sim_send_message(msg, entity);
		}
	}

	purge_messages();

	#if OPTION_VERIFY_DATA
	VirtualFree(g_local_mem, 0, MEM_RELEASE);
	#endif

	printf("\n\nSimulation ended.\n");
}

static void host_begin_session(void)
{
	// ������ ����

	g_host.num_pending_cmds = 0;
	g_host.free_slot_ptr = g_host.cmd_table;
	g_host.sata_state = SHS_INIT;
	g_host.prev_count = 256;
	g_host.prev_lba = 0;
	g_host.seq_lba = 0;
	g_host.current_cmd = NULL;
	g_host.hot_spot_enable = FALSE;
	g_host.aligned_read_p = 1024;
	g_host.aligned_write_p = 1024;
	g_host.max_queue_depth = NCQ_SIZE;
	g_host.random_lba_min = 0;
	g_host.random_lba_max = NUM_LSECTORS - 1;

	for (UINT32 i = 0; i < NCQ_SIZE; i++)
	{
		sim_sata_cmd_t* table_slot = g_host.cmd_table + i;

		table_slot->code = FF8;
		table_slot->ncq_tag = (UINT8) i;

		if (i == NCQ_SIZE - 1)
		{
			table_slot->link_ptr = NULL;
		}
		else
		{
			table_slot->link_ptr = table_slot + 1;
		}
	}

	// �ùķ��̼ǿ����� ���� �ʱ�ȭ ���� (���� �ϵ����ʹ� �ٸ�)
	//
	// 1. HOST�� NAND���� SIM_EVENT_POWER_ON �޽����� ������.
	// 2. NAND�� �ʱ�ȭ�� ��ġ�� ���� FIL���� SIM_EVENT_POWER_ON �޽����� �����Ѵ�.
	// 3. FIL�� �ʱ�ȭ�� ��ġ�� ���� FTL���� SIM_EVENT_POWER_ON �޽����� �����Ѵ�.
	// 4. FTL�� �� �ʿ��� �ʱ�ȭ ������ ������ ���� HIL���� SIM_EVENT_POWER_ON �޽����� �����Ѵ�.
	//    FTL �ʱ�ȭ �������� NAND �б�/���Ⱑ �ʿ��ϴٸ�, �ϴ� ���� SIM_EVENT_POWER_ON �޽����� �����ϰ� ���� NAND ������ �����Ѵ�.
	// 5. HIL�� �ʱ�ȭ�� ��ġ�� ���� FTL�κ��� FB_FTL_READY �� ��ٸ���.
	//    1���� 5������ ������ g_sim_context.current_time �� ������ �ʰ� ���� �ð��� ��� �̷������.
	// 6. �ð��� ����Ͽ� FTL�� �ʱ�ȭ�� ������ ��ġ�� ���� HIL���� FB_FTL_READY�� ������.
	// 7. HIL�� HOST���� SIM_EVENT_HELLO �޽����� ������.

	sim_message_t* msg = sim_new_message();
	msg->code = SIM_EVENT_POWER_ON;
	msg->when = g_sim_context.current_time;
	sim_send_message(msg, SIM_ENTITY_NAND);

	while (g_host.sata_state == SHS_INIT)
	{
		message_handler();	// SIM_EVENT_HELLO �޽����� ��ٸ�
	}

	HOST_STAT(reset_host_statistics());
}

static void host_end_session(void)
{
	for (UINT32 entity = 0; entity < NUM_ENTITIES; entity++)
	{
		if (entity != SIM_ENTITY_HOST)
		{
			sim_message_t* msg = sim_new_message();
			msg->code = (UINT16) SIM_EVENT_POWER_OFF;
			msg->when = g_sim_context.current_time;
			sim_send_message(msg, entity);
		}
	}

	purge_messages();
}

__forceinline sim_sata_cmd_t* get_free_cmd_slot(void)
{
	while (g_host.num_pending_cmds == g_host.max_queue_depth)
	{
		// ���� �������� SATA ����� ������ ���Ѽ��̹Ƿ� �߰� ����� ������ �� ����
		message_handler();
	}

	sim_sata_cmd_t* slot = g_host.free_slot_ptr;
	g_host.free_slot_ptr = slot->link_ptr;

	return slot;
}

static BOOL8 generate_read_range(sim_sata_cmd_t* new_cmd, UINT32 lba, UINT32 sector_count)
{
	switch (sector_count)
	{
		case RANDOM_SIZE:
		{
			new_cmd->num_sectors_requested = random(1, 64 * SECTORS_PER_SLICE);

			if (random(1, 1024) <= g_host.aligned_read_p)
			{
				new_cmd->num_sectors_requested = ROUND_UP(new_cmd->num_sectors_requested, SECTORS_PER_SLICE);
			}

			break;
		}
		case SAME_SIZE:
		{
			new_cmd->num_sectors_requested = g_host.prev_count;
			break;
		}
		default:
		{
			ASSERT(sector_count > 0 && sector_count <= 65536);
			new_cmd->num_sectors_requested = sector_count;
		}
	}

	switch (lba)
	{
		case RANDOM_LBA:
		{
			if (g_host.hot_spot_enable == TRUE)
			{
				UINT32 spot = random(0, NUM_HOT_SPOTS - 1);

				if (random(0, TEMPERATURE_MAX - 1) < g_host.temperature[spot])
				{
					lba = random(g_host.hot_spot[spot], g_host.hot_spot[spot] + HOT_SPOT_SIZE - 1);
				}
			}

			if (lba == RANDOM_LBA)
			{
				lba = random(g_host.random_lba_min, g_host.random_lba_max);
			}

			new_cmd->lba = (random(1, 1024) <= g_host.aligned_read_p) ? ROUND_DOWN(lba, SECTORS_PER_SLICE) : lba;

			break;
		}
		case SEQ_LBA:
		{
			new_cmd->lba = g_host.seq_lba;
			break;
		}
		case SAME_LBA:
		{
			new_cmd->lba = g_host.prev_lba;
			break;
		}
		default:
		{
			ASSERT(lba < NUM_LSECTORS);
			new_cmd->lba = lba;
		}
	}

	g_host.prev_count = new_cmd->num_sectors_requested;
	g_host.prev_lba = new_cmd->lba;
	g_host.seq_lba = new_cmd->lba + new_cmd->num_sectors_requested;

	if (g_host.seq_lba >= NUM_LSECTORS)
	{
		// �������� ����� g_host.prev_count�� �ݿ����� ���� (SAME_SIZE�� ���� �޶����� ���� ����)
		new_cmd->num_sectors_requested = NUM_LSECTORS - new_cmd->lba;
		g_host.seq_lba = 0;
	}

	g_host.loop_sector_count += new_cmd->num_sectors_requested;

	#if OPTION_VERIFY_DATA
	{
		for (UINT32 ncq_tag = 0; ncq_tag < NCQ_SIZE; ncq_tag++)
		{
			if (ncq_tag == new_cmd->ncq_tag)
				continue;

			sim_sata_cmd_t* cmd = g_host.cmd_table + ncq_tag;

			if (cmd->code != REQ_HOST_WRITE)
				continue;

			if (new_cmd->lba < cmd->lba + cmd->num_sectors_requested && cmd->lba < new_cmd->lba + new_cmd->num_sectors_requested)
			{
				return TRUE;	// ������ �������� ��ɰ� LBA �ߺ�
			}
		}
	}
	#endif

	return FALSE;
}

static BOOL8 generate_write_range(sim_sata_cmd_t* new_cmd, UINT32 lba, UINT32 sector_count)
{
	switch (sector_count)
	{
		case RANDOM_SIZE:
		{
			new_cmd->num_sectors_requested = random(1, 64 * SECTORS_PER_SLICE);

			if (random(1, 1024) <= g_host.aligned_write_p)
			{
				new_cmd->num_sectors_requested = ROUND_UP(new_cmd->num_sectors_requested, SECTORS_PER_SLICE);
			}

			break;
		}
		case SAME_SIZE:
		{
			new_cmd->num_sectors_requested = g_host.prev_count;
			break;
		}
		default:
		{
			ASSERT(sector_count > 0 && sector_count <= 65536);
			new_cmd->num_sectors_requested = sector_count;
		}
	}

	switch (lba)
	{
		case RANDOM_LBA:
		{
			if (g_host.hot_spot_enable == TRUE)
			{
				UINT32 spot = random(0, NUM_HOT_SPOTS - 1);

				if (random(0, TEMPERATURE_MAX - 1) < g_host.temperature[spot])
				{
					lba = random(g_host.hot_spot[spot], g_host.hot_spot[spot] + HOT_SPOT_SIZE - 1);
				}
			}

			if (lba == RANDOM_LBA)
			{
				lba = random(g_host.random_lba_min, g_host.random_lba_max);
			}

			new_cmd->lba = (random(1, 1024) <= g_host.aligned_write_p) ? ROUND_DOWN(lba, SECTORS_PER_SLICE) : lba;

			break;
		}
		case SEQ_LBA:
		{
			new_cmd->lba = g_host.seq_lba;
			break;
		}
		case SAME_LBA:
		{
			new_cmd->lba = g_host.prev_lba;
			break;
		}
		default:
		{
			ASSERT(lba < NUM_LSECTORS);
			new_cmd->lba = lba;
		}
	}

	g_host.prev_count = new_cmd->num_sectors_requested;
	g_host.prev_lba = new_cmd->lba;
	g_host.seq_lba = new_cmd->lba + new_cmd->num_sectors_requested;

	if (g_host.seq_lba >= NUM_LSECTORS)
	{
		// �������� ����� g_host.prev_count�� �ݿ����� ���� (SAME_SIZE�� ���� �޶����� ���� ����)
		new_cmd->num_sectors_requested = NUM_LSECTORS - new_cmd->lba;
		g_host.seq_lba = 0;
	}

	g_host.loop_sector_count += new_cmd->num_sectors_requested;

	#if OPTION_VERIFY_DATA
	{
		for (UINT32 ncq_tag = 0; ncq_tag < NCQ_SIZE; ncq_tag++)
		{
			if (ncq_tag == new_cmd->ncq_tag)
				continue;

			sim_sata_cmd_t* cmd = g_host.cmd_table + ncq_tag;

			if (cmd->code != REQ_HOST_READ)
				continue;

			if (new_cmd->lba < cmd->lba + cmd->num_sectors_requested && cmd->lba < new_cmd->lba + new_cmd->num_sectors_requested)
			{
				return TRUE;	// ������ �������� ��ɰ� LBA �ߺ�
			}
		}
	}
	#endif

	return FALSE;
}

static void generate_trim_range(sim_sata_cmd_t* new_cmd, UINT32 lba, UINT32 sector_count)
{
	switch (sector_count)
	{
		case RANDOM_SIZE:
		{
			new_cmd->num_sectors_requested = random(1, 2048) * SECTORS_PER_SLICE;
			break;
		}
		case SAME_SIZE:
		{
			new_cmd->num_sectors_requested = g_host.prev_count;
			break;
		}
		default:
		{
			ASSERT(sector_count > 0 && sector_count <= 65536);
			new_cmd->num_sectors_requested = ROUND_UP(sector_count, SECTORS_PER_SLICE);
		}
	}

	switch (lba)
	{
		case RANDOM_LBA:
		{
			if (g_host.hot_spot_enable == TRUE)
			{
				UINT32 spot = random(0, NUM_HOT_SPOTS - 1);

				if (random(0, TEMPERATURE_MAX - 1) < g_host.temperature[spot])
				{
					lba = random(g_host.hot_spot[spot], g_host.hot_spot[spot] + HOT_SPOT_SIZE - 1);
				}
			}

			if (lba == RANDOM_LBA)
			{
				lba = random(g_host.random_lba_min, g_host.random_lba_max);
			}

			new_cmd->lba = ROUND_DOWN(lba, SECTORS_PER_SLICE);

			break;
		}
		case SEQ_LBA:
		{
			new_cmd->lba = g_host.seq_lba;
			break;
		}
		case SAME_LBA:
		{
			new_cmd->lba = g_host.prev_lba;
			break;
		}
		default:
		{
			ASSERT(lba < NUM_LSECTORS);
			new_cmd->lba = ROUND_DOWN(lba, SECTORS_PER_SLICE);
		}
	}

	g_host.prev_count = new_cmd->num_sectors_requested;
	g_host.prev_lba = new_cmd->lba;
	g_host.seq_lba = new_cmd->lba + new_cmd->num_sectors_requested;

	if (g_host.seq_lba >= NUM_LSECTORS)
	{
		// �������� ����� g_host.prev_count�� �ݿ����� ���� (SAME_SIZE�� ���� �޶����� ���� ����)
		new_cmd->num_sectors_requested = NUM_LSECTORS - new_cmd->lba;
		g_host.seq_lba = 0;
	}

	g_host.loop_sector_count += new_cmd->num_sectors_requested;
}

static void finish_all(void)
{
	while (g_host.num_pending_cmds != 0)
	{
		message_handler();
	}
}

static void internal_delay(UINT64 length)
{
	sim_message_t* msg = sim_new_message();
	msg->code = SIM_EVENT_DELAY;
	msg->when = g_sim_context.current_time + length;
	sim_send_message(msg, SIM_ENTITY_HOST);

	g_host.delay_pending = TRUE;

	do
	{
		message_handler();

	} while (g_host.delay_pending == TRUE);
}

static void send_sata_command(sim_sata_cmd_t* cmd)
{
	// NCQ ����� ó�� ����
	//
	// 1. ������ ������ ���� ����� ������ 32�̸�, 31�� �� ������ ��ٸ���.
	// 2. ncq_tag ���� �Ҵ��Ѵ�.
	// 3. ȣ��Ʈ�� HIL���� SIM_EVENT_SATA_CMD_S�� ������.
	// 4. ȣ��Ʈ�� ȣ��Ʈ���� SIM_EVENT_SATA_CMD_E�� ������.
	// 5. ȣ��Ʈ�� SIM_EVENT_SATA_CMD_E�� �޾Ƽ� HIL���� �����Ѵ�. ȣ��Ʈ�� ���� SHS_WAIT_ACK �����̴�.
	// 6. HIL�� SIM_EVENT_SATA_CMD_E�� ������ ȣ��Ʈ���� SIM_EVENT_SATA_ACK_S�� ������, ����� g_ftl_cmd_q�� �����Ѵ�.
	// 7. HIL�� HIL���� SIM_EVENT_SATA_ACK_E�� ������. �޽����� ����(arg_32)�� TRUE��, NCQ ����̶�� ���̴�.
	// 8. HIL�� SIM_EVENT_SATA_ACK_E�� �޾Ƽ� ȣ��Ʈ���� �����Ѵ�.
	// 9. ȣ��Ʈ�� SIM_EVENT_SATA_ACK_E�� �޾Ƽ� ���ڰ� TRUE �̸� NCQ ����̹Ƿ� remove_command()�� ȣ������ �ʴ´�.
	// 10. HIL�� ȣ��Ʈ ���̿� ������ �ۼ����� �̷������. (sim_hil.c�� receive_write_data() �� send_read_data()�� ���� ����)
	// 11. ������ �ۼ����� ������ HIL�� ȣ��Ʈ���� SIM_EVENT_SDB_S�� ������.
	// 12. HIL�� HIL���� SIM_EVENT_SDB_E�� ������. �޽����� ����(arg_32)�� ncq_tag ���̴�.
	// 13. ȣ��Ʈ�� SIM_EVENT_SDB_E�� �ް� NCQ ����� �������� �����ϰ� remove_command()�� ȣ���Ѵ�.

	// non-NCQ ����� ó�� ����
	//
	// 1. ������ ��ɵ��� ��� �Ϸ�� ������ ��ٸ���.
	// 2. ncq_tag ���� �ϳ� �Ҵ��Ѵ�. SATA �Ծ�� non-NCQ ��ɿ� ���ؼ��� NCQ tag ���̶�� ������ �ƿ� ������, ���ǻ� ncq_tag ���� �ϳ� �Ҵ��ϱ�� �Ѵ�.
	//    ������ ������ ����� ��� �Ϸ�Ǿ NCQ�� ��� �����Ƿ� � tag ���� ����ϴ� ����� ����.
	// 3. ȣ��Ʈ�� HIL���� SIM_EVENT_SATA_CMD_S�� ������.
	// 4. ȣ��Ʈ�� ȣ��Ʈ���� SIM_EVENT_SATA_CMD_E�� ������.
	// 5. ȣ��Ʈ�� SIM_EVENT_SATA_CMD_E�� �޾Ƽ� HIL���� �����Ѵ�. ȣ��Ʈ�� ���� SHS_WAIT_ACK �����̴�.
	// 6. HIL�� FTL���� ����� �����Ѵ�.
	// 7. FTL�� ��� ó���� ������ HIL���� FB_XXX_DONE�� ������. FB_XXX_DONE�� FTL�� non-NCQ ����� ������ ���� �߻��ϴ� feedback�̴�.
	// 8. HIL�� FB_XXX_DONE�� �ް� HOST���� SIM_EVENT_SATA_ACK_S�� ������.
	// 9. HIL�� HIL���� SIM_EVENT_SATA_ACK_E�� ������. �޽����� ����(arg_32)�� FALSE��, non-NCQ ����̶�� ���̴�.
	// 10. HIL�� SIM_EVENT_SATA_ACK_E�� �޾Ƽ� ȣ��Ʈ���� �����Ѵ�.
	// 11. ȣ��Ʈ�� SIM_EVENT_SATA_ACK_E�� �޾Ƽ� ���ڸ� ���� non-NCQ ����� �������� �����ϰ� remove_command()�� ȣ���Ѵ�.

	g_host.last_ncq_tag = cmd->ncq_tag;

	ASSERT(g_host.num_pending_cmds < NCQ_SIZE);
	g_host.num_pending_cmds++;

	while (1)
	{
		// SHS_IDLE ���¿� �������� ��� SIM_SATA_CMD_DELAY ��ŭ�� �ð��� ������� ���� ����� ������.
		// �ð��� ����ϴ� ���ȿ� SHS_IDLE ���¿��� ���� �����ٸ� ���� ���� ��ٷ��� ��

		if (g_host.sata_state == SHS_IDLE)
		{
			if (g_sim_context.current_time >= g_host.idle_begin + SIM_SATA_CMD_DELAY)
			{
				break;
			}
			else
			{
				sim_message_t* msg = sim_new_message();
				msg->code = SIM_EVENT_DELAY;
				msg->when = g_host.idle_begin + SIM_SATA_CMD_DELAY;
				sim_send_message(msg, SIM_ENTITY_HOST);
			}
		}

		message_handler();
	}

	while (1)	// collision ���� ����� ������ ���� ������ �ݺ� �õ�
	{
		g_host.collision = FALSE;
		g_host.sata_state = SHS_CMD;

		UINT64 current_time = g_sim_context.current_time;

		cmd->submit_time = current_time;
		cmd->num_sectors_completed = 0;

		INSIGHT(g_insight.cmd_busy = TRUE, g_insight.link_busy = TRUE);
		HOST_STAT(g_host_stat.link_idle_time += current_time - g_host.idle_begin);

		sim_message_t* msg = sim_new_message();
		msg->code = SIM_EVENT_SATA_CMD_S;
		msg->when = current_time;
		PRINT_SATA_HOST("HOST %llu: SIM_EVENT_SATA_CMD_S, %llu, %u\n", current_time, msg->when, msg->seq_number);
		sim_send_message(msg, SIM_ENTITY_HIL);

		msg = sim_new_message();
		msg->code = SIM_EVENT_SATA_CMD_E;
		msg->when = current_time + SIM_SATA_CMD_TIME;
		msg->arg_32 = cmd->ncq_tag;
		msg->arg_64 = (UINT64) cmd;
		PRINT_SATA_HOST("HOST %llu: SIM_EVENT_SATA_CMD_E, %llu, %u\n", current_time, msg->when, msg->seq_number);
		sim_send_message(msg, SIM_ENTITY_HOST);

		g_host.last_cmd = msg;

		do
		{
			message_handler();

		} while (g_host.sata_state == SHS_CMD);

		if (g_host.sata_state == SHS_WAIT_ACK)
		{
			break; // ����� ������ ������
		}

		ASSERT(g_host.collision == TRUE);

		g_host.last_cmd->arg_64 = NULL;		// �ش� �޽����� �޾��� ���� �����ϱ� ���� ǥ��

		while (g_host.sata_state != SHS_IDLE)
		{
			message_handler();
		}

		PRINT_SATA_HOST("HOST %llu: resending\n", current_time);
	}
}

char* g_script_name;
#define RUN_SCRIPT(X)	{ g_script_name = #X; run(X); }

static void run(UINT32* script_ptr)
{
	// �� ���ǿ� �� �̻��� ��ũ��Ʈ�� �����ϴ� ���� �����ϴ�.
	// �� ��ũ��Ʈ���� �� �̻��� ������ �����ϴ� ���� �Ұ����ϴ�.
	// END_OF_SCRIPT�� ������ ���� �������� ��ɵ��� ���������� �ʰ� (num_pending_cmds == 0 �� ������ ��ٸ��� �ʰ�) �����Ѵ�.
	// �׷��� ������ ������, �� ���ǿ��� �� �̻��� ��ũ��Ʈ�� run�� ��� ��ũ��Ʈ ���̿� �ð� ���� ���� �Ų��ϰ� ���������ν�
	// ��ġ �ϳ��� ū ��ũ��Ʈ�� �������� ���� ���� ����� ��� �����̴�.
	// ��� ����� �Ϸ�Ǳ⸦ ���Ѵٸ� END_OF_SCRIPT�� �տ� FINISH_ALL, FAST_FLUSH, SLOW_FLUSH ���� ������ �ȴ�.

	while (1)
	{
		UINT32 lba = 0, sector_count = 0;
		BOOL32 random_cmd = FALSE;
		UINT32 instruction = *script_ptr++;

		if (instruction == RANDOM_CMD)
		{
			UINT32 temp = random(0, g_host.cmd_p_sum - 1);

			for (UINT32 i = 0; i < RANDOM_CMD; i++)
			{
				if (temp < g_host.cmd_p[i])
				{
					instruction = i;
					break;
				}
			}

			lba = RANDOM_LBA;
			sector_count = RANDOM_SIZE;
			random_cmd = TRUE;
		}

		switch (instruction)
		{
			case READ:
			{
				if (random_cmd == FALSE)
				{
					lba = *script_ptr++;
					sector_count = *script_ptr++;
				}

				sim_sata_cmd_t* new_cmd = get_free_cmd_slot();
				new_cmd->code = (UINT8) instruction;

				BOOL8 overlap = generate_read_range(new_cmd, lba, sector_count);

				if (overlap == TRUE)
				{
					finish_all();	// ambiguity�� ȸ���ϱ� ���� ������ ��ɵ��� ���� �Ϸ�
				}

				send_sata_command(new_cmd);
				break;
			}
			case WRITE:
			{
				if (random_cmd == FALSE)
				{
					lba = *script_ptr++;
					sector_count = *script_ptr++;
				}

				sim_sata_cmd_t* new_cmd = get_free_cmd_slot();
				new_cmd->code = (UINT8) instruction;

				BOOL8 overlap = generate_write_range(new_cmd, lba, sector_count);

				if (overlap == TRUE)
				{
					finish_all();	// ambiguity�� ȸ���ϱ� ���� ������ ��ɵ��� ���� �Ϸ�
				}

				send_sata_command(new_cmd);

				break;
			}
			case TRIM:
			{
				// non-NCQ command �̹Ƿ� ������ �ٸ� ��ɰ� �ð������� ��ġ�� �κ��� ����� �Ѵ�.

				finish_all();	// TRIM ��� ������ ���� ������ NCQ ����� ��� �Ϸ�Ǳ⸦ ��ٸ�

				if (random_cmd == FALSE)
				{
					lba = *script_ptr++;
					sector_count = *script_ptr++;
				}

				sim_sata_cmd_t* new_cmd = get_free_cmd_slot();
				new_cmd->code = (UINT8) instruction;

				generate_trim_range(new_cmd, lba, sector_count);

				// TRIM �� non-NCQ ����̹Ƿ� write�ʹ� �޸� overlap �˻縦 �� �ʿ䰡 ����

				send_sata_command(new_cmd);

				#if OPTION_VERIFY_DATA
				memset(g_local_mem + new_cmd->lba, 0, new_cmd->num_sectors_requested);
				#endif

				finish_all();

				break;
			}
			case FAST_FLUSH:	// FLUSH
			case SLOW_FLUSH:	// STANDBY_IMMEDIATE
			{
				finish_all();	// TRIM�� ���������� non-NCQ ���

				sim_sata_cmd_t* new_cmd = get_free_cmd_slot();
				new_cmd->code = (UINT8) instruction;
				send_sata_command(new_cmd);
				finish_all();

				break;
			}
			case FINISH_ALL:	// ������ ���� ��� ����� �Ϸ�� ������ ��ٸ�
			{
				finish_all();
				break;
			}
			case NOP:
			{
				UINT32 duration = RANDOM_SIZE;

				if (random_cmd == FALSE)
				{
					duration = *script_ptr++;
				}

				if (duration == RANDOM_SIZE)
				{
					duration = random(g_host.nop_period_min, g_host.nop_period_max);	// ����: ��
				}

				internal_delay(duration * 1000000000ULL);

				break;
			}
			case BEGIN_LOOP:
			{
				UINT32 loop_mode = *script_ptr++;
				UINT32 length = *script_ptr++;

				g_host.loop_begin = script_ptr;

				switch (loop_mode)
				{
					case CYCLES:
					{
						g_host.loop_mode = CYCLES;
						g_host.loop_cycle_count = 0;
						g_host.loop_end = length - 1;
						break;
					}
					case SECONDS:
					{
						g_host.loop_mode = SECONDS;
						g_host.loop_end = g_sim_context.current_time + 1000000000ULL * length;
						break;
					}
					case MINUTES:
					{
						g_host.loop_mode = SECONDS;
						g_host.loop_end = g_sim_context.current_time + 1000000000ULL * 60 * length;
						break;
					}
					case HOURS:
					{
						g_host.loop_mode = SECONDS;
						g_host.loop_end = g_sim_context.current_time + 1000000000ULL * 60 * 60 * length;
						break;
					}
					case SECTORS:
					{
						g_host.loop_mode = SECTORS;
						g_host.loop_sector_count = 0;
						g_host.loop_end = length;
						break;
					}
					case MEGABYTES:
					{
						g_host.loop_mode = SECTORS;
						g_host.loop_sector_count = 0;
						g_host.loop_end = 2048ULL * length;
						break;
					}
					case GIGABYTES:
					{
						g_host.loop_mode = SECTORS;
						g_host.loop_sector_count = 0;
						g_host.loop_end = 2097152ULL * length;
						break;
					}
					case FOREVER:
					{
						g_host.loop_mode = FOREVER;
						break;
					}
					default:
					{
						ASSERT(FAIL);
					}
				}

				break;
			}
			case END_LOOP:
			{
				switch (g_host.loop_mode)
				{
					case CYCLES:
					{
						if (g_host.loop_cycle_count < g_host.loop_end)
						{
							g_host.loop_cycle_count++;
							script_ptr = g_host.loop_begin;
						}

						break;
					}
					case SECONDS:
					{
						if (g_sim_context.current_time < g_host.loop_end)
						{
							script_ptr = g_host.loop_begin;
						}

						break;
					}
					case SECTORS:
					{
						if (g_host.loop_sector_count < g_host.loop_end)
						{
							script_ptr = g_host.loop_begin;
						}

						break;
					}
					case FOREVER:
					{
						script_ptr = g_host.loop_begin;
						break;
					}
					default:
					{
						ASSERT(FAIL);
					}
				}

				break;
			}
			case ALIGN:
			{
				g_host.aligned_read_p = (UINT16) (*script_ptr++);
				g_host.aligned_write_p = (UINT16) (*script_ptr++);

				ASSERT(g_host.aligned_read_p <= 1024);
				ASSERT(g_host.aligned_write_p <= 1024);

				break;
			}
			case SET_LBA:
			{
				lba = *script_ptr++;

				if (lba == RANDOM_LBA)
				{
					lba = ROUND_DOWN(random(g_host.random_lba_min, g_host.random_lba_max), SECTORS_PER_SLICE);
				}

				g_host.prev_lba = lba;
				g_host.seq_lba = lba;

				break;
			}
			case SET_LBA_RANGE:
			{
				g_host.random_lba_min = *script_ptr++;
				g_host.random_lba_max = *script_ptr++;
				g_host.hot_spot_enable = FALSE;
				break;
			}
			case SET_NOP_PERIOD:
			{
				g_host.nop_period_min = (UINT8) (*script_ptr++);
				g_host.nop_period_max = (UINT8) (*script_ptr++);
				break;
			}
			case SET_CMD_P:
			{
				// RANDOM_CMD�� ���� �� ����� Ȯ�� ����

				UINT32 sum = 0;

				for (UINT32 i = 0; i < RANDOM_CMD; i++)
				{
					UINT32 inst = *script_ptr++;
					UINT32 p = *script_ptr++;
					g_host.cmd_p[inst] = p;

					sum += p;
				}

				ASSERT(IS_POWER_OF_TWO(sum));
				g_host.cmd_p_sum = sum;

				for (UINT32 i = 1; i < RANDOM_CMD; i++)
				{
					g_host.cmd_p[i] += g_host.cmd_p[i - 1];
				}

				break;
			}
			case SET_MAX_QD:
			{
				g_host.max_queue_depth = (UINT16) (*script_ptr++);
				ASSERT(g_host.max_queue_depth != 0 && g_host.max_queue_depth <= NCQ_SIZE);
				break;
			}
			case ENABLE_HOT_SPOT:
			{
				if (g_host.random_lba_max - g_host.random_lba_min > HOT_SPOT_SIZE)
				{
					g_host.hot_spot_enable = TRUE;

					for (UINT32 i = 0; i < NUM_HOT_SPOTS; i++)
					{
						#if NUM_LSECTORS < HOT_SPOT_SIZE
						#error
						#endif

						g_host.hot_spot[i] = ROUND_DOWN(random(g_host.random_lba_min, g_host.random_lba_max - HOT_SPOT_SIZE), SECTORS_PER_SLICE);
						g_host.temperature[i] = random(1, TEMPERATURE_MAX);
					}
				}

				break;
			}
			case DISABLE_HOT_SPOT:
			{
				g_host.hot_spot_enable = FALSE;
				break;
			}
			case STOP:
			{
				__debugbreak();
				break;
			}
			case PRINT_STAT:
			{
				printf("------------------------------------------------------------------------------------------------------\n");
				printf("%s\n\n", g_script_name);

				HOST_STAT(print_host_statistics(&g_host_stat));
				HOST_STAT(reset_host_statistics());

				sim_message_t* msg = sim_new_message();
				msg->code = SIM_EVENT_PRINT_STAT;
				msg->when = g_sim_context.current_time;
				sim_send_message(msg, SIM_ENTITY_FTL);

				purge_messages();	// ��ũ��Ʈ���� PRINT_STAT ������ FLUSH �ϹǷ�, ������ �������� �޽����� ����.

				break;
			}
			case BEGIN_INSIGHT:
			{
				UINT32 max_file_size = *script_ptr++;
				insight_begin(max_file_size);
				break;
			}
			case END_INSIGHT:
			{
				insight_end();
				break;
			}
			case END_OF_SCRIPT:
			{
				return;
			}
			default:
			{
				ASSERT(FAIL);
			}
		}
	}
}

void sim_host_thread(void* arg_list)
{
	UNREFERENCED_PARAMETER(arg_list);

	host_begin_simulation();

	// ���� session�� Ư���� session ���μ�, SSD internal format �� �����Ѵ�. (���� �ʱ�ȭ)
	// session #0 �� ���۵Ǹ� FTL�� internal format�� �����ϰ�, �Ϸ�Ǹ� SIM_EVENT_HELLO �� ȣ��Ʈ���� ������.
	// ȣ��Ʈ�� SIM_EVENT_HELLO �� �ް� �ƹ��� SATA ��ɵ� ������ �ʰ� session #1���� �Ѿ��.
	// session #1���ʹ� FTL internal booting�� �Ϸ�Ǿ��� ���� SIM_EVENT_HELLO�� �߻��ϰ�, ȣ��Ʈ�� ���������� SATA ����� ������.

	g_sim_context.session = 0;
	host_begin_session();
	host_end_session();

	for (UINT32 session = 1; session <= 100; session++)
	{
		// �� session�� power cycle�� �ǹ��Ѵ�.
		// session�� ���۵Ǹ� ftl_open() �Լ��� ����ǰ� FTL ���� ������ �����Ѵ�. power loss recovery ���۵� �̷������.
		// FTL ���� ������ ������ ȣ��Ʈ���� SIM_EVENT_HELLO �� ������. ȣ��Ʈ�� SIM_EVENT_HELLO �� �ޱ� �������� SATA ����� ������ �ʰ� ��ٸ���.
		// session�� ������ ���� ������ SATA ����� SLOW FLUSH (SATA���� ����ϴ� ���� ��Ī�� STAND BY IMMEDIATE) �̸� graceful shutdown (safe shutdown) �̴�.

		g_sim_context.session = session;
		host_begin_session();

		if (session == 1)
		{
			RUN_SCRIPT(scr_sw_16gb);
			RUN_SCRIPT(scr_sr_16gb);
			RUN_SCRIPT(scr_rw_32gb);
			RUN_SCRIPT(scr_sr_16gb);
			RUN_SCRIPT(scr_rr_16gb);
		}
		else
		{
			#if 1
			{
				if (session > 1)
				{
					RUN_SCRIPT(scr_read_all);
				}

				RUN_SCRIPT(scr_prepare_1);
				RUN_SCRIPT(scr_random);
			}
			#else
			{
				RUN_SCRIPT(scr_standard_test);
			}
			#endif
		}

		host_end_session();
	}

	host_end_simulation();
}

