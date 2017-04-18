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

host_stat_t g_host_stat;
ftl_stat_t g_ftl_stat;
buf_stat_t g_buf_stat;
nand_stat_t g_nand_stat;

char* format_size(UINT64 num_bytes)
{
	static char string[128];

	if (num_bytes == 0)
	{
		sprintf(string, "0");
	}
	else if (num_bytes < 1024)
	{
		sprintf(string, "%llu Byte", num_bytes);
	}
	else if (num_bytes < 1048576)
	{
		sprintf(string, "%.1f KiB", num_bytes / 1024.0);
	}
	else if (num_bytes < 1073741824)
	{
		sprintf(string, "%.1f MiB", num_bytes / 1048576.0);
	}
	else
	{
		sprintf(string, "%.1f GiB", num_bytes / 1073741824.0);
	}

	return string;
}

char* format_decimal_size(UINT64 num_bytes)
{
	static char string[128];

	if (num_bytes < 1000)
	{
		sprintf(string, "%llu Byte", num_bytes);
	}
	else if (num_bytes < 1000000)
	{
		sprintf(string, "%.1f KB", num_bytes / 1000.0);
	}
	else if (num_bytes < 1000000000)
	{
		sprintf(string, "%.1f MB", num_bytes / 1000000.0);
	}
	else
	{
		sprintf(string, "%.1f GB", num_bytes / 1000000000.0);
	}

	return string;
}

char* format_time(UINT64 ns, char* buf)
{
	static char internal_buf[128];

	char* p = (buf == NULL) ? internal_buf : buf;

	if (ns == 0 || ns == FF64)
	{
		sprintf(p, "0");
	}
	else if (ns < 1000)
	{
		sprintf(p, "%lluns", ns);
	}
	else if (ns < 1000000)
	{
		sprintf(p, "%.2fus", ns / 1000.0);
	}
	else if (ns < 1000000000ULL)
	{
		sprintf(p, "%.2fms", ns / 1000000.0);
	}
	else if (ns < 60000000000ULL)
	{
		sprintf(p, "%.1fsec", ns / 1000000000.0);
	}
	else if (ns < 3600000000000ULL)
	{
		sprintf(p, "%.1fmin", ns / 60000000000.0);
	}
	else
	{
		sprintf(p, "%.1fhr", ns / 3600000000000.0);
	}

	return p;
}

void print_banner(void)
{
	HANDLE h1 = GetStdHandle(STD_OUTPUT_HANDLE);
	COORD bufferSize = {120, 300};
	SetConsoleScreenBufferSize(h1, bufferSize);

	HWND h2 = GetConsoleWindow();
	RECT r;
	GetWindowRect(h2, &r);
	MoveWindow(h2, r.left, r.top, 2000, 500, TRUE);

	printf(__FILE__);

	printf("\n\nNAND total = %s\n", format_size((UINT64) NUM_PSLICES * BYTES_PER_SLICE));
	printf("logical sectors = %u = %s = %s\n\n", NUM_LSECTORS, format_size((UINT64) NUM_LSECTORS * BYTES_PER_SECTOR), format_decimal_size((UINT64) NUM_LSECTORS * BYTES_PER_SECTOR));

	printf("DRAM size   = %u\n", DRAM_SIZE);
	printf("DRAM demand = %zu\n\n", sizeof(dram_t));
	printf("write buf = %s\n", format_size(sizeof(g_dram.write_buf)));
	printf("read buf  = %s\n", format_size(sizeof(g_dram.read_buf)));
	printf("gc buf    = %s\n", format_size(sizeof(g_dram.gc_buf)));

	printf("\nOPTION_VERIFY_DATA = %u, OPTION_ASSERT = %u%s\n", OPTION_VERIFY_DATA, OPTION_ASSERT, RANDOM_SEED ? ", replay mode" : "");
}

void print_sim_progress(void)
{
	printf("\nsession=%u, current_time=%llu, ", g_sim_context.session, g_sim_context.current_time);

	time_t now;
	time(&now);
	UINT64 real_time = (UINT64) (now - g_sim_context.sim_begin_time) * 1000000000;
	printf("%s (real time) ", format_time(real_time, NULL));
	printf("%s (virtual time) ", format_time(g_sim_context.current_time, NULL));
	printf("simulation speed = %.1f\n", g_sim_context.current_time / (double) real_time);
}

