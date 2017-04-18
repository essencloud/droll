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


#ifndef SIMBASE_H
#define SIMBASE_H

/////////////////////////////////
// �ùķ��̼ǿ� �����ϴ� ��ü��
/////////////////////////////////

enum
{
	// dram_ssd.c�� ����ϴ� ��쿡�� FIL�� NAND�� �ƹ� �͵� ���� ����

	SIM_ENTITY_HOST = 0,
	SIM_ENTITY_HIL,
	SIM_ENTITY_FTL,
	SIM_ENTITY_FIL,
	SIM_ENTITY_NAND,
	NUM_ENTITIES
};

void sim_host_thread(void* arg_list);
void sim_hil_thread(void* arg_list);
void sim_ftl_thread(void* arg_list);
void sim_fil_thread(void* arg_list);
void sim_nand_thread(void* arg_list);


//////////////
// ��ȣ ����
//////////////

extern CRITICAL_SECTION g_cs_msg_pool;
extern CRITICAL_SECTION g_cs_msg_q[NUM_ENTITIES];
extern CRITICAL_SECTION g_cs_flash_cmd_table;


//////////////////////
// Simulation Events
//////////////////////

typedef enum
{
	// SATA events
	// H2D�� D2H�� ���� host-to-device, device-to-host�� �ǹ�
	// DMA SETUP FIS�� DMA ACTIVATE FIS�� ���̴� receive_data()���� ����

	SIM_EVENT_SATA_CMD_S,		// Register FIS H2D�� ���� (command)
	SIM_EVENT_SATA_CMD_E,		// Register FIS H2D�� �� (command)
	SIM_EVENT_SATA_ACK_S,		// Register FIS D2H�� ���� (NCQ command ���� ���, non-NCQ command ó�� ��)
	SIM_EVENT_SATA_ACK_E,		// Register FIS D2H�� �� (NCQ command ���� ���, non-NCQ command ó�� ��)
	SIM_EVENT_SDB_S,			// Set Device Bits FIS D2H�� ���� (NCQ command ó���� �Ϸ�Ǿ����� �ǹ�)
	SIM_EVENT_SDB_E,			// Set Device Bits FIS D2H�� �� (NCQ command ó���� �Ϸ�Ǿ����� �ǹ�)
	SIM_EVENT_DMA_SETUP_S,		// DMA SETUP FIS D2H�� ���� (write data�� �����޶�� �ǹ�)
	SIM_EVENT_DMA_SETUP_E,		// DMA SETUP FIS D2H�� �� (write data�� �����޶�� �ǹ�)
	SIM_EVENT_DMA_ACTV_S,		// DMA ACTIVATE FIS D2H�� ���� (write data�� �����޶�� �ǹ�)
	SIM_EVENT_DMA_ACTV_E,		// DMA ACTIVATE FIS D2H�� �� (write data�� �����޶�� �ǹ�)
	SIM_EVENT_RDATA_S,			// DATA FIS D2H�� ����
	SIM_EVENT_RDATA_E,			// DATA FIS D2H�� ��
	SIM_EVENT_WDATA_S,			// DATA FIS H2D�� ����
	SIM_EVENT_WDATA_E,			// DATA FIS H2D�� ��
	SIM_EVENT_HELLO,			// power on reset ���� SSD�� �غ��

	// ��Ÿ

	SIM_EVENT_POWER_ON,
	SIM_EVENT_POWER_OFF,
	SIM_EVENT_END_SIMULATION,
	SIM_EVENT_PRINT_STAT,

	SIM_EVENT_WAKE_UP,
	SIM_EVENT_FEEDBACK,
	SIM_EVENT_DELAY,

	SIM_EVENT_TYPES,

} se_t;


typedef struct sim_msg
{
	union
	{
		struct sim_msg* link_ptr;	// �� ������ linked list
		UINT64	arg_64;				// �޽��� ���뿡 ���� �߰� ����
	};

	UINT64	when;					// �̺�Ʈ�� �߻��ϴ� �ð�

	UINT16	code;					// SIM_EVENT_XXX
	UINT16	arg_16;
	UINT32	seq_number;				// debug break point ������ ���� ����

	UINT32	arg_32;
	UINT32	arg_32_a;

} sim_message_t;

#define MESSAGE_POOL_SIZE	256		// �����ؼ� ������ ���� ������ ���δ�.
#define MSG_Q_SIZE			256		// must be power of two. �����ؼ� ������ ���� ������ ���δ�.

