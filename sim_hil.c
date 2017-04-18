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


// sim_hil.c는 host interface firmware와  hardware를 하나로 통합한 추상적 모델이다.
//
//   1. 호스트 명령을 받아 FTL에게 전달한다.
//   2. write buffer를 할당받고 write data를 수신한다.
//   3. read data를 호스트에게 전송하고 read buffer를 반납한다.


#include "droll.h"

typedef enum
{
	// SATA Device State

	SDS_INIT,			// 초기화 진행중
	SDS_IDLE,			// SATA IDLE 상태
	SDS_CMD,			// 호스트로부터 명령 수신중
	SDS_BUSY,			// 명령을 받고 아직 ACK 보내지 않은 상태
	SDS_ACK,			// 호스트에게 ACK 보내고 있는 상태
	SDS_DMA_SETUP_W,	// 호스트에게 DMA SETUP FIS 보내고 있는 상태 (write)
	SDS_DMA_SETUP_R,	// 호스트에게 DMA SETUP FIS 보내고 있는 상태 (read)
	SDS_SDB,			// 호스트에게 Set Device Bits FIS 보내고 있는 상태

	SDS_WAIT_DATA,		// 호스트로부터 데이터가 오기를 기다리는 상태
	SDS_RX_DATA,		// 호스트로부터 데이터를 받고 있는 상태
	SDS_RX_PAUSE,		// 호스트에게 DMA ACTIVATE FIS를 보내야 하는 상태

	SDS_TX_DATA,		// 호스트에게 데이터를 보내고 있는 상태
	SDS_TX_PAUSE,		// 호스트에게 데이터를 보내야 하는 상태

} sds_t;

typedef struct
{
	sim_sata_cmd_t* cmd_table[NCQ_SIZE];	// 구조체 복사를 피하기 위해 g_host.cmd_table[] 엔트리에 대한 포인터만 가짐. HIL은 g_host.cmd_table[] 을 읽기만 한다.
	sim_sata_cmd_t* write_queue[NCQ_SIZE];	// queue of write commands
	UINT8	read_queue[NCQ_SIZE];			// queue of read commands (각 항목은 ncq_tag 값)
	UINT8	rq_rear_mem[NCQ_SIZE];			// rq_rear_mem[X] = Y는 ncq_tag 값이 X인 write command가 접수되던 당시에 read_q_rear의 값이 Y였다는 것을 의미함.
	UINT32	num_commands;
	UINT8	write_q_front;
	UINT8	write_q_rear;
	UINT8	read_q_front;
	UINT8	read_q_rear;

	UINT8	data_fis[DATA_FIS_SIZE_MAX];	// 호스트와 주고 받는 DATA FIS의 내용. 한 섹터당 한 바이트.

	BOOL8	read_ready[NCQ_SIZE];			// read 명령의 시작 부분 8KB(DATA_FIS_SIZE_MAX)에 대하여 NAND 읽기가 끝나면 데이터 전송 시작

	sds_t	sata_state;
	UINT32	sdb_bitmap;						// bit #N이 1이면, ncq_tag == #N인 SATA 명령이 완료되었는데 아직 호스트에게 SDB로 통보하지 못했다는 뜻

} sim_hil_t;

static sim_hil_t g_sim_hil;


static void hil_init(void)
{
	STOSQ(&g_sim_hil, 0, sizeof(g_sim_hil));
	g_sim_hil.sata_state = SDS_INIT;
}

enum { FIS_ACK, FIS_DMA_SETUP_R, FIS_DMA_SETUP_W, FIS_DMA_ACTV, FIS_SDB, FIS_READ_DATA, FIS_HELLO };

