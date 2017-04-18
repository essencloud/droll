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


#ifndef FTL_H
#define FTL_H


// HIL�� FTL ������ �������̽��� ������ ���� ��ҷ� �̷������.
//
// 1. g_ftl_cmd_q
//    HIL���� FTL�� �������� command queue
//
// 2. g_feedback_q
//    �Ϻ� ����� ��쿡�� FTL�� HIL���� ó�� ����� �˷��� �ʿ䰡 �ִ�.
//
// 3. hil_notify_read_ready()
//    HIL�� �����ϰ� FTL�� ȣ���ϴ� �Լ��μ�, REQ_HOST_READ ����� ó���� �ʿ��ϴ�.
//    feed back queue�� ������� �ʰ� ������ �Լ��� �����ϴ� ������ HIL�� ó�� �δ��� �����ֱ� ���� ���̴�.


////////////////////////////////////////////
// FTL Command Queue
// HIL���� FTL�� �������� command queue
// ȣ��Ʈ�κ��� ���� ��� �Ӹ� �ƴ϶�
// HIL�� ��ü������ ���� ��ɵ�
// �� ť�� ���Ͽ� FTL���� ���޵ȴ�.
////////////////////////////////////////////

typedef enum
{
	REQ_HOST_READ = 0,
	REQ_HOST_WRITE,
	REQ_TRIM,
	REQ_FAST_FLUSH,
	REQ_SLOW_FLUSH,

	// ���� �׸���� ������ �߿��ϹǷ� �ٲ��� �� ��. (script_cmd_t�� ��ġ�ؾ� ��)

	REQ_STOP,
	REQ_INTERNAL_READ,
	REQ_INTERNAL_WRITE,
	REQ_READ_VERIFY,
	REQ_ERASE_ALL,
	REQ_RESET_ERASE_COUNT,
	REQ_UPDATE_FIRMWARE,
	REQ_DISABLE_WRITE_CACHE,

	REQ_ABORTED = 0x80	// bit flag
} req_t;

typedef struct
{
	UINT8	req_code;		// REQ_XXX
	UINT8	ncq_tag;		// FF8 for non-NCQ commands
	UINT16	sector_count;
	UINT32	lba;

} ftl_cmd_t;

#define FTL_CMD_Q_SIZE		128	// must be power of two; must be at least 32 (SATA NCQ size)

typedef struct
{
	ftl_cmd_t queue[FTL_CMD_Q_SIZE];

	UINT32	front;	// FTL�� ���Ͽ� ����
	UINT32	rear;	// HIL�� ���Ͽ� ����

} ftl_cmd_queue_t;

extern ftl_cmd_queue_t g_ftl_cmd_q;	// FTL Command Queue



////////////////////
// feedback queue
////////////////////

typedef enum { FB_FTL_READY, FB_TRIM_DONE, FB_FLUSH_DONE } fbc_t;

typedef struct
{
	UINT8	code;	// FB_XXX
	UINT8	arg;
	UINT16	unused;

} feedback_t;

#define FEEDBACK_Q_SIZE		FTL_CMD_Q_SIZE

typedef struct
{
	feedback_t queue[FEEDBACK_Q_SIZE];

	UINT32	front;
	UINT32	rear;
} feedback_queue_t;

extern feedback_queue_t g_feedback_q;


void ftl_format(void);
void ftl_open(void);
void ftl_main(void);
void hil_notify_read_ready(UINT32 ncq_tag);
void send_feedback_to_hil(UINT8 code, UINT8 arg);


#endif	// FTL_H