void print_host_statistics(host_stat_t* stat)
{
	// 통계의 각 수치가 증가하는 시점이 조금씩 다르기 때문에 sudden power loss가 발생했을 경우에는 약간의 오차가 있음

	UINT64 nanosec = g_sim_context.current_time - stat->begin_time;

	if (nanosec == 0)
		return;

	char b1[100], b2[100], b3[100], b4[100], b5[100];

	UINT64 read_cmds = stat->num_read_commands == 0 ? 1 : stat->num_read_commands;
	UINT64 write_cmds = stat->num_write_commands == 0 ? 1 : stat->num_write_commands;
	UINT64 trim_cmds = stat->num_trim_commands == 0 ? 1 : stat->num_trim_commands;
	UINT64 fast_flush_cmds = stat->num_fast_flush_commands == 0 ? 1 : stat->num_fast_flush_commands;
	UINT64 slow_flush_cmds = stat->num_slow_flush_commands == 0 ? 1 : stat->num_slow_flush_commands;

	printf("\n\n%-15s%11s%11s%11s%11s%11s\n", "", "READ", "WRITE", "TRIM", "F_FLUSH", "S_FLUSH");

	printf("%-15s%11llu%11llu%11llu%11llu%11llu\n", "command count",
		stat->num_read_commands, stat->num_write_commands, stat->num_trim_commands, stat->num_fast_flush_commands, stat->num_slow_flush_commands);

	printf("%-15s", "total size");
	printf("%11s", format_size(stat->num_read_sectors * BYTES_PER_SECTOR));
	printf("%11s", format_size(stat->num_write_sectors * BYTES_PER_SECTOR));
	printf("%11s", format_size(stat->num_trim_sectors * BYTES_PER_SECTOR));
	printf("\n");

	printf("%-15s", "KiB/cmd");
	printf("%11.1f", stat->num_read_sectors * BYTES_PER_SECTOR / 1024.0 / read_cmds);
	printf("%11.1f", stat->num_write_sectors * BYTES_PER_SECTOR / 1024.0 / write_cmds);
	printf("%11.1f", stat->num_trim_sectors * BYTES_PER_SECTOR / 1024.0 / trim_cmds);
	printf("\n");

	printf("%-15s%11s%11s%11s%11s%11s\n", "latency min",
		format_time(stat->read_latency_min, b1), format_time(stat->write_latency_min, b2),
		format_time(stat->trim_latency_min, b3), format_time(stat->fast_flush_latency_min, b4), format_time(stat->slow_flush_latency_min, b5));

	printf("%-15s%11s%11s%11s%11s%11s\n", "latency avg",
		format_time(stat->read_latency_sum / read_cmds, b1),
		format_time(stat->write_latency_sum / write_cmds, b2),
		format_time(stat->trim_latency_sum / trim_cmds, b3),
		format_time(stat->fast_flush_latency_sum / fast_flush_cmds, b4),
		format_time(stat->slow_flush_latency_sum / slow_flush_cmds, b5));

	printf("%-15s%11s%11s%11s%11s%11s\n", "latency max",
		format_time(stat->read_latency_max, b1), format_time(stat->write_latency_max, b2),
		format_time(stat->trim_latency_max, b3), format_time(stat->fast_flush_latency_max, b4), format_time(stat->slow_flush_latency_max, b5));

	printf("SATA link idle time = %.2f%%\n", stat->link_idle_time * 100.0 / nanosec);

	UINT64 num_xfer_bytes = (stat->num_read_sectors + stat->num_write_sectors) * BYTES_PER_SECTOR;
	UINT64 num_xfer_commands = stat->num_read_commands + stat->num_write_commands;

	if (num_xfer_commands != 0)
	{
		double seconds = nanosec / 1000000000.0;

		printf("%llu data transfer commands total, %s, %.0f IOPS, ",
			num_xfer_commands,
			format_size(num_xfer_bytes),
			num_xfer_commands / seconds);

		printf("%s/s, ", format_size((UINT64) (num_xfer_bytes / seconds)));
		printf("%s/s\n", format_decimal_size((UINT64)(num_xfer_bytes / seconds)));
	}

	printf("\n");
}

void print_ftl_statistics(void)
{
	ftl_stat_t* stat = &g_ftl_stat;

	printf("FTL statistics:\n");

	printf("%I64u slices read by host\n", stat->num_user_read_slices);

	if (stat->num_user_read_pages != 0)
	{
		printf("%I64u pages read by host, efficiency=%.1f%%\n", stat->num_user_read_pages, stat->num_user_read_slices * 100.0 / stat->num_user_read_pages / SLICES_PER_BIG_PAGE);
	}

	printf("%I64u slices written by host\n", stat->num_user_written_slices);

	if (stat->num_user_written_wls != 0)
	{
		printf("%I64u wordlines written by host, efficiency=%.1f%%\n", stat->num_user_written_wls, stat->num_user_written_slices * 100.0 / stat->num_user_written_wls / SLICES_PER_BIG_WL);
	}

	printf("%u blocks erased\n", stat->num_erased_blocks);

	if (stat->num_user_written_slices != 0)
	{
		printf("Write Amplification = %.2f\n", stat->num_erased_blocks * SLICES_PER_BIG_BLK / (double) stat->num_user_written_slices);
	}

	printf("%u blocks chosen for GC\n", stat->num_victims);
	printf("%u blocks reclaimed\n", stat->num_reclaimed_blks);
	printf("%I64u slices read by GC\n", stat->gc_cost_sum);

	if (stat->num_gc_read_pages != 0)
	{
		printf("%I64u pages read by GC, efficiency=%.1f%%\n", stat->num_gc_read_pages, stat->gc_cost_sum * 100.0 / stat->num_gc_read_pages / SLICES_PER_BIG_PAGE);
	}

	printf("%I64u slices written by GC\n", stat->num_gc_written_slices);

	if (stat->num_gc_written_wls != 0)
	{
		printf("%I64u pages written by GC, efficiency=%.1f%%\n", stat->num_gc_written_wls, stat->num_gc_written_slices * 100.0 / stat->num_gc_written_wls / SLICES_PER_BIG_WL);
	}

	if (stat->num_victims != 0)
	{
		printf("GC cost low  = %u = %.2f%% of block size\n", stat->gc_cost_min, stat->gc_cost_min * 100.0 / SLICES_PER_BIG_BLK);
		printf("GC cost high = %u = %.2f%% of block size\n", stat->gc_cost_max, stat->gc_cost_max * 100.0 / SLICES_PER_BIG_BLK);
		printf("GC cost avg  = %llu = %.2f%% of block size\n", stat->gc_cost_sum / stat->num_victims, stat->gc_cost_sum / stat->num_victims * 100.0 / SLICES_PER_BIG_BLK);
	}

	printf("FTL command queue length high = %u = %.1f%%\n", stat->ftl_q_high, stat->ftl_q_high * 100.0 / FTL_CMD_Q_SIZE);
	printf("flash command queue length high = %u = %.1f%%\n", stat->flash_q_high, stat->flash_q_high * 100.0 / FLASH_QUEUE_SIZE);
	printf("flash command table high = %u = %.1f%%\n", stat->flash_table_high, stat->flash_table_high * 100.0 / FLASH_CMD_TABLE_SIZE);
}

