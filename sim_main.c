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

sim_context_t g_sim_context;

CRITICAL_SECTION g_cs_msg_pool;
CRITICAL_SECTION g_cs_msg_q[NUM_ENTITIES];
CRITICAL_SECTION g_cs_flash_cmd_table;

jmp_buf g_jump_buf[NUM_ENTITIES];

thread_local mt19937* g_random;		// uniform random number generator


// profile에서 sim_send_message의 비중도 참고

void sim_send_message(sim_message_t* new_msg, UINT32 recipient)
{
	// 메시지 큐의 항목들은 event 발생 예정시각에 따라 sort 되어있다.
	// 큐에 새로 들어오는 항목은 무조건 맨 뒤에 놓이는 것이 아니고 적절한 위치를 찾는다.

	MUTEX_LOCK(g_cs_msg_q + recipient);

	msg_queue_t* msg_q = g_sim_context.msg_queue + recipient;
	ASSERT(msg_q->rear - msg_q->front < MSG_Q_SIZE);	// 만일 큐가 가득 차서 에러가 나면 MSG_Q_SIZE를 늘이면 된다.

	if (new_msg->when == g_sim_context.current_time)
	{
		UINT32 msg_q_index = msg_q->front;

		while (msg_q_index != msg_q->rear)
		{
			sim_message_t* existing_msg = msg_q->queue[msg_q_index % MSG_Q_SIZE];

			if (existing_msg->when > new_msg->when)
			{
				break;
			}
			else
			{
				msg_q->queue[(msg_q_index - 1) % MSG_Q_SIZE] = existing_msg;	// 기존에 있던 항목을 앞으로 한칸 밀어냄
				msg_q_index++;
			}
		}

		msg_q->queue[(msg_q_index - 1) % MSG_Q_SIZE] = new_msg; 	// 새 메시지가 들어갈 자리
		msg_q->front--;
	}
	else
	{
		// 큐의 맨 뒤에서부터 역순으로 검색하여 새 메시지의 위치를 결정

		UINT32 msg_q_index = msg_q->rear - 1;

		while (msg_q_index != msg_q->front - 1)
		{
			sim_message_t* existing_msg = msg_q->queue[msg_q_index % MSG_Q_SIZE];

			if (existing_msg->when <= new_msg->when)
			{
				break;
			}
			else
			{
				msg_q->queue[(msg_q_index + 1) % MSG_Q_SIZE] = existing_msg;	// 기존에 있던 항목을 뒤로 한칸 밀어냄
				msg_q_index--;
			}
		}

		msg_q->queue[(msg_q_index + 1) % MSG_Q_SIZE] = new_msg; 	// 새 메시지가 들어갈 자리
		msg_q->rear++;
	}

	MUTEX_UNLOCK(g_cs_msg_q + recipient);
}

sim_message_t* sim_receive_message(UINT32 entity)
{
	msg_queue_t* msg_q = g_sim_context.msg_queue + entity;
	BOOL8 slept = FALSE;

	while (1)
	{
		// 현재 시각에 수신할 메시지가 있으면 잠들지 말고 즉시 수신한다.

		if (V32(msg_q->front) != V32(msg_q->rear))
		{
			sim_message_t* msg = msg_q->queue[msg_q->front % MSG_Q_SIZE];

			if (msg->when == V64(g_sim_context.current_time))
			{
				MUTEX_LOCK(g_cs_msg_q + entity);

				UINT32 front = V32(msg_q->front);
				msg = msg_q->queue[front % MSG_Q_SIZE];
				V32(msg_q->front) = front + 1;

				MUTEX_UNLOCK(g_cs_msg_q + entity);

				g_sim_context.last_wake_up[entity] = V32(g_sim_context.wake_up[entity]);

				if (msg->code == SIM_EVENT_POWER_OFF)
				{
					// longjmp를 호출하면 리턴하지 않으므로 아래의 return msg; 문장이 수행되지 않는다.
					// 그 대신 sim_XXX_thread() 에서 호출했던 setjmp() 함수로부터 리턴하게 된다.

					sim_release_message_slot(msg);

					longjmp(g_jump_buf[entity], 1);
				}

				return msg;
			}
		}

		// 현재 시각에 메시지는 없지만 혹시 wake_up 이 있으면 잠들지 말고 리턴한다.
		// wake_up 은 SIM_EVENT_WAKE_UP 의 단점을 해결하기 위한 수단이다.

		UINT32 wake_up = V32(g_sim_context.wake_up[entity]);
		UINT32 last_wake_up = g_sim_context.last_wake_up[entity];

		if (wake_up != last_wake_up)
		{
			g_sim_context.last_wake_up[entity] = wake_up;
			return NULL;
		}

		// 현재 시각이 증가하거나 또는 새로운 메시지(또는 wake_up)이 올 때까지 자면서 기다린다.

		ASSERT(slept == FALSE);		// 한번 자고 일어났으면 함수에서 나가는 것이 정상이다.

		#if OPTION_THREAD_SYNC == 3
		V32(g_sim_context.msg_arrived[entity]) = FALSE;
		#endif

		ATOMIC_INCREMENT(&g_sim_context.num_waiting_entities);

		#if OPTION_THREAD_SYNC == 2
		WaitForSingleObject(g_sim_context.msg_arrived[entity], INFINITE);
		#else
		while (V32(g_sim_context.msg_arrived[entity]) == FALSE);
		#endif

		slept = TRUE;
	}
}

