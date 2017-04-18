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


// FTL 종류에 관계 없이 내용이 변하지 않는 함수들


#include "droll.h"

void send_feedback_to_hil(UINT8 code, UINT8 arg)
{
	feedback_queue_t* q = &g_feedback_q;

	ASSERT(q->rear - V32(q->front) < FEEDBACK_Q_SIZE);

	feedback_t* fb = q->queue + q->rear%FEEDBACK_Q_SIZE;
	fb->code = code;
	fb->arg = arg;
	V32(q->rear) = q->rear + 1;

	sim_message_t* msg = sim_new_message();
	msg->code = SIM_EVENT_FEEDBACK;
	msg->when = g_sim_context.current_time;
	sim_send_message(msg, SIM_ENTITY_HIL);
}

void sim_ftl_thread(void* arg_list)
{
	UNREFERENCED_PARAMETER(arg_list);

	PRINT_NUM(NUM_DIES);
	PRINT_NUM(BIG_BLKS_PER_DIE);
	PRINT_NUM(WLS_PER_BLK);
	PRINT_NUM(SLICES_PER_BIG_WL);
	PRINT_NUM(SLICES_PER_BIG_BLK);
	printf("\n");

	if (setjmp(g_jump_buf[SIM_ENTITY_FTL]) != 0)
	{
		// FTL이 ftl_main() 함수를 수행하는 동안에 SIM_EVENT_POWER_OFF 를 받으면 longjmp에 의해서 여기로 돌아온다.
		// ftl_main() 함수는 while (1) 이므로 return하는 경우는 없다.
		// 특별히 할 일은 없고, 아래로 내려가서 SIM_EVENT_POWER_ON 기다린다.
		// SIM_EVENT_POWER_ON 보다 먼저 수신되는 각종 이벤트 메시지들은 이제 무의미하게 되었으므로 받아서 그냥 버린다.
	}

	while (1)
	{
		sim_message_t* msg = sim_receive_message(SIM_ENTITY_FTL);

		if (msg != NULL)
		{
			UINT32 msg_code = msg->code;

			if (msg_code == SIM_EVENT_POWER_ON)						// 실제 하드웨어에서는 CPU 0가 CPU 1을 살려줌으로써 FTL이 시작됨 (start_cpu_1() 함수 참고)
			{
				STOSD(&g_ftl_cmd_q, 0, sizeof(g_ftl_cmd_q));		// HIL과 FTL 사이의 인터페이스를 초기화
				STOSD(&g_feedback_q, 0, sizeof(g_feedback_q));

				sim_send_message(msg, SIM_ENTITY_HIL);				// 메시지를 HIL에게 전달

				if (g_sim_context.session == 0)
				{
					g_random = new mt19937(g_sim_context.random_seed);

					ftl_format();
					send_feedback_to_hil(FB_FTL_READY, NULL);		// 초기화가 완료되었다고 HIL에게 알림
				}
				else
				{
					ftl_open(); 									// FTL 초기화
					send_feedback_to_hil(FB_FTL_READY, NULL);		// 초기화가 완료되었다고 HIL에게 알림

					ftl_main();
				}
			}
			else
			{
				sim_release_message_slot(msg);

				if (msg_code == SIM_EVENT_END_SIMULATION)
					break;
			}
		}
	}
}

