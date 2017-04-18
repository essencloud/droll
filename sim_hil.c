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


// sim_hil.c�� host interface firmware��  hardware�� �ϳ��� ������ �߻��� ���̴�.
//
//   1. ȣ��Ʈ ����� �޾� FTL���� �����Ѵ�.
//   2. write buffer�� �Ҵ�ް� write data�� �����Ѵ�.
//   3. read data�� ȣ��Ʈ���� �����ϰ� read buffer�� �ݳ��Ѵ�.


#include "droll.h"

typedef enum
{
	// SATA Device State

	SDS_INIT,			// �ʱ�ȭ ������
	SDS_IDLE,			// SATA IDLE ����
	SDS_CMD,			// ȣ��Ʈ�κ��� ��� ������
	SDS_BUSY,			// ����� �ް� ���� ACK ������ ���� ����
	SDS_ACK,			// ȣ��Ʈ���� ACK ������ �ִ� ����
	SDS_DMA_SETUP_W,	// ȣ��Ʈ���� DMA SETUP FIS ������ �ִ� ���� (write)
	SDS_DMA_SETUP_R,	// ȣ��Ʈ���� DMA SETUP FIS ������ �ִ� ���� (read)
	SDS_SDB,			// ȣ��Ʈ���� Set Device Bits FIS ������ �ִ� ����

	SDS_WAIT_DATA,		// ȣ��Ʈ�κ��� �����Ͱ� ���⸦ ��ٸ��� ����
	SDS_RX_DATA,		// ȣ��Ʈ�κ��� �����͸� �ް� �ִ� ����
	SDS_RX_PAUSE,		// ȣ��Ʈ���� DMA ACTIVATE FIS�� ������ �ϴ� ����

	SDS_TX_DATA,		// ȣ��Ʈ���� �����͸� ������ �ִ� ����
	SDS_TX_PAUSE,		// ȣ��Ʈ���� �����͸� ������ �ϴ� ����

} sds_t;

typedef struct
{
	sim_sata_cmd_t* cmd_table[NCQ_SIZE];	// ����ü ���縦 ���ϱ� ���� g_host.cmd_table[] ��Ʈ���� ���� �����͸� ����. HIL�� g_host.cmd_table[] �� �б⸸ �Ѵ�.
	sim_sata_cmd_t* write_queue[NCQ_SIZE];	// queue of write commands
	UINT8	read_queue[NCQ_SIZE];			// queue of read commands (�� �׸��� ncq_tag ��)
	UINT8	rq_rear_mem[NCQ_SIZE];			// rq_rear_mem[X] = Y�� ncq_tag ���� X�� write command�� �����Ǵ� ��ÿ� read_q_rear�� ���� Y���ٴ� ���� �ǹ���.
	UINT32	num_commands;
	UINT8	write_q_front;
	UINT8	write_q_rear;
	UINT8	read_q_front;
	UINT8	read_q_rear;

	UINT8	data_fis[DATA_FIS_SIZE_MAX];	// ȣ��Ʈ�� �ְ� �޴� DATA FIS�� ����. �� ���ʹ� �� ����Ʈ.

	BOOL8	read_ready[NCQ_SIZE];			// read ����� ���� �κ� 8KB(DATA_FIS_SIZE_MAX)�� ���Ͽ� NAND �бⰡ ������ ������ ���� ����

	sds_t	sata_state;
	UINT32	sdb_bitmap;						// bit #N�� 1�̸�, ncq_tag == #N�� SATA ����� �Ϸ�Ǿ��µ� ���� ȣ��Ʈ���� SDB�� �뺸���� ���ߴٴ� ��

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
			msg->arg_32 = arg;		// TRUE �̸� NCQ ����� �� �޾Ҵٴ� ��, FALSE �̸� non-NCQ ����� �Ϸ�Ǿ��ٴ� �� - send_sata_command()�� ���� ����
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
			msg->arg_64 = (UINT64) (g_sim_hil.data_fis);	// ȣ��Ʈ�� write data�� ���⿡ ���� ���̴�.
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
			msg->arg_64 = (UINT64) (g_sim_hil.data_fis);	// ȣ��Ʈ�� write data�� ���⿡ ���� ���̴�.
			msg->when = current_time + SIM_SATA_ACTV_DELAY + SIM_SATA_ACTV_TIME;
			sim_send_message(msg, SIM_ENTITY_HOST);

			break;
		}
		case FIS_SDB:	// NCQ ��� �Ϸ�
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
			// ����̽� �ʱ�ȭ ������ ��� �Ϸ�ǰ� ����� ���� �غ� �Ǿ����� ȣ��Ʈ���� �뺸

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
	ASSERT(rear - V32(g_ftl_cmd_q.front) < FTL_CMD_Q_SIZE);				// g_ftl_cmd_q �� ��ġ�� �ʴ� ���� receive_write_data() �� ���Ͽ� ����ȴ�.
	ftl_cmd_t* ftl_cmd = g_ftl_cmd_q.queue + (rear % FTL_CMD_Q_SIZE);

	ftl_cmd->req_code = req_code;
	ftl_cmd->ncq_tag = ncq_tag;
	ftl_cmd->sector_count = sector_count;	// 65536�� 0���� ǥ���Ѵ�. �̴� SATA �Ծ࿡ ���� ���̴�.
	ftl_cmd->lba = lba;

	V32(g_ftl_cmd_q.rear) = rear + 1;

	sim_wake_up(SIM_ENTITY_FTL);
}

