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

#if OPTION_INSIGHT

// 생성된 .alb 파일은 Agilent LPA software에서 import할 수 있는 Module Binary File format 이다.
// Agilent Logic Analyzer 장비를 구매하지 않은 경우에는 별도의 유료 라이센스가 필요한 듯.


insight_t g_insight;

typedef struct
{
	FILE*	file;
	UINT32	num_records;
	UINT32	num_records_limit;

} insight_context_t;

static insight_context_t g_insight_context;


void insight_begin(UINT32 limit_megabyte)
{
	insight_context_t* ic = &g_insight_context;

	CHECK(ic->file == NULL);

	FILE* file = fopen("insight.alb", "wb");
	CHECK(file != NULL);
	ic->file = file;

	fprintf(file, "AGILENT_BINARY_DATA\n");
	fprintf(file, "HEADER_BEGIN\n");
	fprintf(file, "BYTE_ORDER=LITTLE_ENDIAN\n");
	fprintf(file, "TIME_SOURCE COLUMN=\"sim_time\"\n");

	fprintf(file, "COLUMN \"sim_time\" TIME NBYTES=8 UNSIGNED_INTEGER EXPONENT=-9 ABSOLUTE\n");

	// 아래 각 항목의 순서와 크기(NBYTES)는 insight_t 구조체의 정의와 완전히 일치해야 한다.

    fprintf(file, "COLUMN \"NCQ cmd\"       VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=8\n");
    fprintf(file, "COLUMN \"non-NCQ cmd\"   VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=8\n");
    fprintf(file, "COLUMN \"read\"          VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=8\n");
    fprintf(file, "COLUMN \"write\"         VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=8\n");
    fprintf(file, "COLUMN \"SDB\"           VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=8\n");
    fprintf(file, "COLUMN \"cmd idle\"      VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=1\n");
    fprintf(file, "COLUMN \"link idle\"     VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=1\n");
    fprintf(file, "COLUMN \"collision\"     VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=1\n");

    for (UINT32 i = 0; i < NCQ_SIZE; i++)
    {
        fprintf(file, "COLUMN \"tag%02u\"   VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=8\n", i);
    }

    for (UINT32 i = 0; i < NUM_DIES; i++)
    {
		fprintf(file, "COLUMN \"die_%02u\"   VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=8\n", i);
    }

    for (UINT32 i = 0; i < NUM_CHANNELS; i++)
    {
	    fprintf(file, "COLUMN \"ch_%u_user\"       VALUE NBYTES=1 UNSIGNED_INTEGER WIDTH_BITS=8\n", i);
    }

    for (UINT32 i = 0; i < NUM_DIES; i++)
    {
	    fprintf(file, "COLUMN \"die_%02u_q\"   VALUE NBYTES=2 UNSIGNED_INTEGER WIDTH_BITS=16\n", i);
    }

    fprintf(file, "COLUMN \"NCQ\"           VALUE NBYTES=2 UNSIGNED_INTEGER WIDTH_BITS=16\n");
    fprintf(file, "COLUMN \"FTL cmd q\"     VALUE NBYTES=2 UNSIGNED_INTEGER WIDTH_BITS=16\n");
    fprintf(file, "COLUMN \"write q\"       VALUE NBYTES=2 UNSIGNED_INTEGER WIDTH_BITS=16\n");
    fprintf(file, "COLUMN \"read buf\"      VALUE NBYTES=2 UNSIGNED_INTEGER WIDTH_BITS=16\n");
    fprintf(file, "COLUMN \"write buf\"     VALUE NBYTES=2 UNSIGNED_INTEGER WIDTH_BITS=16\n");

	fprintf(file, "HEADER_END\n");

	ic->num_records = 0;
	ic->num_records_limit = (UINT32) (limit_megabyte * 1048576ULL / (sizeof(UINT64) + sizeof(insight_t)));
}

void insight_add_record(void)
{
	insight_context_t* ic = &g_insight_context;

	if (ic->file != NULL)
	{
		FILE* file = ic->file;

		fwrite(&g_sim_context.current_time, sizeof(UINT64), 1, file) != 0;

		fwrite(&g_insight, sizeof(g_insight), 1, file) != 0;

		ic->num_records++;

		if (ic->num_records == ic->num_records_limit)
		{
			// 스크립트에서 END_INSIGHT 를 만나기 전에 파일 크기(헤더 제외)가 제한치를 넘으면 더이상 record 생성을 하지 않는다.

			insight_end();
		}
	}
}

void insight_end(void)
{
	insight_context_t* ic = &g_insight_context;

	if (ic->file != NULL)
	{
		fclose(ic->file);
		ic->file = NULL;
	}
}

#endif	// OPTION_INSIGHT