void sim_release_message_slot(sim_message_t* slot)
{
	MUTEX_LOCK(&g_cs_msg_pool);

	ASSERT(g_sim_context.num_free_msg_slots < MESSAGE_POOL_SIZE);
	g_sim_context.num_free_msg_slots++;
	slot->link_ptr = g_sim_context.free_msg_slot;
	g_sim_context.free_msg_slot = slot;

	MUTEX_UNLOCK(&g_cs_msg_pool);
}

void sim_error(char* file, int line)
{
	char string[128];

	printf("\ncrashed at %s:%d\n", file, line);
	printf("session %u\n", g_sim_context.session);
	printf("random seed = %u\n", g_sim_context.random_seed);
	printf("g_sim_context.current_time = %llu\n", g_sim_context.current_time);

	time_t current_time;
	time(&current_time);

	UINT64 diff = current_time - g_sim_context.sim_begin_time;
	printf("Simulation has been running for %s.\n", format_time(diff*1000000000, NULL));

	ctime_s(string, sizeof(string), &current_time);
	printf("crash time = %s\n", string);
}

static void sim_init(void)
{
	STOSQ(&g_sim_context, 0, sizeof(g_sim_context));

	time(&g_sim_context.sim_begin_time);
	char string[128];
	ctime_s(string, sizeof(string), &g_sim_context.sim_begin_time);
	printf("\nSimulation begins at %s\n", string);

	for (UINT32 i = 0; i < MESSAGE_POOL_SIZE; i++)
	{
		sim_message_t* slot = g_sim_context.message_pool + i;
		slot->link_ptr = slot + 1;	// linked list of free slots
	}

	g_sim_context.free_msg_slot = g_sim_context.message_pool;
	g_sim_context.num_free_msg_slots = MESSAGE_POOL_SIZE;

	InitializeCriticalSectionAndSpinCount(&g_cs_flash_cmd_table, 10000);
	InitializeCriticalSectionAndSpinCount(&g_cs_msg_pool, 10000);

	for (UINT32 i = 0; i < NUM_ENTITIES; i++)
	{
		InitializeCriticalSectionAndSpinCount(g_cs_msg_q + i, 10000);
	}

	#if RANDOM_SEED
	{
		// 에러를 재현하려면 화면에 출력된 random_seed 값을 RANDOM_SEED 값으로 고치고 다시 돌린다.
		g_sim_context.random_seed = RANDOM_SEED;
	}
	#else
	{
		LARGE_INTEGER temp;
		QueryPerformanceCounter(&temp);
		g_sim_context.random_seed = (UINT32) temp.LowPart;
	}
	#endif

	for (UINT32 entity = 0; entity < NUM_ENTITIES; entity++)
	{
		#if OPTION_THREAD_SYNC == 2
		g_sim_context.msg_arrived[entity] = CreateEvent(NULL, FALSE, FALSE, NULL);
		#endif
	}

	_beginthread(sim_host_thread, 0, NULL);
	_beginthread(sim_hil_thread, 0, NULL);
	_beginthread(sim_ftl_thread, 0, NULL);
	_beginthread(sim_fil_thread, 0, NULL);
	_beginthread(sim_nand_thread, 0, NULL);

	g_random = new mt19937(g_sim_context.random_seed);
}