typedef struct
{
	sim_message_t* queue[MSG_Q_SIZE];
	UINT32	front;
	UINT32	rear;

} msg_queue_t;


///////////////////////
// Simulation Context
///////////////////////

typedef struct
{
	// message�� �߻� �ð��� ���� �ð��� ���� �ְ�, ������ �ð��� �帥 ���ϼ��� �ִ�.
	// �߽��ڴ� ���� �Ǵ� �̷��� Ư�� ������ �޽����� ��޵ǵ��� �����Ѵ�. ������ �ð��� �����ؼ��� �ȵȴ�.
	// �����ڴ� ������ �ð��� �޽����� �����ϰ�, �� ������ �ٷ� �ش� event�� �߻� �ð��̴�.
	//
	// message pool�� sim_message_t ����ü�� �����̴�.
	// message pool�� �� ���Ե��� linked list ���·� ����Ǿ� �ִ�.
	// event�� �߻���Ű�� ���� entity�� message pool�� �� ������ �ϳ� �޾Ƽ� ����ü ������ ä���,
	// �̸� ������ entity���� �޽����� ������. �̷��� �ڱ� �ڽſ��� ���� ���� �ִ�.
	// �� entity�� �ڽŸ��� �޽��� ���� queue�� �ϳ��� ������.
	// entity A�� entity B���� �޽����� ������ ���� A�� B�� �޽��� ���� queue�� �޽����� �ִ� ���� �ǹ��Ѵ�.
	// queue�� �޽����� ���� ����, sim_message_t ����ü�� ��°�� �����ϴ� ���� �ƴϰ� ����ü �ּҸ� ����Ű�� ������ ���� �ִ´�.
	// �����ڴ� ó���� ���� �ڿ� �ش� ����ü instance�� message pool�� �ݳ��Ѵ�.

	time_t			sim_begin_time;				// �ùķ����͸� ������ �ð� (real world time)
	UINT64			current_time;				// nanoseconds
	msg_queue_t		msg_queue[NUM_ENTITIES];
	sim_message_t	message_pool[MESSAGE_POOL_SIZE];
	sim_message_t*	free_msg_slot;
	UINT32			num_free_msg_slots;
	UINT32			wake_up[NUM_ENTITIES];
	UINT32			last_wake_up[NUM_ENTITIES];
	UINT32			num_waiting_entities;
	BOOL32			thread_sync;
	UINT32			session;					// ���� ��ȣ - session�� �����ϸ� ������ ���� ������.
	UINT32			random_seed;

	#if OPTION_THREAD_SYNC == 2
	UINT32			unused;
	HANDLE			msg_arrived[NUM_ENTITIES];
	#endif

	#if OPTION_THREAD_SYNC == 3
	BOOL32			msg_arrived[NUM_ENTITIES];
	#endif

} sim_context_t;

extern sim_context_t g_sim_context;


//////////////
// �Լ���
//////////////

extern thread_local mt19937* g_random;		// uniform random number generator

INLINE UINT32 random(UINT32 min, UINT32 max)
{
	if (min > max)
	{
		while (1);
	}

	UINT64 interval = (UINT64) max - min + 1;
	UINT64 temp_1 = (UINT64) (*g_random)() * interval;
	UINT32 temp_2 = (UINT32) (temp_1 >> 32);

	return temp_2 + min;
}


__declspec(thread) static UINT32 g_msg_seq_number;

extern jmp_buf g_jump_buf[NUM_ENTITIES];

static __forceinline sim_message_t* sim_new_message(void)
{
	// Message pool�� �� ������ �Ҵ�

	MUTEX_LOCK(&g_cs_msg_pool);

	ASSERT(g_sim_context.num_free_msg_slots != 0);	// ���� �� ������ �����ؼ� ������ ���� MESSAGE_POOL_SIZE �� ������ ���̸� �ȴ�.
	g_sim_context.num_free_msg_slots--;
	sim_message_t* slot = g_sim_context.free_msg_slot;
	g_sim_context.free_msg_slot = slot->link_ptr;
	slot->seq_number = g_msg_seq_number++;

	MUTEX_UNLOCK(&g_cs_msg_pool);

	return slot;
}

void sim_send_message(sim_message_t* msg, UINT32 recipient);
sim_message_t* sim_receive_message(UINT32 entity);
void sim_release_message_slot(sim_message_t* slot);

INLINE void sim_wake_up(UINT32 entity)
{
	V32(g_sim_context.wake_up[entity])++;	// ATOMIC_INCREMENT �ʿ� ����?
}

#endif	// SIMBASE_H
