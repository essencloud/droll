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


#ifndef INSIGHT_H
#define INSIGHT_H


typedef struct
{
	UINT8	ncq_cmd;
	UINT8	non_ncq_cmd;
	UINT8	read;
	UINT8	write;
	UINT8	sdb;
	UINT8	cmd_busy;		// SATA command layer not idle
	UINT8	link_busy;		// SATA link layer not idle
	UINT8	collision;		// SATA collision이 발생할 때마다 토글

	UINT8	tag[NCQ_SIZE];

	UINT8	die[NUM_DIES];
	UINT8	channel_user[NUM_CHANNELS];

	UINT16	die_q_length[NUM_DIES];
	UINT16	ncq_length;
	UINT16	ftl_cmd_q_length;
	UINT16	write_q_length;
	UINT16	read_buf_level;
	UINT16	write_buf_level;

} insight_t;


extern insight_t g_insight;


#if OPTION_INSIGHT
void insight_begin(UINT32 limit_megabyte);
void insight_end(void);
void insight_add_record(void);
#else
#define insight_begin(...)
#define insight_end(...)
#define insight_add_record(...)
#endif

#if OPTION_INSIGHT
#define INSIGHT(...)		__VA_ARGS__
#else
#define INSIGHT(...)
#endif

#endif	// INSIGHT_H

