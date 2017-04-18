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

	SHS_INIT,			// Register FIS D2H (hello) 기다리는 중
	SHS_IDLE,			// SATA IDLE 상태
	SHS_CMD,			// Register FIS H2D (command) 전송중
	SHS_DATA_H2D,		// DATA FIS H2D 전송중
	SHS_DATA_D2H,		// 디바이스로부터 DATA FIS D2H 수신중
	SHS_ACK,			// 디바이스로부터 Register FIS D2H (ack) 수신중
	SHS_DMA_SETUP,		// 디바이스로부터 DMA SETUP FIS 수신중
	SHS_DMA_ACTIVATE,	// 디바이스로부터 DMA ACTIVATE FIS 수신중
	SHS_WAIT_ACK,		// Register FIS D2H (ack) 기다리는 중
	SHS_WAIT_DMA_ACT,	// DMA ACTIVATE FIS 기다리는 중
	SHS_WAIT_DATA, 		// DATA FIS D2H 기다리는 중
	SHS_SDB,			// 디바이스로부터 Set Device Bits FIS 수신중
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
	UINT16	aligned_read_p;			// read 명령의 시작/끝 LBA가 4KB 경계선에 맞을 확률 (1024 = 100%)
	UINT16	aligned_write_p;		// write 명령의 시작/끝 LBA가 4KB 경계선에 맞을 확률 (1024 = 100%)

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

	UINT64	idle_begin;		// SATA link layer의 idle 상태가 시작된 시각
	sim_message_t* last_cmd;

	UINT32	data_seq;

	UINT8	last_ncq_tag;	// 가장 최근에 보냈던 명령의 ncq_tag 값
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
	// 이 함수를 호출할 때마다 하나의 data FIS (최대 8KB)를 디바이스에게 전송

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
	// 종료된 명령을 테이블에서 삭제

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
		case SIM_EVENT_SATA_CMD_E:	// Register FIS H2D의 끝
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
				// PCIe/NVMe와 달리 SATA는 half duplex이다.
				// SATA IDLE 상태에 진입하는 순간 호스트는 새로운 명령을 보낼 수 있고,
				// 디바이스는 기존 명령에 대한 DMA SETUP FIS 또는 Set Device Bits FIS를 보낼 수 있기 때문에 상호 충돌 가능성이 있다.
				// SATA bus collision이 발생하면 호스트가 양보하도록 SATA 규약에 명시되어 있으므로, 나중에 SHS_IDLE 상태가 되면 재시도 할 것이다.

				PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SATA_CMD_E collision, %llu, %u\n", current_time, msg->when, msg->seq_number);

				ASSERT(msg->arg_64 == NULL || g_host.collision == TRUE);
			}

			break;
		}
		case SIM_EVENT_SATA_ACK_S:	// Register FIS D2H의 시작
		{
			PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SATA_ACK_S, %llu, %u\n", current_time, msg->when, msg->seq_number);

			ASSERT(current_sata_state == SHS_WAIT_ACK);
			next_sata_state = SHS_ACK;
			INSIGHT(g_insight.link_busy = TRUE);
			HOST_STAT(g_host_stat.link_idle_time += current_time - g_host.idle_begin);
			break;
		}
		case SIM_EVENT_SATA_ACK_E:	// Register FIS D2H의 끝
		{
			PRINT_SATA_HOST("HOST %llu: received SIM_EVENT_SATA_ACK_E, %llu, %u\n", current_time, msg->when, msg->seq_number);

			ASSERT(current_sata_state == SHS_ACK);
			next_sata_state = SHS_IDLE;

			INSIGHT(g_insight.cmd_busy = FALSE, g_insight.link_busy = FALSE);
			g_host.idle_begin = current_time;

			if (msg->arg_32 == FALSE)	// non-NCQ 명령이 완료되었음, send_sata_command()의 설명 참고
			{
				remove_command(g_host.last_ncq_tag);
			}

			break;
		}
		case SIM_EVENT_SDB_S:		// Set Device Bits FIS의 시작
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
		case SIM_EVENT_SDB_E:		// Set Device Bits FIS의 끝
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

				remove_command(ncq_tag);	// NCQ 명령이 완료되었음, send_sata_command()의 설명 참고
			}

			break;
		}
		case SIM_EVENT_DMA_SETUP_S:	// DMA SETUP FIS의 시작 (read or write)
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
		case SIM_EVENT_DMA_SETUP_E:	// DMA SETUP FIS의 끝 (read or write)
		{
			// 이 호스트 모델은 non-zero buffer offset을 지원하지 않는다.
			// 디바이스가 커맨드를 선택하는 순서는 자유지만, 일단 하나를 골라서 DMA SETUP FIS를 보냈으면 해당 커맨드의 데이터 전송이 한방에 완료되어야 한다.

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
		case SIM_EVENT_DMA_ACTV_S:	// DMA ACTIVATE FIS의 시작 (write)
		{
			ASSERT(current_sata_state == SHS_WAIT_DMA_ACT);
			next_sata_state = SHS_DMA_ACTIVATE;
			INSIGHT(g_insight.link_busy = TRUE);
			HOST_STAT(g_host_stat.link_idle_time += current_time - g_host.idle_begin);
			break;
		}
		case SIM_EVENT_DMA_ACTV_E:	// DMA ACTIVATE FIS의 끝 (write)
		{
			ASSERT(current_sata_state == SHS_DMA_ACTIVATE);
			next_sata_state = SHS_DATA_H2D;

			ASSERT(g_host.current_cmd != NULL);	// DMA SETUP FIS를 받을 때에 설정되었음
			sim_sata_cmd_t* cmd = g_host.current_cmd;
			ASSERT(cmd->num_sectors_completed > 0 && cmd->num_sectors_completed < cmd->num_sectors_requested);
			ASSERT(cmd->code == REQ_HOST_WRITE);

			send_write_data((UINT8*) msg->arg_64, cmd);

			break;
		}
		case SIM_EVENT_RDATA_S:		// DATA FIS의 시작 (read)
		{
			ASSERT(current_sata_state == SHS_WAIT_DATA);
			next_sata_state = SHS_DATA_D2H;

			INSIGHT(g_insight.link_busy = TRUE);
			HOST_STAT(g_host_stat.link_idle_time += current_time - g_host.idle_begin);

			ASSERT(g_host.current_cmd != NULL);	// DMA SETUP FIS를 받을 때에 설정되었음
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
		case SIM_EVENT_RDATA_E:		// DATA FIS의 끝 (read)
		{
			ASSERT(current_sata_state == SHS_DATA_D2H);

			sim_sata_cmd_t* cmd = g_host.current_cmd;

			if (cmd->num_sectors_completed == cmd->num_sectors_requested)
			{
				// 이 커맨드는 사실상 완료되었으나, 디바이스가 Set Device Bits를 보내야 비로소 완료된 것으로 간주

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
		case SIM_EVENT_WDATA_E:		// DATA FIS의 끝 (write)
		{
			ASSERT(current_sata_state == SHS_DATA_H2D);

			ASSERT(g_host.current_cmd != NULL);	// DMA SETUP FIS를 받을 때에 설정되었음
			sim_sata_cmd_t* cmd = g_host.current_cmd;

			ASSERT(cmd->code == REQ_HOST_WRITE);

			UINT32 num_sectors = msg->arg_32;
			ASSERT(num_sectors > 0 && num_sectors <= DATA_FIS_SIZE_MAX);
			cmd->num_sectors_completed += num_sectors;

			if (cmd->num_sectors_completed == cmd->num_sectors_requested)
			{
				// 이 명령은 사실상 완료되었으나, 디바이스가 Set Device Bits를 보내야 비로소 완료된 것으로 간주

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
		case SIM_EVENT_HELLO:		// Register FIS D2H의 시작과 동시에 끝 (power on reset 이후 SSD가 준비되었다는 의미)
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
				// 더이상 발생할 이벤트가 남지 않은 상황에서 sim_main()이 SIM_ENTITY_HOST 를 깨워주었다.
				break;
			}
		}
		else
		{
			// 처리하지 말고 버림
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
		UINT32 num_bytes = ROUND_UP(NUM_LSECTORS, sizeof(UINT64));	// 한 섹터당 한 바이트; STOSQ를 사용하기 위해 8 바이트의 배수 확보

		g_local_mem = (UINT8*) VirtualAlloc(NULL, num_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

		CHECK(g_local_mem != NULL);									// PC 메모리가 부족해서 생기는 문제이므로 가상 메모리를 늘여서 해결

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
	// 전원이 켜짐

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

	// 시뮬레이션에서의 전원 초기화 과정 (실제 하드웨어와는 다름)
	//
	// 1. HOST가 NAND에게 SIM_EVENT_POWER_ON 메시지를 보낸다.
	// 2. NAND가 초기화를 마치고 나서 FIL에게 SIM_EVENT_POWER_ON 메시지를 전달한다.
	// 3. FIL이 초기화를 마치고 나서 FTL에게 SIM_EVENT_POWER_ON 메시지를 전달한다.
	// 4. FTL이 꼭 필요한 초기화 절차만 마지고 나서 HIL에게 SIM_EVENT_POWER_ON 메시지를 전달한다.
	//    FTL 초기화 과정에서 NAND 읽기/쓰기가 필요하다면, 일단 먼저 SIM_EVENT_POWER_ON 메시지를 전달하고 나서 NAND 접근을 시작한다.
	// 5. HIL이 초기화를 마치고 나서 FTL로부터 FB_FTL_READY 를 기다린다.
	//    1부터 5까지의 과정은 g_sim_context.current_time 이 변하지 않고 같은 시각에 모두 이루어진다.
	// 6. 시간이 경과하여 FTL이 초기화를 완전히 마치고 나면 HIL에게 FB_FTL_READY를 보낸다.
	// 7. HIL이 HOST에게 SIM_EVENT_HELLO 메시지를 보낸다.

	sim_message_t* msg = sim_new_message();
	msg->code = SIM_EVENT_POWER_ON;
	msg->when = g_sim_context.current_time;
	sim_send_message(msg, SIM_ENTITY_NAND);

	while (g_host.sata_state == SHS_INIT)
	{
		message_handler();	// SIM_EVENT_HELLO 메시지를 기다림
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
		// 현재 진행중인 SATA 명령의 개수가 상한선이므로 추가 명령을 생성할 수 없음
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
		// 재조정한 결과는 g_host.prev_count에 반영되지 않음 (SAME_SIZE의 값이 달라지는 것을 방지)
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
				return TRUE;	// 기존에 진행중인 명령과 LBA 중복
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
		// 재조정한 결과는 g_host.prev_count에 반영되지 않음 (SAME_SIZE의 값이 달라지는 것을 방지)
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
				return TRUE;	// 기존에 진행중인 명령과 LBA 중복
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
		// 재조정한 결과는 g_host.prev_count에 반영되지 않음 (SAME_SIZE의 값이 달라지는 것을 방지)
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
	// NCQ 명령의 처리 과정
	//
	// 1. 기존에 끝나지 않은 명령의 개수가 32이면, 31이 될 때까지 기다린다.
	// 2. ncq_tag 값을 할당한다.
	// 3. 호스트가 HIL에게 SIM_EVENT_SATA_CMD_S를 보낸다.
	// 4. 호스트가 호스트에게 SIM_EVENT_SATA_CMD_E를 보낸다.
	// 5. 호스트가 SIM_EVENT_SATA_CMD_E를 받아서 HIL에게 전달한다. 호스트는 이제 SHS_WAIT_ACK 상태이다.
	// 6. HIL이 SIM_EVENT_SATA_CMD_E를 받으면 호스트에게 SIM_EVENT_SATA_ACK_S를 보내고, 명령을 g_ftl_cmd_q에 삽입한다.
	// 7. HIL이 HIL에게 SIM_EVENT_SATA_ACK_E를 보낸다. 메시지의 인자(arg_32)는 TRUE로, NCQ 명령이라는 뜻이다.
	// 8. HIL이 SIM_EVENT_SATA_ACK_E를 받아서 호스트에게 전달한다.
	// 9. 호스트는 SIM_EVENT_SATA_ACK_E를 받아서 인자가 TRUE 이면 NCQ 명령이므로 remove_command()를 호출하지 않는다.
	// 10. HIL과 호스트 사이에 데이터 송수신이 이루어진다. (sim_hil.c의 receive_write_data() 및 send_read_data()의 설명 참고)
	// 11. 데이터 송수신이 끝나면 HIL이 호스트에게 SIM_EVENT_SDB_S를 보낸다.
	// 12. HIL이 HIL에게 SIM_EVENT_SDB_E를 보낸다. 메시지의 인자(arg_32)는 ncq_tag 값이다.
	// 13. 호스트는 SIM_EVENT_SDB_E를 받고서 NCQ 명령이 끝났음을 인지하고 remove_command()를 호출한다.

	// non-NCQ 명령의 처리 과정
	//
	// 1. 기존의 명령들이 모두 완료될 때까지 기다린다.
	// 2. ncq_tag 값을 하나 할당한다. SATA 규약상 non-NCQ 명령에 대해서는 NCQ tag 값이라는 개념이 아예 없지만, 편의상 ncq_tag 값을 하나 할당하기로 한다.
	//    어차피 기존의 명령이 모두 완료되어서 NCQ가 비어 있으므로 어떤 tag 값을 사용하던 상관은 없다.
	// 3. 호스트가 HIL에게 SIM_EVENT_SATA_CMD_S를 보낸다.
	// 4. 호스트가 호스트에게 SIM_EVENT_SATA_CMD_E를 보낸다.
	// 5. 호스트가 SIM_EVENT_SATA_CMD_E를 받아서 HIL에게 전달한다. 호스트는 이제 SHS_WAIT_ACK 상태이다.
	// 6. HIL이 FTL에게 명령을 전달한다.
	// 7. FTL이 명령 처리를 끝내고 HIL에게 FB_XXX_DONE을 보낸다. FB_XXX_DONE은 FTL이 non-NCQ 명령을 끝냈을 때에 발생하는 feedback이다.
	// 8. HIL이 FB_XXX_DONE을 받고서 HOST에게 SIM_EVENT_SATA_ACK_S를 보낸다.
	// 9. HIL이 HIL에게 SIM_EVENT_SATA_ACK_E를 보낸다. 메시지의 인자(arg_32)는 FALSE로, non-NCQ 명령이라는 뜻이다.
	// 10. HIL이 SIM_EVENT_SATA_ACK_E를 받아서 호스트에게 전달한다.
	// 11. 호스트는 SIM_EVENT_SATA_ACK_E를 받아서 인자를 보고 non-NCQ 명령이 끝났음을 인지하고 remove_command()를 호출한다.

	g_host.last_ncq_tag = cmd->ncq_tag;

	ASSERT(g_host.num_pending_cmds < NCQ_SIZE);
	g_host.num_pending_cmds++;

	while (1)
	{
		// SHS_IDLE 상태에 진입한지 적어도 SIM_SATA_CMD_DELAY 만큼의 시간이 경과했을 때에 명령을 보낸다.
		// 시간이 경과하는 동안에 SHS_IDLE 상태에서 빠져 나갔다면 더욱 오래 기다려야 함

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

	while (1)	// collision 없이 명령을 무사히 보낼 때까지 반복 시도
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
			break; // 명령을 무사히 보냈음
		}

		ASSERT(g_host.collision == TRUE);

		g_host.last_cmd->arg_64 = NULL;		// 해당 메시지를 받았을 때에 무시하기 위한 표시

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
	// 한 세션에 둘 이상의 스크립트를 수행하는 것이 가능하다.
	// 한 스크립트에서 둘 이상의 세션을 수행하는 것은 불가능하다.
	// END_OF_SCRIPT를 만나면 현재 진행중인 명령들을 마무리하지 않고 (num_pending_cmds == 0 될 때까지 기다리지 않고) 리턴한다.
	// 그렇게 설계한 이유는, 한 세션에서 둘 이상의 스크립트를 run할 경우 스크립트 사이에 시간 지연 없이 매끈하게 연결함으로써
	// 마치 하나의 큰 스크립트를 수행했을 때와 같은 결과를 얻기 위함이다.
	// 모든 명령이 완료되기를 원한다면 END_OF_SCRIPT의 앞에 FINISH_ALL, FAST_FLUSH, SLOW_FLUSH 등을 넣으면 된다.

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
					finish_all();	// ambiguity를 회피하기 위해 기존의 명령들을 먼저 완료
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
					finish_all();	// ambiguity를 회피하기 위해 기존의 명령들을 먼저 완료
				}

				send_sata_command(new_cmd);

				break;
			}
			case TRIM:
			{
				// non-NCQ command 이므로 전후의 다른 명령과 시간적으로 겹치는 부분이 없어야 한다.

				finish_all();	// TRIM 명령 보내기 전에 기존의 NCQ 명령이 모두 완료되기를 기다림

				if (random_cmd == FALSE)
				{
					lba = *script_ptr++;
					sector_count = *script_ptr++;
				}

				sim_sata_cmd_t* new_cmd = get_free_cmd_slot();
				new_cmd->code = (UINT8) instruction;

				generate_trim_range(new_cmd, lba, sector_count);

				// TRIM 은 non-NCQ 명령이므로 write와는 달리 overlap 검사를 할 필요가 없음

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
				finish_all();	// TRIM과 마찬가지로 non-NCQ 명령

				sim_sata_cmd_t* new_cmd = get_free_cmd_slot();
				new_cmd->code = (UINT8) instruction;
				send_sata_command(new_cmd);
				finish_all();

				break;
			}
			case FINISH_ALL:	// 기존에 보낸 모든 명령이 완료될 때까지 기다림
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
					duration = random(g_host.nop_period_min, g_host.nop_period_max);	// 단위: 초
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
				// RANDOM_CMD를 위한 각 명령의 확률 설정

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

				purge_messages();	// 스크립트에서 PRINT_STAT 직전에 FLUSH 하므로, 실제로 버려지는 메시지는 없다.

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

	// 최초 session은 특별한 session 으로서, SSD internal format 을 수행한다. (공장 초기화)
	// session #0 이 시작되면 FTL이 internal format을 수행하고, 완료되면 SIM_EVENT_HELLO 를 호스트에게 보낸다.
	// 호스트는 SIM_EVENT_HELLO 를 받고서 아무런 SATA 명령도 보내지 않고 session #1으로 넘어간다.
	// session #1부터는 FTL internal booting이 완료되었을 때에 SIM_EVENT_HELLO가 발생하고, 호스트는 정상적으로 SATA 명령을 보낸다.

	g_sim_context.session = 0;
	host_begin_session();
	host_end_session();

	for (UINT32 session = 1; session <= 100; session++)
	{
		// 각 session은 power cycle을 의미한다.
		// session이 시작되면 ftl_open() 함수가 수행되고 FTL 내부 부팅을 진행한다. power loss recovery 동작도 이루어진다.
		// FTL 내부 부팅이 끝나면 호스트에게 SIM_EVENT_HELLO 를 보낸다. 호스트는 SIM_EVENT_HELLO 를 받기 전까지는 SATA 명령을 보내지 않고 기다린다.
		// session이 끝나기 직전 마지막 SATA 명령이 SLOW FLUSH (SATA에서 사용하는 정식 명칭은 STAND BY IMMEDIATE) 이면 graceful shutdown (safe shutdown) 이다.

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

