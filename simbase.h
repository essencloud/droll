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
// 시뮬레이션에 참여하는 객체들
/////////////////////////////////

enum
{
	// dram_ssd.c를 사용하는 경우에는 FIL과 NAND는 아무 것도 하지 않음

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
// 상호 배제
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
	// H2D와 D2H는 각각 host-to-device, device-to-host를 의미
	// DMA SETUP FIS와 DMA ACTIVATE FIS의 차이는 receive_data()에서 설명

	SIM_EVENT_SATA_CMD_S,		// Register FIS H2D의 시작 (command)
	SIM_EVENT_SATA_CMD_E,		// Register FIS H2D의 끝 (command)
	SIM_EVENT_SATA_ACK_S,		// Register FIS D2H의 시작 (NCQ command 수신 즉시, non-NCQ command 처리 후)
	SIM_EVENT_SATA_ACK_E,		// Register FIS D2H의 끝 (NCQ command 수신 즉시, non-NCQ command 처리 후)
	SIM_EVENT_SDB_S,			// Set Device Bits FIS D2H의 시작 (NCQ command 처리가 완료되었음을 의미)
	SIM_EVENT_SDB_E,			// Set Device Bits FIS D2H의 끝 (NCQ command 처리가 완료되었음을 의미)
	SIM_EVENT_DMA_SETUP_S,		// DMA SETUP FIS D2H의 시작 (write data를 보내달라는 의미)
	SIM_EVENT_DMA_SETUP_E,		// DMA SETUP FIS D2H의 끝 (write data를 보내달라는 의미)
	SIM_EVENT_DMA_ACTV_S,		// DMA ACTIVATE FIS D2H의 시작 (write data를 보내달라는 의미)
	SIM_EVENT_DMA_ACTV_E,		// DMA ACTIVATE FIS D2H의 끝 (write data를 보내달라는 의미)
	SIM_EVENT_RDATA_S,			// DATA FIS D2H의 시작
	SIM_EVENT_RDATA_E,			// DATA FIS D2H의 끝
	SIM_EVENT_WDATA_S,			// DATA FIS H2D의 시작
	SIM_EVENT_WDATA_E,			// DATA FIS H2D의 끝
	SIM_EVENT_HELLO,			// power on reset 이후 SSD가 준비됨

	// 기타

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
		struct sim_msg* link_ptr;	// 빈 슬롯의 linked list
		UINT64	arg_64;				// 메시지 내용에 따른 추가 정보
	};

	UINT64	when;					// 이벤트가 발생하는 시각

	UINT16	code;					// SIM_EVENT_XXX
	UINT16	arg_16;
	UINT32	seq_number;				// debug break point 설정을 위한 정보

	UINT32	arg_32;
	UINT32	arg_32_a;

} sim_message_t;

#define MESSAGE_POOL_SIZE	256		// 부족해서 에러가 나면 적당히 늘인다.
#define MSG_Q_SIZE			256		// must be power of two. 부족해서 에러가 나면 적당히 늘인다.

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
	// message의 발생 시각은 현재 시각일 수도 있고, 지정된 시간이 흐른 뒤일수도 있다.
	// 발신자는 현재 또는 미래의 특정 시점에 메시지가 배달되도록 예약한다. 과거의 시각을 예약해서는 안된다.
	// 수신자는 지정된 시각에 메시지를 수신하고, 이 시점이 바로 해당 event의 발생 시각이다.
	//
	// message pool은 sim_message_t 구조체의 집합이다.
	// message pool의 빈 슬롯들은 linked list 형태로 연결되어 있다.
	// event를 발생시키고 싶은 entity는 message pool의 빈 슬롯을 하나 받아서 구조체 내용을 채우고,
	// 이를 접수할 entity에게 메시지를 보낸다. 미래의 자기 자신에게 보낼 수도 있다.
	// 각 entity는 자신만의 메시지 수신 queue를 하나씩 가진다.
	// entity A가 entity B에게 메시지를 보내는 것은 A가 B의 메시지 수신 queue에 메시지를 넣는 것을 의미한다.
	// queue에 메시지를 넣을 때에, sim_message_t 구조체를 통째로 복사하는 것이 아니고 구조체 주소를 가리키는 포인터 값만 넣는다.
	// 수신자는 처리가 끝난 뒤에 해당 구조체 instance를 message pool에 반납한다.

	time_t			sim_begin_time;				// 시뮬레이터를 시작한 시각 (real world time)
	UINT64			current_time;				// nanoseconds
	msg_queue_t		msg_queue[NUM_ENTITIES];
	sim_message_t	message_pool[MESSAGE_POOL_SIZE];
	sim_message_t*	free_msg_slot;
	UINT32			num_free_msg_slots;
	UINT32			wake_up[NUM_ENTITIES];
	UINT32			last_wake_up[NUM_ENTITIES];
	UINT32			num_waiting_entities;
	BOOL32			thread_sync;
	UINT32			session;					// 세션 번호 - session이 증가하면 전원이 껐다 켜진다.
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
// 함수들
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
	// Message pool의 빈 슬롯을 할당

	MUTEX_LOCK(&g_cs_msg_pool);

	ASSERT(g_sim_context.num_free_msg_slots != 0);	// 만일 빈 슬롯이 부족해서 에러가 나면 MESSAGE_POOL_SIZE 를 적당히 늘이면 된다.
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
	V32(g_sim_context.wake_up[entity])++;	// ATOMIC_INCREMENT 필요 없나?
}

#endif	// SIMBASE_H
