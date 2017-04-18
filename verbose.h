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


#ifndef VERBOSE_H
#define VERBOSE_H

typedef struct
{
	UINT64	begin_time;	// 이 통계를 시작한 시각 (virtual clock)

	UINT64	num_commands;

	UINT64	num_read_commands;
	UINT64	num_read_sectors;
	UINT64	read_latency_sum;
	UINT64	read_latency_max;
	UINT64	read_latency_min;

	UINT64	num_write_commands;
	UINT64	num_write_sectors;
	UINT64	write_latency_sum;
	UINT64	write_latency_max;
	UINT64	write_latency_min;

	UINT64	num_fua_commands;
	UINT64	num_fua_sectors;
	UINT64	fua_latency_sum;
	UINT64	fua_latency_max;
	UINT64	fua_latency_min;

	UINT64	num_trim_commands;
	UINT64	num_trim_sectors;
	UINT64	trim_latency_sum;
	UINT64	trim_latency_max;
	UINT64	trim_latency_min;

	UINT64	num_fast_flush_commands;
	UINT64	fast_flush_latency_sum;
	UINT64	fast_flush_latency_max;
	UINT64	fast_flush_latency_min;

	UINT64	num_slow_flush_commands;
	UINT64	slow_flush_latency_sum;
	UINT64	slow_flush_latency_max;
	UINT64	slow_flush_latency_min;

	UINT64	link_idle_time;

} host_stat_t;

typedef struct
{
	UINT64	num_user_read_slices;
	UINT64	num_user_read_pages;
	UINT64	num_user_written_slices;
	UINT64	num_user_written_wls;
	UINT64	gc_cost_sum;
	UINT32	gc_cost_min;
	UINT32	gc_cost_max;
	UINT64	num_gc_read_pages;
	UINT64	num_gc_written_slices;
	UINT64	num_gc_written_wls;
	UINT32	num_victims;
	UINT32	num_reclaimed_blks;
	UINT32	num_erased_blocks;
	UINT32	ftl_q_high;
	UINT32	flash_q_high;
	UINT32	flash_table_high;

} ftl_stat_t;

typedef struct
{
	UINT32	read_buf_high;
	UINT32	write_buf_high;
	UINT32	gc_buf_high;

} buf_stat_t;

typedef struct
{
	UINT64	begin_time;

	UINT64	read_time_sum;
	UINT64	write_time_sum;
	UINT64	erase_time_sum;

	UINT64	read_stall_sum;
	UINT64	write_stall_sum;
	UINT64	erase_stall_sum;

	UINT32	num_read_commands;
	UINT32	num_write_commands;
	UINT32	num_erase_commands;
	UINT32	unused;

	UINT64	idle_time[NUM_CHANNELS];

	UINT64	write_count[NUM_DIES];
	UINT64	read_count[NUM_DIES];
	UINT64	erase_count[NUM_DIES];
	UINT64	io_time[NUM_DIES];
	UINT64	cell_time[NUM_DIES];

} nand_stat_t;

extern host_stat_t g_host_stat;
extern ftl_stat_t g_ftl_stat;
extern buf_stat_t g_buf_stat;
extern nand_stat_t g_nand_stat;

#if VERBOSE_HOST_STATISTICS
#define HOST_STAT(...)		__VA_ARGS__
#else
#define HOST_STAT(...)
#endif

#if VERBOSE_FTL_STATISTICS
#define FTL_STAT(...)		__VA_ARGS__
#else
#define FTL_STAT(...)
#endif

#if VERBOSE_BUF_STATISTICS
#define BUF_STAT(...)		__VA_ARGS__
#else
#define BUF_STAT(...)
#endif

#if VERBOSE_NAND_STATISTICS
#define NAND_STAT(...)		__VA_ARGS__
#else
#define NAND_STAT(...)
#endif

void print_host_statistics(host_stat_t* stat);
void print_ftl_statistics(void);
void print_buf_statistics(void);
void print_nand_statistics(void);

void reset_host_statistics(void);
void reset_ftl_statistics(void);
INLINE void reset_buf_statistics(void) { STOSD(&g_buf_stat, 0, sizeof(buf_stat_t)); }
void reset_nand_statistics(void);


char* format_time(UINT64 ns, char* buf);
char* format_size(UINT64 num_bytes);
char* format_decimal_size(UINT64 num_bytes);

void print_banner(void);
void print_sim_progress(void);

#if VEBOSE_HIL_PROTOCOL
#define PRINT_SATA_HIL		printf
#else
#define PRINT_SATA_HIL(...)
#endif

#if VEBOSE_HOST_PROTOCOL
#define PRINT_SATA_HOST		printf
#else
#define PRINT_SATA_HOST(...)
#endif

#if VERBOSE_FTL_CMD
#define PRINT_FTL_CMD		printf
#else
#define PRINT_FTL_CMD(...)
#endif


#endif	// VERBOSE_H