static void send_fis_to_host(UINT32 fis_type, UINT32 arg)
{
	sim_hil_t* hil = &g_sim_hil;
	sim_message_t* msg = sim_new_message();
	UINT64 current_time = g_sim_context.current_time;

	switch (fis_type)
	{
		case FIS_ACK:
		{
			ASSERT(hil->sata_state == SDS_BUSY);
			hil->sata_state = SDS_ACK;

			msg->code = SIM_EVENT_SATA_ACK_S;
			msg->when = current_time + SIM_SATA_ACK_DELAY;
			sim_send_message(msg, SIM_ENTITY_HOST);

			msg = sim_new_message();
			msg->code = SIM_EVENT_SATA_ACK_E;
			msg->arg_32 = arg;		// TRUE 이면 NCQ 명령을 잘 받았다는 뜻, FALSE 이면 non-NCQ 명령이 완료되었다는 뜻 - send_sata_command()의 설명 참고
			msg->when = current_time + SIM_SATA_ACK_DELAY + SIM_SATA_ACK_TIME;
			sim_send_message(msg, SIM_ENTITY_HIL);

			break;
		}
		case FIS_DMA_SETUP_R:
		case FIS_DMA_SETUP_W:
		{
			ASSERT(hil->sata_state == SDS_IDLE);
			g_sim_hil.sata_state = (fis_type == FIS_DMA_SETUP_R) ? SDS_DMA_SETUP_R : SDS_DMA_SETUP_W;

			msg->code = SIM_EVENT_DMA_SETUP_S;
			msg->when = current_time;
			PRINT_SATA_HIL("HIL %llu: SIM_EVENT_DMA_SETUP_S, %llu, %u\n", current_time, msg->when, msg->seq_number);
			sim_send_message(msg, SIM_ENTITY_HOST);

			msg = sim_new_message();
			msg->code = SIM_EVENT_DMA_SETUP_E;
			msg->arg_32 = arg;	// ncq_tag
			msg->arg_64 = (UINT64) (g_sim_hil.data_fis);	// 호스트가 write data를 여기에 넣을 것이다.
			msg->when = current_time + SIM_SATA_SETUP_TIME;
			PRINT_SATA_HIL("HIL %llu: SIM_EVENT_DMA_SETUP_E, %llu, %u\n", current_time, msg->when, msg->seq_number);
			sim_send_message(msg, SIM_ENTITY_HIL);

			break;
		}
		case FIS_DMA_ACTV:
		{
			ASSERT(hil->sata_state == SDS_RX_PAUSE);
			g_sim_hil.sata_state = SDS_WAIT_DATA;

			msg->code = SIM_EVENT_DMA_ACTV_S;
			msg->when = current_time + SIM_SATA_ACTV_DELAY;
			sim_send_message(msg, SIM_ENTITY_HOST);

			msg = sim_new_message();
			msg->code = SIM_EVENT_DMA_ACTV_E;
			msg->arg_64 = (UINT64) (g_sim_hil.data_fis);	// 호스트가 write data를 여기에 넣을 것이다.
			msg->when = current_time + SIM_SATA_ACTV_DELAY + SIM_SATA_ACTV_TIME;
			sim_send_message(msg, SIM_ENTITY_HOST);

			break;
		}
		case FIS_SDB:	// NCQ 명령 완료
		{
			UINT32 sdb_bitmap = arg;

			do
			{
				UINT32 ncq_tag;
				_BitScanForward((DWORD*) &ncq_tag, sdb_bitmap);
				sdb_bitmap &= ~BIT(ncq_tag);

				ASSERT(hil->cmd_table[ncq_tag] != NULL && hil->num_commands != 0);

				hil->cmd_table[ncq_tag] = NULL;
				hil->num_commands--;

			} while (sdb_bitmap != 0);

			ASSERT(hil->sata_state == SDS_IDLE);
			g_sim_hil.sata_state = SDS_SDB;

			msg->code = SIM_EVENT_SDB_S;
			msg->when = current_time;
			PRINT_SATA_HIL("HIL %llu: SIM_EVENT_SDB_S, %llu, %u\n", current_time, msg->when, msg->seq_number);
			sim_send_message(msg, SIM_ENTITY_HOST);

			msg = sim_new_message();
			msg->code = SIM_EVENT_SDB_E;
			msg->arg_32 = arg;
			msg->when = current_time + SIM_SATA_SDB_TIME;
			PRINT_SATA_HIL("HIL %llu: SIM_EVENT_SDB_E, %llu, %u\n", current_time, msg->when, msg->seq_number);
			sim_send_message(msg, SIM_ENTITY_HIL);

			break;
		}
		case FIS_READ_DATA:
		{
			ASSERT(hil->sata_state == SDS_TX_PAUSE);
			g_sim_hil.sata_state = SDS_TX_DATA;

			msg->code = SIM_EVENT_RDATA_S;
			msg->arg_32 = arg;
			msg->arg_64 = (UINT64) (g_sim_hil.data_fis);
			msg->when = current_time + SIM_SATA_RDATA_DELAY;
			sim_send_message(msg, SIM_ENTITY_HOST);

			msg = sim_new_message();
			msg->code = SIM_EVENT_RDATA_E;
			msg->when = current_time + SIM_SATA_RDATA_DELAY + SIM_SATA_NANOSEC_PER_SECTOR * arg;
			sim_send_message(msg, SIM_ENTITY_HIL);

			break;
		}
		case FIS_HELLO:
		{
			// 디바이스 초기화 과정이 모두 완료되고 명령을 받을 준비가 되었음을 호스트에게 통보

			ASSERT(g_sim_hil.sata_state == SDS_INIT);
			g_sim_hil.sata_state = SDS_IDLE;

			msg->code = SIM_EVENT_HELLO;
			msg->when = current_time;
			sim_send_message(msg, SIM_ENTITY_HOST);

			break;
		}
		default:
		{
			__assume(FALSE);
		}
	}
}

