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


// FTL ������ ���� ���� ������ ������ �ʴ� �Լ���


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
		// FTL�� ftl_main() �Լ��� �����ϴ� ���ȿ� SIM_EVENT_POWER_OFF �� ������ longjmp�� ���ؼ� ����� ���ƿ´�.
		// ftl_main() �Լ��� while (1) �̹Ƿ� return�ϴ� ���� ����.
		// Ư���� �� ���� ����, �Ʒ��� �������� SIM_EVENT_POWER_ON ��ٸ���.
		// SIM_EVENT_POWER_ON ���� ���� ���ŵǴ� ���� �̺�Ʈ �޽������� ���� ���ǹ��ϰ� �Ǿ����Ƿ� �޾Ƽ� �׳� ������.
	}

	while (1)
	{
		sim_message_t* msg = sim_receive_message(SIM_ENTITY_FTL);

		if (msg != NULL)
		{
			UINT32 msg_code = msg->code;

			if (msg_code == SIM_EVENT_POWER_ON)						// ���� �ϵ������� CPU 0�� CPU 1�� ��������ν� FTL�� ���۵� (start_cpu_1() �Լ� ����)
			{
				STOSD(&g_ftl_cmd_q, 0, sizeof(g_ftl_cmd_q));		// HIL�� FTL ������ �������̽��� �ʱ�ȭ
				STOSD(&g_feedback_q, 0, sizeof(g_feedback_q));

				sim_send_message(msg, SIM_ENTITY_HIL);				// �޽����� HIL���� ����

				if (g_sim_context.session == 0)
				{
					g_random = new mt19937(g_sim_context.random_seed);

					ftl_format();
					send_feedback_to_hil(FB_FTL_READY, NULL);		// �ʱ�ȭ�� �Ϸ�Ǿ��ٰ� HIL���� �˸�
				}
				else
				{
					ftl_open(); 									// FTL �ʱ�ȭ
					send_feedback_to_hil(FB_FTL_READY, NULL);		// �ʱ�ȭ�� �Ϸ�Ǿ��ٰ� HIL���� �˸�

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