BOOL ctrl_c_handler(DWORD ctrl_type)
{
	if (ctrl_type== CTRL_C_EVENT)
	{
		print_sim_progress();
		return TRUE;
	}
	else
	{
		return FALSE;
	}
}

int main(void)
{
	SetConsoleCtrlHandler((PHANDLER_ROUTINE) ctrl_c_handler, TRUE);

	print_banner();
	sanity_check();
	sim_init();

	// main thread

	while (1)
	{
		while (V32(g_sim_context.num_waiting_entities) != NUM_ENTITIES);

		// 모든 entity의 incoming message queue를 뒤져서 발생 시각이 가장 이른 메시지를 찾아냄
		// wake_up 은 현재 시각에 전달할 메시지로 간주되므로 수신자를 깨운다. 실제로 현재 시각에 메시지를 받을 스레드가 있으면 덩달아서 깨운다.

		UINT64 earliest = FF64;
		UINT64 now = g_sim_context.current_time;
		BOOL8 go[NUM_ENTITIES];
		UINT32 msg_count = 0, wake_up_count = 0;

		for (UINT32 entity = 0; entity < NUM_ENTITIES; entity++)
		{
			if (V32(g_sim_context.wake_up[entity]) != V32(g_sim_context.last_wake_up[entity]))
			{
				go[entity] = TRUE;
				wake_up_count++;
			}
			else
			{
				go[entity] = FALSE;

				msg_queue_t* msg_q = g_sim_context.msg_queue + entity;

				if (V32(msg_q->front) == V32(msg_q->rear))
					continue;

				sim_message_t* message = msg_q->queue[msg_q->front % MSG_Q_SIZE];

				if (message->when == now)
				{
					msg_count++;
					go[entity] = TRUE;
				}
				else if (message->when < earliest)
				{
					earliest = message->when;
				}
			}
		}

		if (wake_up_count == 0 && msg_count == 0)
		{
			if (earliest == FF64)
			{
				// HOST가 thread_sync 를 설정하고 기다리고 있다.
				// thread_sync == FALSE 이면 누군가 이벤트 생성을 깜빡 잊었거나 교착상태에 빠진 것이다.

				CHECK(V32(g_sim_context.thread_sync) == TRUE);
				V32(g_sim_context.thread_sync) = FALSE;
				V32(g_sim_context.wake_up[SIM_ENTITY_HOST])++;
				wake_up_count = 1;

				go[SIM_ENTITY_HOST] = TRUE;
			}
			else
			{
				// 현재 시각은 아니지만 전달할 메시지가 있긴 있으므로, current_time을 증가시키고 전달한다.

				#if OPTION_INSIGHT
				insight_add_record();
				#endif

				// message queue 검사를 먼저 다 하고나서 스레드들을 한꺼번에 깨움
				// 하나씩 깨우면 message queue 검사하는 동안에 race condition 발생

				for (UINT32 entity = 0; entity < NUM_ENTITIES; entity++)
				{
					msg_queue_t* msg_q = g_sim_context.msg_queue + entity;

					if (msg_q->front != msg_q->rear)
					{
						sim_message_t* message = msg_q->queue[msg_q->front % MSG_Q_SIZE];

						if (message->when == earliest)
						{
							go[entity] = TRUE;
							msg_count++;
						}
					}
				}

				V64(g_sim_context.current_time) = earliest;
			}
		}

		V32(g_sim_context.num_waiting_entities) = NUM_ENTITIES - msg_count - wake_up_count;

		for (UINT32 entity = 0; entity < NUM_ENTITIES; entity++)
		{
			if (go[entity] == TRUE)
			{
				#if OPTION_THREAD_SYNC == 2
				SetEvent(g_sim_context.msg_arrived[entity]);
				#else
				V32(g_sim_context.msg_arrived[entity]) = TRUE;
				#endif
			}
		}
	}

	return 0;
}