static void send_req_to_ftl(UINT8 req_code, UINT8 ncq_tag, UINT16 sector_count, UINT32 lba)
{
	UINT32 rear = g_ftl_cmd_q.rear;
	ASSERT(rear - V32(g_ftl_cmd_q.front) < FTL_CMD_Q_SIZE);				// g_ftl_cmd_q 가 넘치지 않는 것은 receive_write_data() 에 의하여 보장된다.
	ftl_cmd_t* ftl_cmd = g_ftl_cmd_q.queue + (rear % FTL_CMD_Q_SIZE);

	ftl_cmd->req_code = req_code;
	ftl_cmd->ncq_tag = ncq_tag;
	ftl_cmd->sector_count = sector_count;	// 65536은 0으로 표현한다. 이는 SATA 규약에 따른 것이다.
	ftl_cmd->lba = lba;

	V32(g_ftl_cmd_q.rear) = rear + 1;

	sim_wake_up(SIM_ENTITY_FTL);
}

static void receive_host_cmd(sim_sata_cmd_t* new_cmd)
{
	sim_hil_t* hil = &g_sim_hil;

	UINT8 ncq_tag = new_cmd->ncq_tag;				// 호스트가 선택한 값; 진행중인 SATA 명령중에서 하나를 unique하게 identify하기 위한 값
	ASSERT(ncq_tag < NCQ_SIZE);

	// 이 함수의 인자로 주어진 new_cmd는 g_host.cmd_table[]의 슬롯을 가리키는 포인터이다. 즉, host thread가 관리하는 구조체에 대한 포인터이다.
	// memory copy를 피하고 코드를 간소화하기 위해, 별도의 구조체 테이블을 g_sim_hil에 만들지 않고 그냥 주어진 포인터만 저장하는 테이블을 정의하기로 한다.
	// HIL이 HOST에게 Set Device Bits FIS를 보내기 전까지는 해당 슬롯의 내용이 지워지지 않으므로, 위험하지 않다.

	ASSERT(hil->cmd_table[ncq_tag] == NULL);		// 빈 슬롯이어야 한다. 즉, 놀고 있는 NCQ tag 값을 호스트가 골랐어야 한다.
	hil->cmd_table[ncq_tag] = new_cmd;				// 포인터만 저장
	hil->num_commands++;

	if (new_cmd->code == REQ_HOST_WRITE)
	{
		ASSERT(new_cmd->num_sectors_requested != 0 && new_cmd->num_sectors_requested <= SECTOR_COUNT_MAX);
		ASSERT(new_cmd->lba + new_cmd->num_sectors_requested <= NUM_LSECTORS);

		hil->write_queue[hil->write_q_rear++ % NCQ_SIZE] = new_cmd;		// write 명령들의 접수 순서를 기억하기 위한 queue
		hil->rq_rear_mem[ncq_tag] = hil->read_q_rear;	// 이 write 명령보다 먼저 접수되고 현재 시점을 기준으로 아직 완료되지 않은 read 명령들을 구분하기 위한 정보

		// NCQ 명령의 경우, 명령을 받는 즉시 ACK을 보낸다.
		// NCQ 명령(READ와 WRITE)은 데이터를 완전히 받고 나서 Set Device Bits FIS를 보냄으로써 명령 완료를 통보한다.
		// non-NCQ 명령은 Set Device Bits FIS는 사용하지 않고, 명령 처리가 완료된 뒤에 ACK을 보낸다.

		send_fis_to_host(FIS_ACK, TRUE);
	}
	else if (new_cmd->code == REQ_HOST_READ)
	{
		ASSERT(new_cmd->num_sectors_requested != 0 && new_cmd->num_sectors_requested <= SECTOR_COUNT_MAX);
		ASSERT(new_cmd->lba + new_cmd->num_sectors_requested <= NUM_LSECTORS);
		ASSERT(hil->read_q_rear - hil->read_q_front < NCQ_SIZE);

		hil->read_ready[ncq_tag] = FALSE;
		hil->read_queue[hil->read_q_rear++ % NCQ_SIZE] = ncq_tag;

		send_fis_to_host(FIS_ACK, TRUE);
	}
	else
	{
		// non-NCQ command (TRIM, SLOW_FLUSH, FAST_FLUSH)

		hil->cmd_table[ncq_tag] = NULL;
		ncq_tag = FF8;
	}

	// 아래 함수는 명령을 FTL command queue에 추가한다.

	send_req_to_ftl(new_cmd->code, ncq_tag, (UINT16) new_cmd->num_sectors_requested, new_cmd->lba);
}