void print_buf_statistics(void)
{
	buf_stat_t* stat = &g_buf_stat;

	printf("write buffer level high = %u = %.1f%%\n", stat->write_buf_high, stat->write_buf_high * 100.0 / NUM_WBUF_SLOTS);
	printf("read buffer level high = %u = %.1f%%\n", stat->read_buf_high, stat->read_buf_high* 100.0 / NUM_RBUF_SLOTS);
	printf("GC buffer level high = %u = %.1f%%\n", stat->gc_buf_high, stat->gc_buf_high * 100.0 / NUM_GCBUF_SLOTS);
	printf("\n");
}

void print_nand_statistics(void)
{
	nand_stat_t* stat = &g_nand_stat;

	printf("NAND statistics:\n");

	if (stat->num_read_commands != 0)
	{
		printf("avg. %s per read command, %.2f%% stall\n",
			format_time(stat->read_time_sum / stat->num_read_commands, NULL), stat->read_stall_sum * 100.0 / stat->read_time_sum);
	}

	if (stat->num_write_commands != 0)
	{
		printf("avg. %s per write command, %.2f%% stall\n",
			format_time(stat->write_time_sum / stat->num_write_commands, NULL), stat->write_stall_sum * 100.0 / stat->write_time_sum);
	}

	if (stat->num_erase_commands != 0)
	{
		printf("avg. %s per erase command, %.2f%% stall\n",
			format_time(stat->erase_time_sum / stat->num_erase_commands, NULL), stat->erase_stall_sum * 100.0 / stat->erase_time_sum);
	}

	UINT64 total_time = g_sim_context.current_time - stat->begin_time;

	for (UINT32 i = 0; i < NUM_CHANNELS; i++)
	{
		printf("channel %u idle time = %.3f%%\n", i, stat->idle_time[i] * 100.0 / total_time);
	}

	printf("\n%4s%10s%10s%10s%10s%10s%10s\n", "DIE", "CELL", "IO", "IDLE", "READ", "WRITE", "ERASE");

	for (UINT32 die = 0; die < NUM_DIES; die++)
	{
		UINT64 active_time = stat->cell_time[die] + stat->io_time[die];

		printf("%4u", die);
		printf("%9.2f%%", stat->cell_time[die] * 100.0 / total_time);
		printf("%9.2f%%", stat->io_time[die] * 100.0 / total_time);
		printf("%9.2f%%", (total_time - active_time) * 100.0 / total_time);

		printf("%10llu%10llu%10llu\n", stat->read_count[die], stat->write_count[die], stat->erase_count[die]);
	}

	printf("\n");
}

void reset_host_statistics(void)
{
	STOSQ(&g_host_stat, 0, sizeof(g_host_stat));

	g_host_stat.read_latency_min = FF64;
	g_host_stat.write_latency_min = FF64;
	g_host_stat.fua_latency_min = FF64;
	g_host_stat.trim_latency_min = FF64;
	g_host_stat.fast_flush_latency_min = FF64;
	g_host_stat.slow_flush_latency_min = FF64;

	g_host_stat.begin_time = g_sim_context.current_time;
}

void reset_ftl_statistics(void)
{
	mu_fill(&g_ftl_stat, 0, sizeof(g_ftl_stat));

	g_ftl_stat.gc_cost_min = FF32;
}

void reset_nand_statistics(void)
{
	STOSQ(&g_nand_stat, 0, sizeof(g_nand_stat));

	g_nand_stat.begin_time = g_sim_context.current_time;
}

