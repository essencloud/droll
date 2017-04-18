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


#ifndef DEBUG_H
#define DEBUG_H

#define RANDOM_SEED			0			// 0이 아닌 값으로 설정하면 replay mode가 되어서 동일한 시나리오가 반복됨


extern __declspec(noinline) void sim_error(char* file, int line);

#define CHECK(X)							\
{											\
	if (!(X))								\
	{										\
		sim_error(__FILE__, __LINE__);		\
		__debugbreak();						\
	}										\
}

#if OPTION_ASSERT
#define ASSERT(X)	CHECK(X)
#else
#define ASSERT(X)	__assume(X)
#endif

#define BRK(X)		if (X) __debugbreak()


#define SANITY_CHECK(condition) typedef char SANITY_CHECK ## __LINE__[1 - 2 * !(condition)]

extern void sanity_check(void);

#endif	// DEBUG_H