static void receive_feedback_from_ftl(feedback_t* fb)
{
	sim_hil_t* hil = &g_sim_hil;

	switch (fb->code)
	{
		case FB_FTL_READY:
		{
			send_fis_to_host(FIS_HELLO, NULL);
			break;
		}
		case FB_TRIM_DONE:
		case FB_FLUSH_DONE:
		{
			ASSERT(hil->num_commands == 1);
			hil->num_commands--;
			send_fis_to_host(FIS_ACK, FALSE);
			break;
		}
		default:
			ASSERT(FAIL);
	}
}

static UINT32 wait_message(void)
{
	sim_hil_t* hil = &g_sim_hil;
	sim_message_t* msg = sim_receive_message(SIM_ENTITY_HIL);
	UINT16 msg_code = (msg == NULL) ? SIM_EVENT_WAKE_UP : msg->code;

	switch (msg_code)
	{
		case SIM_EVENT_WDATA_S:
		{
			ASSERT(hil->sata_state == SDS_WAIT_DATA);
			hil->sata_state = SDS_RX_DATA;
			break;
		}
		case SIM_EVENT_WDATA_E:
		{
			ASSERT(hil->sata_state == SDS_RX_DATA);
			// 다음 상태는 receive_write_data() 함수에서 결정
			break;
		}
		case SIM_EVENT_RDATA_E:
		{
			ASSERT(hil->sata_state == SDS_TX_DATA);
			sim_send_message(msg, SIM_ENTITY_HOST);
			msg = NULL;
			// 다음 상태는 send_read_data() 함수에서 결정
			break;
		}
		case SIM_EVENT_DMA_SETUP_E:
		{
			if (hil->sata_state == SDS_DMA_SETUP_R)
			{
				hil->sata_state = SDS_TX_PAUSE;
			}
			else
			{
				ASSERT(hil->sata_state == SDS_DMA_SETUP_W);
				hil->sata_state = SDS_WAIT_DATA;
			}

			sim_send_message(msg, SIM_ENTITY_HOST);
			msg = NULL;

			break;
		}
		case SIM_EVENT_SDB_E:
		{
			ASSERT(hil->sata_state == SDS_SDB);
			hil->sata_state = SDS_IDLE;

			sim_send_message(msg, SIM_ENTITY_HOST);
			msg = NULL;

			break;
		}
		case SIM_EVENT_SATA_CMD_S:
		{
			if (hil->sata_state == SDS_IDLE)
			{
				PRINT_SATA_HIL("HIL %llu: received SIM_EVENT_SATA_CMD_S good, %llu, %u\n", g_sim_context.current_time, msg->when, msg->seq_number);
				hil->sata_state = SDS_CMD;
			}
			else
			{
				// SATA bus collision: sim_host.c의 설명 참고
				PRINT_SATA_HIL("HIL %llu: received SIM_EVENT_SATA_CMD_S collision, %llu, %u\n", g_sim_context.current_time, msg->when, msg->seq_number);
			}

			break;
		}
		case SIM_EVENT_SATA_CMD_E:
		{
			if (hil->sata_state == SDS_CMD)
			{
				PRINT_SATA_HIL("HIL %llu: received SIM_EVENT_SATA_CMD_E good, %llu, %u\n", g_sim_context.current_time, msg->when, msg->seq_number);

				hil->sata_state = SDS_BUSY;

				sim_sata_cmd_t* new_cmd = (sim_sata_cmd_t*) msg->arg_64;
				receive_host_cmd(new_cmd);
			}
			else
			{
				// SATA bus collision: sim_host.c의 설명 참고
				PRINT_SATA_HIL("HIL %llu: received SIM_EVENT_SATA_CMD_E collision, %llu, %u\n", g_sim_context.current_time, msg->when, msg->seq_number);
			}

			break;
		}
		case SIM_EVENT_SATA_ACK_E:
		{
			ASSERT(hil->sata_state == SDS_ACK);
			hil->sata_state = SDS_IDLE;

			sim_send_message(msg, SIM_ENTITY_HOST);
			msg = NULL;

			break;
		}
		case SIM_EVENT_FEEDBACK:
		{
			feedback_queue_t* fbq = &g_feedback_q;

			ASSERT(fbq->front != V32(fbq->rear));
			feedback_t* fb = fbq->queue + fbq->front%FEEDBACK_Q_SIZE;
			receive_feedback_from_ftl(fb);
			V32(fbq->front) = fbq->front + 1;

			break;
		}
		case SIM_EVENT_WAKE_UP:
		case SIM_EVENT_DELAY:
		{
			break;	// nothing to do
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

	return msg_code;
}

static void internal_delay(UINT64 length)
{
	sim_hil_t* hil = &g_sim_hil;

	sim_message_t* msg = sim_new_message();
	msg->code = SIM_EVENT_DELAY;
	msg->when = g_sim_context.current_time + length;
	sim_send_message(msg, SIM_ENTITY_HIL);

	BOOL32 delay_done = FALSE;

	while (1)
	{
		UINT32 msg_code = wait_message();

		if (msg_code == SIM_EVENT_DELAY)
		{
			delay_done = TRUE;
		}

		if (delay_done == TRUE && hil->sata_state == SDS_IDLE)
		{
			break;
		}
	}
}

static __forceinline void receive_write_data(void)
{
	// 디바이스가 write data를 받을 준비가 되면 호스트에게 DMA SETUP FIS를 보낸다.
	// DMA SETUP FIS의 인자는 NCQ tag로서, 현재 데이터 전송이 완료되지 않은 read/write command 중에서 어떤 것인지 구분하는 용도이다.
	// 디바이스는 여러 write command 중에서 원하는 것을 선택할 자유가 있지만, 이 프로젝트에서는 무조건 command 접수 순서 대로 선택하기로 한다.
	// 호스트는 DMA SETUP FIS에 대한 응답으로 즉시 DATA FIS를 보낸다. DATA FIS의 크기는 최대 8KB 이다.
	//
	// [중요] DATA FIS 전송은 항상 8KB 단위로 이루어지다가 마지막 자투리에 대해서만 8KB 이하이다.
	//
	// 예를 들어 선택된 write command의 total size가 17KB (34 sectors)라고 가정하자.
	// 디바이스는 적어도 8KB 만큼의 버퍼가 확보되었을 때에 DMA SETUP FIS를 보내고 첫번째 DATA FIS를 받는다.
	// 호스트는 두번째 DATA FIS를 보내지 않고 일단 기다린다.
	// 디바이스에 추가로 8KB 버퍼가 확보되면 DMA ACTIVATE FIS를 호스트에게 보내고 두번째 DATA FIS를 받는다.
	// 마지막으로 1KB 버퍼가 확보되면 세번째 DMA ACTIVATE FIS를 호스트에게 보내고 마지막 1KB를 받는다.
	// 더 자세히 설명하면,
	// (1) 호스트가 SATA IDLE 상태에서 write command를 보낸다. 디바이스가 접수하고 간단한 응답 메시지를 보내면 다시 SATA IDLE 상태가 된다.
	// (2) 디바이스는 SATA IDLE 상태에서 write command 중에서 하나를 선택하여 DMA SETUP FIS를 보내고 DATA FIS를 받는다.
	// (3) 선택된 write command의 total size가 8KB를 넘는 경우에는 추가적으로 DMA ACTIVATE FIS를 보내고 DATA FIS를 받는다.
	//     첫번째 DATA FIS는 DMA SETUP FIS에 대한 응답이고, 두번째부터 DATA FIS는 DMA ACTIVATE FIS에 대한 응답이다.
	//     DMA SETUP FIS의 인자는 NCQ tag이고, DMA ACTIVATE FIS에는 인자가 없다.
	// (4) DMA ACTIVATE FIS 보내고 DATA FIS 받는 과정을 반복하다가, 마지막 DATA FIS를 받고 나면 다시 SATA IDLE 상태로 되돌아간다.
	// (5) 선택된 write command는 이제 사실상 SATA 차원의 처리가 끝나고 NAND 차원의 뒤처리만 남았지만, SATA 규약에 의하면 호스트에게 Set Device Bits FIS를
	//     보내야 비로소 해당 write command가 완전히 끝난 것으로 간주되어 NCQ에서 지워진다. Set Device Bits FIS의 인자는 NCQ tag이다.
	//     WRITE_FUA 명령의 경우에는 데이터가 NAND에 안전하게 쓰여진 다음에 Set Device Bits FIS를 보내야 한다.
	//
	// 만일 디바이스가 필요한 버퍼를 확보하지 않은 채로 DMA SETUP FIS 또는 DMA ACTIVATE FIS를 보내면, DATA FIS 수신 도중에
	// HOLD 신호를 호스트에게 보내서 잠시 멈추는 것이 가능하다. 예를 들어 버퍼를 4KB만 확보하고 8KB짜리 DATA FIS의 수신을 시작하면
	// 중간에 buffer overflow error가 발생하는 것이 아니고 HOLD 신호에 의해서 잠시 멈추게 된다. SATA link layer를 담당하는 하드웨어가 이러한 기능을 가진다.
	// 추가 버퍼를 신속하게 확보하지 못해서 HOLD 신호가 일정 시간 지속되면 호스트 측의 link layer에서 time-out error가 발생한다.
	// 반면, DMA ACTIVATE FIS를 보내지 않고 뜸들이면 호스트가 비교적 오랫동안 기다린 뒤에 time-out error가 발생한다.
	// 결국 HOLD 신호를 이용한 flow control은 link layer 차원에서 수십 바이트 규모로 사용하는 기능이므로, command layer는 여기에 의존하지 않는 것이 바람직하다.
	// 즉, 매번 수신하게 될 DATA FIS의 크기를 예측해서 버퍼를 미리 확보할 책임이 command layer에게 있다고 봐야 한다.
	//
	// 위에 설명된 내용은 SATA 규약에 정의된 non-zero buffer offset 기능을 지원하지 않을 경우를 기준으로 한 것이다.
	// non-zero buffer offset 기능을 사용하면 많은 제약이 없어지고 성능 향상의 여지가 있지만 일반 소비자용 PC의 HBA는 non-zero buffer offset 기능을
	// 지원하지 않기 때문에 이 프로젝트에서도 지원하지 않기로 한다.
	//
	// [주의] 아래의 bm_alloc_write_buf() 가 실패하면 즉시 return하므로, 그 전에 전역 변수를 변경하는 코드가 없도록 작성할 것.
	// [주의] 이 함수는 DATA_FIS_SIZE_MAX >= SECTORS_PER_SLICE를 가정하고 작성되었다.

	sim_hil_t* hil = &g_sim_hil;
	sim_sata_cmd_t* cmd = hil->write_queue[hil->write_q_front % NCQ_SIZE];	// 호스트 명령을 선택 (receive_host_cmd()에서 접수한 순서대로)
	UINT32 remaining_sectors = cmd->num_sectors_requested;					// 선택된 호스트 명령의 total size (섹터수)
	UINT32 data_fis_size = MIN(remaining_sectors, DATA_FIS_SIZE_MAX);		// 처음 수신하게 될 DATA FIS의 크기 (섹터수)
	UINT32 ncq_tag = cmd->ncq_tag;
	UINT32 lba = cmd->lba;

	#if OPTION_DRAM_SSD
	UINT32 sector_offset = 0;
	#else
	UINT32 sector_offset = cmd->lba % SECTORS_PER_SLICE;					// 할당 받아서 데이터를 넣지 않고 그냥 건너 뛰는 버퍼 슬롯의 개수
	#endif

	if (g_ftl_cmd_q.rear - V32(g_ftl_cmd_q.front) >= FTL_CMD_Q_SIZE - NCQ_SIZE)
	{
		return;
	}
	else if ((UINT32) (hil->read_q_front - hil->rq_rear_mem[ncq_tag]) <= NCQ_SIZE)
	{
		// 지금 처리하려고 하는 write 명령은 먼저 접수된 read 명령을 새치기 하지 않는다.

		if (bm_alloc_write_buf(sector_offset + data_fis_size) == FAIL)
		{
			// 버퍼가 부족하면 DMA SETUP FIS를 보내지 말고 나중에 다시 시도
			// DMA SETUP FIS를 보내지 않고 SATA IDLE 상태에 남아 있으면 새로운 호스트 명령을 받거나 read data를 보낼 기회가 있다.
			return;
		}
	}
	else
	{
		// 지금 처리하려고 하는 write 명령보다 더 과거에 접수된 read 명령이 아직 안 끝났다.
		// 새치기를 하려면 필요한 자원 전체가 완전하게 확보되었을 때에만 한다. (교착 상태 방지)

		if (bm_query_write_buf() < sector_offset + remaining_sectors)
		{
			return;
		}

		bm_alloc_write_buf(sector_offset + data_fis_size);
	}

	if (sector_offset != 0)
	{
		// 호스트 명령의 시작 위치가 slice 경계선에 align 되어 있지 않은 경우
		// 건너뛴 버퍼에는 read_merge_fore() 함수에 의하여 NAND에서 읽은 데이터가 들어갈 것이다.

		bm_skip_write_buf(sector_offset);
	}

	hil->write_q_front++;

	internal_delay(SIM_SATA_SETUP_W_DELAY);		// 펌웨어로 인한 시간 지연 (FIS_DMA_SETUP_W 전송이 시작되기까지의 지연)

	send_fis_to_host(FIS_DMA_SETUP_W, ncq_tag);

	while (1)
	{
		UINT32 msg_code = wait_message();

		if (msg_code == SIM_EVENT_WDATA_E)	// DATA FIS가 도착했음
		{
			bm_import_write_data(hil->data_fis, data_fis_size, lba);

			lba += data_fis_size;

			sim_wake_up(SIM_ENTITY_FTL);

			remaining_sectors -= data_fis_size;

			if (remaining_sectors == 0)
			{
				hil->sata_state = SDS_IDLE;
				break;	// write command 완료 - while loop 탈출
			}
			else
			{
				hil->sata_state = SDS_RX_PAUSE;
			}
		}

		if (hil->sata_state == SDS_RX_PAUSE)
		{
			data_fis_size = MIN(remaining_sectors, DATA_FIS_SIZE_MAX);	// 수신하게 될 DATA FIS의 크기 (섹터수)

			if (bm_alloc_write_buf(data_fis_size) == OK)
			{
				send_fis_to_host(FIS_DMA_ACTV, NULL);
			}
		}
	}

	#if OPTION_DRAM_SSD == FALSE
	{
		if ((cmd->lba + cmd->num_sectors_requested) % SECTORS_PER_SLICE != 0)
		{
			// 호스트 명령의 끝 위치가 slice 경계선에 align 되어 있지 않은 경우
			// 건너뛴 버퍼에는 read_merge_aft() 함수에 의하여 NAND에서 읽은 데이터가 들어갈 것이다.

			UINT32 num_missing_sectors = SECTORS_PER_SLICE - (cmd->lba + cmd->num_sectors_requested) % SECTORS_PER_SLICE;

			bm_alloc_write_buf(num_missing_sectors);

			bm_skip_write_buf(num_missing_sectors);
		}
	}
	#endif

	hil->sdb_bitmap |= BIT(ncq_tag);
}

static __forceinline void send_read_data(void)
{
	// receive_write_data()의 설명 참고.
	// memory.h의 "read buffer에 관한 설명" 참고.
	// read data를 호스트에게 보낼 때에도 write data를 받을 때와 마찬가지로 DATA FIS 전체가 준비되었을 때에 전송을 시작하기로 한다.
	// DATA FIS를 보내는 도중에 보낼 데이터가 소진되어서 HOLD 신호를 사용하는 것은 바람직하지 않기 때문이다.
	// DATA FIS와 DATA FIS의 사이에 시간 지연이 생기는 것은 허용하기로 한다.
	// write와는 달리, read에서는 DMA ACTIVATE FIS가 존재하지 않는다. 즉, DMA SETUP FIS를 보낸 다음 DATA FIS 전송만 반복한다.

	sim_hil_t* hil = &g_sim_hil;
	UINT32 ncq_tag = hil->read_queue[hil->read_q_front % NCQ_SIZE];	// 처리할 read 명령을 선택

	if (hil->read_ready[ncq_tag] == FALSE)
	{
		// 아직 첫번째 DATA FIS를 보낼 만큼의 데이터가 확보되지 않았다.
		// 최초의 DATA FIS를 보낼 준비가 안되었으면 (보낼 데이터가 아직 NAND에서 나오지 않았으면) 성급하게 DMA SETUP FIS를 보내지 말고 기다리는 편이 좋다.
		// SATA IDLE 상태를 유지함으로서 다른 호스트 명령을 받거나 다른 명령의 데이터 송수신을 할 기회가 먼저 생길 수 있기 때문이다.
		// HIL 펌웨어가 복잡한 조건을 반복적으로 검사하는 부담을 덜기 위해, read_ready를 TRUE로 바꾸는 역할은 FIL이 담당하기로 한다.

		return;
	}

	hil->read_q_front++;

	sim_sata_cmd_t* cmd = hil->cmd_table[ncq_tag];
	UINT32 remaining_sectors = cmd->num_sectors_requested;			// 선택된 호스트 명령의 total size (섹터수)
	UINT32 offset = cmd->lba % SECTORS_PER_SLICE;					// 시작 위치 (slice 내에서의 섹터 번호)
	UINT32 data_fis_size = 0;
	UINT32 lba = cmd->lba;

	internal_delay(SIM_SATA_SETUP_R_DELAY);			// 펌웨어로 인한 시간 지연 (FIS_DMA_SETUP_R 전송이 시작되기까지의 지연)

	send_fis_to_host(FIS_DMA_SETUP_R, ncq_tag);

	if (offset != 0)
	{
		bm_discard_read_data(offset);				// 호스트에게 보내지 않고 버리는 데이터
	}

	while (1)
	{
		UINT32 msg_code = wait_message();

		if (msg_code == SIM_EVENT_RDATA_E)
		{
			bm_release_read_buf(data_fis_size);		// 전송이 완료된 read buffer slot 반납

			remaining_sectors -= data_fis_size;

			if (remaining_sectors == 0)
			{
				UINT32 end_offset = (offset + data_fis_size) % SECTORS_PER_SLICE;

				if (end_offset != 0)
				{
					bm_discard_read_data(SECTORS_PER_SLICE - end_offset);	// 끝나는 위치가 slice 경계선에 align되지 않은 경우, 호스트에게 보내고 남은 데이터는 버린다.
				}

				hil->sata_state = SDS_IDLE;

				break;	// read command 완료 - while loop 탈출
			}

			hil->sata_state = SDS_TX_PAUSE;
		}
		else if (msg_code != SIM_EVENT_WAKE_UP && msg_code != SIM_EVENT_DMA_SETUP_E)
		{
			continue;
		}

		if (hil->sata_state == SDS_TX_PAUSE)
		{
			data_fis_size = MIN(remaining_sectors, DATA_FIS_SIZE_MAX);	// 이번에 전송할 DATA FIS의 크기 (섹터수)

			if (bm_export_read_data(hil->data_fis, data_fis_size, lba) == OK)
			{
				// 리턴값이 FAIL 이면 SIM_EVENT_WAKE_UP 받고 다시 시도
				send_fis_to_host(FIS_READ_DATA, data_fis_size);
				lba += data_fis_size;
			}
			else
			{
				ASSERT(lba != cmd->lba);	// 여기에 걸리면 read_ready 로직에 bug가 있는 것임
			}
		}
	}

	hil->sdb_bitmap |= BIT(ncq_tag);
}

void hil_notify_read_ready(UINT32 ncq_tag)
{
	// 이 함수는 HIL의 context 정보를 수정하지만, FIL에 의해 호출된다.

	ASSERT(g_sim_hil.read_ready[ncq_tag] == FALSE);
	g_sim_hil.read_ready[ncq_tag] = TRUE;
}

static void sim_hil_main(void)
{
	if (setjmp(g_jump_buf[SIM_ENTITY_HIL]) != 0)		// SIM_EVENT_POWER_OFF
	{
		return;
	}

	sim_hil_t* hil = &g_sim_hil;

	while (1)
	{
		wait_message();

		UINT32 count = 2;

		do
		{
			// receive_write_data()와 send_read_data()가 모두 가능할 때에는 선택의 여지가 있다.
			// 단, 교착 상태를 피해야 한다.
			// 시나리오:
			// 1. 호스트가 read 명령을 보낸다.
			// 3. FTL이 read 명령 처리를 시작한다.
			// 2. 호스트가 write 명령을 보낸다.
			// 3. HIL이 write data 수신을 시작한다.
			// 4. FTL이 read buffer가 부족하여 read 명령 처리를 끝내지 못한다. (read buffer를 할당 받는 주체가 FTL이라고 가정) write 명령 처리를 시작하지도 못한다.
			// 5. HIL이 write buffer가 부족하여 write data 수신을 끝내지 못한다. (write buffer를 할당 받는 주체가 HIL이라고 가정) read data 전송을 시작하지도 못한다. (non-zero buffer offset을 사용하지 않으므로)
			// 6. read buffer 부족을 해소하려면 HIL이 read data 전송을 시작해야 하고, write buffer 부족을 해소하려면 FTL이 write 명령 처리를 시작해야 한다.
			// 호스트 명령은 read 다음에 write가 왔는데 성능 향상을 위해 HIL이 데이터 송수신을 out-of-order로 시작했기 때문에 문제가 발생한다.
			// 이러한 교착 상태는 read, write 명령의 sector count가 아주 클 때에 쉽게 재현되지만,
			// 명령 수신 당시에 read, write buffer가 거의 차 있었다면 sector count가 작아도 재현될 수 있다.
			// 반대로, HIL은 read data 전송 도중에 멈추고 FTL은 write data 수신을 기다리는 시나리오도 존재한다. (FTL이 read를 중간에 suspend 하고 write로 넘어가는 경우)
			// 해결책:
			// 작업 도중에 리소스 부족으로 인하여 교착 상태가 발생하는 것을 예방하기 위해, 리스소 확보가 보장될 때에만 out-of-order 처리를 시작한다.
			// 즉, HIL이 out-of-order 처리를 시도할 때에는 (먼저 접수한 read를 처리하지 않고 나중에 접수한 write를 먼저 처리) write buffer의 남은 공간을 검사한다.

			if (hil->sata_state == SDS_IDLE && hil->read_q_front != hil->read_q_rear)
			{
				send_read_data();
			}

			if (hil->sata_state == SDS_IDLE && hil->write_q_front != hil->write_q_rear)
			{
				receive_write_data();
			}

			if (hil->sata_state == SDS_IDLE && hil->sdb_bitmap != 0)
			{
				internal_delay(SIM_SATA_SDB_DELAY);
				send_fis_to_host(FIS_SDB, hil->sdb_bitmap);
				hil->sdb_bitmap = 0;
			}

		} while (--count != 0);
	}
}

void sim_hil_thread(void* arg_list)
{
	UNREFERENCED_PARAMETER(arg_list);

	g_random = new mt19937(g_sim_context.random_seed);

	while (1)
	{
		sim_message_t* msg = sim_receive_message(SIM_ENTITY_HIL);
		UINT32 msg_code = msg->code;
		sim_release_message_slot(msg);

		if (msg_code == SIM_EVENT_POWER_ON)
		{
			// sim_hil_main() 내부에서 FTL로부터 FB_FTL_READY 받으면 HOST 에게 SIM_EVENT_HELLO 보낼 것이다.

			hil_init();
			sim_hil_main();		// SIM_EVENT_POWER_OFF 받으면 sim_hil_main() 으로부터 리턴
		}
		else if (msg_code == SIM_EVENT_END_SIMULATION)
		{
			break;
		}
	}
}