static void receive_host_cmd(sim_sata_cmd_t* new_cmd)
{
	sim_hil_t* hil = &g_sim_hil;

	UINT8 ncq_tag = new_cmd->ncq_tag;				// ȣ��Ʈ�� ������ ��; �������� SATA ����߿��� �ϳ��� unique�ϰ� identify�ϱ� ���� ��
	ASSERT(ncq_tag < NCQ_SIZE);

	// �� �Լ��� ���ڷ� �־��� new_cmd�� g_host.cmd_table[]�� ������ ����Ű�� �������̴�. ��, host thread�� �����ϴ� ����ü�� ���� �������̴�.
	// memory copy�� ���ϰ� �ڵ带 ����ȭ�ϱ� ����, ������ ����ü ���̺��� g_sim_hil�� ������ �ʰ� �׳� �־��� �����͸� �����ϴ� ���̺��� �����ϱ�� �Ѵ�.
	// HIL�� HOST���� Set Device Bits FIS�� ������ �������� �ش� ������ ������ �������� �����Ƿ�, �������� �ʴ�.

	ASSERT(hil->cmd_table[ncq_tag] == NULL);		// �� �����̾�� �Ѵ�. ��, ��� �ִ� NCQ tag ���� ȣ��Ʈ�� ������ �Ѵ�.
	hil->cmd_table[ncq_tag] = new_cmd;				// �����͸� ����
	hil->num_commands++;

	if (new_cmd->code == REQ_HOST_WRITE)
	{
		ASSERT(new_cmd->num_sectors_requested != 0 && new_cmd->num_sectors_requested <= SECTOR_COUNT_MAX);
		ASSERT(new_cmd->lba + new_cmd->num_sectors_requested <= NUM_LSECTORS);

		hil->write_queue[hil->write_q_rear++ % NCQ_SIZE] = new_cmd;		// write ��ɵ��� ���� ������ ����ϱ� ���� queue
		hil->rq_rear_mem[ncq_tag] = hil->read_q_rear;	// �� write ��ɺ��� ���� �����ǰ� ���� ������ �������� ���� �Ϸ���� ���� read ��ɵ��� �����ϱ� ���� ����

		// NCQ ����� ���, ����� �޴� ��� ACK�� ������.
		// NCQ ���(READ�� WRITE)�� �����͸� ������ �ް� ���� Set Device Bits FIS�� �������ν� ��� �ϷḦ �뺸�Ѵ�.
		// non-NCQ ����� Set Device Bits FIS�� ������� �ʰ�, ��� ó���� �Ϸ�� �ڿ� ACK�� ������.

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

	// �Ʒ� �Լ��� ����� FTL command queue�� �߰��Ѵ�.

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
			// ���� ���´� receive_write_data() �Լ����� ����
			break;
		}
		case SIM_EVENT_RDATA_E:
		{
			ASSERT(hil->sata_state == SDS_TX_DATA);
			sim_send_message(msg, SIM_ENTITY_HOST);
			msg = NULL;
			// ���� ���´� send_read_data() �Լ����� ����
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
				// SATA bus collision: sim_host.c�� ���� ����
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
				// SATA bus collision: sim_host.c�� ���� ����
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
	// ����̽��� write data�� ���� �غ� �Ǹ� ȣ��Ʈ���� DMA SETUP FIS�� ������.
	// DMA SETUP FIS�� ���ڴ� NCQ tag�μ�, ���� ������ ������ �Ϸ���� ���� read/write command �߿��� � ������ �����ϴ� �뵵�̴�.
	// ����̽��� ���� write command �߿��� ���ϴ� ���� ������ ������ ������, �� ������Ʈ������ ������ command ���� ���� ��� �����ϱ�� �Ѵ�.
	// ȣ��Ʈ�� DMA SETUP FIS�� ���� �������� ��� DATA FIS�� ������. DATA FIS�� ũ��� �ִ� 8KB �̴�.
	//
	// [�߿�] DATA FIS ������ �׻� 8KB ������ �̷�����ٰ� ������ �������� ���ؼ��� 8KB �����̴�.
	//
	// ���� ��� ���õ� write command�� total size�� 17KB (34 sectors)��� ��������.
	// ����̽��� ��� 8KB ��ŭ�� ���۰� Ȯ���Ǿ��� ���� DMA SETUP FIS�� ������ ù��° DATA FIS�� �޴´�.
	// ȣ��Ʈ�� �ι�° DATA FIS�� ������ �ʰ� �ϴ� ��ٸ���.
	// ����̽��� �߰��� 8KB ���۰� Ȯ���Ǹ� DMA ACTIVATE FIS�� ȣ��Ʈ���� ������ �ι�° DATA FIS�� �޴´�.
	// ���������� 1KB ���۰� Ȯ���Ǹ� ����° DMA ACTIVATE FIS�� ȣ��Ʈ���� ������ ������ 1KB�� �޴´�.
	// �� �ڼ��� �����ϸ�,
	// (1) ȣ��Ʈ�� SATA IDLE ���¿��� write command�� ������. ����̽��� �����ϰ� ������ ���� �޽����� ������ �ٽ� SATA IDLE ���°� �ȴ�.
	// (2) ����̽��� SATA IDLE ���¿��� write command �߿��� �ϳ��� �����Ͽ� DMA SETUP FIS�� ������ DATA FIS�� �޴´�.
	// (3) ���õ� write command�� total size�� 8KB�� �Ѵ� ��쿡�� �߰������� DMA ACTIVATE FIS�� ������ DATA FIS�� �޴´�.
	//     ù��° DATA FIS�� DMA SETUP FIS�� ���� �����̰�, �ι�°���� DATA FIS�� DMA ACTIVATE FIS�� ���� �����̴�.
	//     DMA SETUP FIS�� ���ڴ� NCQ tag�̰�, DMA ACTIVATE FIS���� ���ڰ� ����.
	// (4) DMA ACTIVATE FIS ������ DATA FIS �޴� ������ �ݺ��ϴٰ�, ������ DATA FIS�� �ް� ���� �ٽ� SATA IDLE ���·� �ǵ��ư���.
	// (5) ���õ� write command�� ���� ��ǻ� SATA ������ ó���� ������ NAND ������ ��ó���� ��������, SATA �Ծ࿡ ���ϸ� ȣ��Ʈ���� Set Device Bits FIS��
	//     ������ ��μ� �ش� write command�� ������ ���� ������ ���ֵǾ� NCQ���� ��������. Set Device Bits FIS�� ���ڴ� NCQ tag�̴�.
	//     WRITE_FUA ����� ��쿡�� �����Ͱ� NAND�� �����ϰ� ������ ������ Set Device Bits FIS�� ������ �Ѵ�.
	//
	// ���� ����̽��� �ʿ��� ���۸� Ȯ������ ���� ä�� DMA SETUP FIS �Ǵ� DMA ACTIVATE FIS�� ������, DATA FIS ���� ���߿�
	// HOLD ��ȣ�� ȣ��Ʈ���� ������ ��� ���ߴ� ���� �����ϴ�. ���� ��� ���۸� 4KB�� Ȯ���ϰ� 8KB¥�� DATA FIS�� ������ �����ϸ�
	// �߰��� buffer overflow error�� �߻��ϴ� ���� �ƴϰ� HOLD ��ȣ�� ���ؼ� ��� ���߰� �ȴ�. SATA link layer�� ����ϴ� �ϵ��� �̷��� ����� ������.
	// �߰� ���۸� �ż��ϰ� Ȯ������ ���ؼ� HOLD ��ȣ�� ���� �ð� ���ӵǸ� ȣ��Ʈ ���� link layer���� time-out error�� �߻��Ѵ�.
	// �ݸ�, DMA ACTIVATE FIS�� ������ �ʰ� ����̸� ȣ��Ʈ�� ���� �������� ��ٸ� �ڿ� time-out error�� �߻��Ѵ�.
	// �ᱹ HOLD ��ȣ�� �̿��� flow control�� link layer �������� ���� ����Ʈ �Ը�� ����ϴ� ����̹Ƿ�, command layer�� ���⿡ �������� �ʴ� ���� �ٶ����ϴ�.
	// ��, �Ź� �����ϰ� �� DATA FIS�� ũ�⸦ �����ؼ� ���۸� �̸� Ȯ���� å���� command layer���� �ִٰ� ���� �Ѵ�.
	//
	// ���� ����� ������ SATA �Ծ࿡ ���ǵ� non-zero buffer offset ����� �������� ���� ��츦 �������� �� ���̴�.
	// non-zero buffer offset ����� ����ϸ� ���� ������ �������� ���� ����� ������ ������ �Ϲ� �Һ��ڿ� PC�� HBA�� non-zero buffer offset �����
	// �������� �ʱ� ������ �� ������Ʈ������ �������� �ʱ�� �Ѵ�.
	//
	// [����] �Ʒ��� bm_alloc_write_buf() �� �����ϸ� ��� return�ϹǷ�, �� ���� ���� ������ �����ϴ� �ڵ尡 ������ �ۼ��� ��.
	// [����] �� �Լ��� DATA_FIS_SIZE_MAX >= SECTORS_PER_SLICE�� �����ϰ� �ۼ��Ǿ���.

	sim_hil_t* hil = &g_sim_hil;
	sim_sata_cmd_t* cmd = hil->write_queue[hil->write_q_front % NCQ_SIZE];	// ȣ��Ʈ ����� ���� (receive_host_cmd()���� ������ �������)
	UINT32 remaining_sectors = cmd->num_sectors_requested;					// ���õ� ȣ��Ʈ ����� total size (���ͼ�)
	UINT32 data_fis_size = MIN(remaining_sectors, DATA_FIS_SIZE_MAX);		// ó�� �����ϰ� �� DATA FIS�� ũ�� (���ͼ�)
	UINT32 ncq_tag = cmd->ncq_tag;
	UINT32 lba = cmd->lba;

	#if OPTION_DRAM_SSD
	UINT32 sector_offset = 0;
	#else
	UINT32 sector_offset = cmd->lba % SECTORS_PER_SLICE;					// �Ҵ� �޾Ƽ� �����͸� ���� �ʰ� �׳� �ǳ� �ٴ� ���� ������ ����
	#endif

	if (g_ftl_cmd_q.rear - V32(g_ftl_cmd_q.front) >= FTL_CMD_Q_SIZE - NCQ_SIZE)
	{
		return;
	}
	else if ((UINT32) (hil->read_q_front - hil->rq_rear_mem[ncq_tag]) <= NCQ_SIZE)
	{
		// ���� ó���Ϸ��� �ϴ� write ����� ���� ������ read ����� ��ġ�� ���� �ʴ´�.

		if (bm_alloc_write_buf(sector_offset + data_fis_size) == FAIL)
		{
			// ���۰� �����ϸ� DMA SETUP FIS�� ������ ���� ���߿� �ٽ� �õ�
			// DMA SETUP FIS�� ������ �ʰ� SATA IDLE ���¿� ���� ������ ���ο� ȣ��Ʈ ����� �ްų� read data�� ���� ��ȸ�� �ִ�.
			return;
		}
	}
	else
	{
		// ���� ó���Ϸ��� �ϴ� write ��ɺ��� �� ���ſ� ������ read ����� ���� �� ������.
		// ��ġ�⸦ �Ϸ��� �ʿ��� �ڿ� ��ü�� �����ϰ� Ȯ���Ǿ��� ������ �Ѵ�. (���� ���� ����)

		if (bm_query_write_buf() < sector_offset + remaining_sectors)
		{
			return;
		}

		bm_alloc_write_buf(sector_offset + data_fis_size);
	}

	if (sector_offset != 0)
	{
		// ȣ��Ʈ ����� ���� ��ġ�� slice ��輱�� align �Ǿ� ���� ���� ���
		// �ǳʶ� ���ۿ��� read_merge_fore() �Լ��� ���Ͽ� NAND���� ���� �����Ͱ� �� ���̴�.

		bm_skip_write_buf(sector_offset);
	}

	hil->write_q_front++;

	internal_delay(SIM_SATA_SETUP_W_DELAY);		// �߿���� ���� �ð� ���� (FIS_DMA_SETUP_W ������ ���۵Ǳ������ ����)

	send_fis_to_host(FIS_DMA_SETUP_W, ncq_tag);

	while (1)
	{
		UINT32 msg_code = wait_message();

		if (msg_code == SIM_EVENT_WDATA_E)	// DATA FIS�� ��������
		{
			bm_import_write_data(hil->data_fis, data_fis_size, lba);

			lba += data_fis_size;

			sim_wake_up(SIM_ENTITY_FTL);

			remaining_sectors -= data_fis_size;

			if (remaining_sectors == 0)
			{
				hil->sata_state = SDS_IDLE;
				break;	// write command �Ϸ� - while loop Ż��
			}
			else
			{
				hil->sata_state = SDS_RX_PAUSE;
			}
		}

		if (hil->sata_state == SDS_RX_PAUSE)
		{
			data_fis_size = MIN(remaining_sectors, DATA_FIS_SIZE_MAX);	// �����ϰ� �� DATA FIS�� ũ�� (���ͼ�)

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
			// ȣ��Ʈ ����� �� ��ġ�� slice ��輱�� align �Ǿ� ���� ���� ���
			// �ǳʶ� ���ۿ��� read_merge_aft() �Լ��� ���Ͽ� NAND���� ���� �����Ͱ� �� ���̴�.

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
	// receive_write_data()�� ���� ����.
	// memory.h�� "read buffer�� ���� ����" ����.
	// read data�� ȣ��Ʈ���� ���� ������ write data�� ���� ���� ���������� DATA FIS ��ü�� �غ�Ǿ��� ���� ������ �����ϱ�� �Ѵ�.
	// DATA FIS�� ������ ���߿� ���� �����Ͱ� �����Ǿ HOLD ��ȣ�� ����ϴ� ���� �ٶ������� �ʱ� �����̴�.
	// DATA FIS�� DATA FIS�� ���̿� �ð� ������ ����� ���� ����ϱ�� �Ѵ�.
	// write�ʹ� �޸�, read������ DMA ACTIVATE FIS�� �������� �ʴ´�. ��, DMA SETUP FIS�� ���� ���� DATA FIS ���۸� �ݺ��Ѵ�.

	sim_hil_t* hil = &g_sim_hil;
	UINT32 ncq_tag = hil->read_queue[hil->read_q_front % NCQ_SIZE];	// ó���� read ����� ����

	if (hil->read_ready[ncq_tag] == FALSE)
	{
		// ���� ù��° DATA FIS�� ���� ��ŭ�� �����Ͱ� Ȯ������ �ʾҴ�.
		// ������ DATA FIS�� ���� �غ� �ȵǾ����� (���� �����Ͱ� ���� NAND���� ������ �ʾ�����) �����ϰ� DMA SETUP FIS�� ������ ���� ��ٸ��� ���� ����.
		// SATA IDLE ���¸� ���������μ� �ٸ� ȣ��Ʈ ����� �ްų� �ٸ� ����� ������ �ۼ����� �� ��ȸ�� ���� ���� �� �ֱ� �����̴�.
		// HIL �߿�� ������ ������ �ݺ������� �˻��ϴ� �δ��� ���� ����, read_ready�� TRUE�� �ٲٴ� ������ FIL�� ����ϱ�� �Ѵ�.

		return;
	}

	hil->read_q_front++;

	sim_sata_cmd_t* cmd = hil->cmd_table[ncq_tag];
	UINT32 remaining_sectors = cmd->num_sectors_requested;			// ���õ� ȣ��Ʈ ����� total size (���ͼ�)
	UINT32 offset = cmd->lba % SECTORS_PER_SLICE;					// ���� ��ġ (slice �������� ���� ��ȣ)
	UINT32 data_fis_size = 0;
	UINT32 lba = cmd->lba;

	internal_delay(SIM_SATA_SETUP_R_DELAY);			// �߿���� ���� �ð� ���� (FIS_DMA_SETUP_R ������ ���۵Ǳ������ ����)

	send_fis_to_host(FIS_DMA_SETUP_R, ncq_tag);

	if (offset != 0)
	{
		bm_discard_read_data(offset);				// ȣ��Ʈ���� ������ �ʰ� ������ ������
	}

	while (1)
	{
		UINT32 msg_code = wait_message();

		if (msg_code == SIM_EVENT_RDATA_E)
		{
			bm_release_read_buf(data_fis_size);		// ������ �Ϸ�� read buffer slot �ݳ�

			remaining_sectors -= data_fis_size;

			if (remaining_sectors == 0)
			{
				UINT32 end_offset = (offset + data_fis_size) % SECTORS_PER_SLICE;

				if (end_offset != 0)
				{
					bm_discard_read_data(SECTORS_PER_SLICE - end_offset);	// ������ ��ġ�� slice ��輱�� align���� ���� ���, ȣ��Ʈ���� ������ ���� �����ʹ� ������.
				}

				hil->sata_state = SDS_IDLE;

				break;	// read command �Ϸ� - while loop Ż��
			}

			hil->sata_state = SDS_TX_PAUSE;
		}
		else if (msg_code != SIM_EVENT_WAKE_UP && msg_code != SIM_EVENT_DMA_SETUP_E)
		{
			continue;
		}

		if (hil->sata_state == SDS_TX_PAUSE)
		{
			data_fis_size = MIN(remaining_sectors, DATA_FIS_SIZE_MAX);	// �̹��� ������ DATA FIS�� ũ�� (���ͼ�)

			if (bm_export_read_data(hil->data_fis, data_fis_size, lba) == OK)
			{
				// ���ϰ��� FAIL �̸� SIM_EVENT_WAKE_UP �ް� �ٽ� �õ�
				send_fis_to_host(FIS_READ_DATA, data_fis_size);
				lba += data_fis_size;
			}
			else
			{
				ASSERT(lba != cmd->lba);	// ���⿡ �ɸ��� read_ready ������ bug�� �ִ� ����
			}
		}
	}

	hil->sdb_bitmap |= BIT(ncq_tag);
}

void hil_notify_read_ready(UINT32 ncq_tag)
{
	// �� �Լ��� HIL�� context ������ ����������, FIL�� ���� ȣ��ȴ�.

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
			// receive_write_data()�� send_read_data()�� ��� ������ ������ ������ ������ �ִ�.
			// ��, ���� ���¸� ���ؾ� �Ѵ�.
			// �ó�����:
			// 1. ȣ��Ʈ�� read ����� ������.
			// 3. FTL�� read ��� ó���� �����Ѵ�.
			// 2. ȣ��Ʈ�� write ����� ������.
			// 3. HIL�� write data ������ �����Ѵ�.
			// 4. FTL�� read buffer�� �����Ͽ� read ��� ó���� ������ ���Ѵ�. (read buffer�� �Ҵ� �޴� ��ü�� FTL�̶�� ����) write ��� ó���� ���������� ���Ѵ�.
			// 5. HIL�� write buffer�� �����Ͽ� write data ������ ������ ���Ѵ�. (write buffer�� �Ҵ� �޴� ��ü�� HIL�̶�� ����) read data ������ ���������� ���Ѵ�. (non-zero buffer offset�� ������� �����Ƿ�)
			// 6. read buffer ������ �ؼ��Ϸ��� HIL�� read data ������ �����ؾ� �ϰ�, write buffer ������ �ؼ��Ϸ��� FTL�� write ��� ó���� �����ؾ� �Ѵ�.
			// ȣ��Ʈ ����� read ������ write�� �Դµ� ���� ����� ���� HIL�� ������ �ۼ����� out-of-order�� �����߱� ������ ������ �߻��Ѵ�.
			// �̷��� ���� ���´� read, write ����� sector count�� ���� Ŭ ���� ���� ����������,
			// ��� ���� ��ÿ� read, write buffer�� ���� �� �־��ٸ� sector count�� �۾Ƶ� ������ �� �ִ�.
			// �ݴ��, HIL�� read data ���� ���߿� ���߰� FTL�� write data ������ ��ٸ��� �ó������� �����Ѵ�. (FTL�� read�� �߰��� suspend �ϰ� write�� �Ѿ�� ���)
			// �ذ�å:
			// �۾� ���߿� ���ҽ� �������� ���Ͽ� ���� ���°� �߻��ϴ� ���� �����ϱ� ����, ������ Ȯ���� ����� ������ out-of-order ó���� �����Ѵ�.
			// ��, HIL�� out-of-order ó���� �õ��� ������ (���� ������ read�� ó������ �ʰ� ���߿� ������ write�� ���� ó��) write buffer�� ���� ������ �˻��Ѵ�.

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
			// sim_hil_main() ���ο��� FTL�κ��� FB_FTL_READY ������ HOST ���� SIM_EVENT_HELLO ���� ���̴�.

			hil_init();
			sim_hil_main();		// SIM_EVENT_POWER_OFF ������ sim_hil_main() ���κ��� ����
		}
		else if (msg_code == SIM_EVENT_END_SIMULATION)
		{
			break;
		}
	}
}
