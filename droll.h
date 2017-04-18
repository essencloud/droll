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


#ifndef DROLL_H
#define DROLL_H

#pragma warning(push)
#pragma warning(disable : 4127 4820 4255 4668 4710)
#include <windows.h>
#include <stdio.h>
#include <process.h>
#include <time.h>
#include <conio.h>
#include <setjmp.h>
#include <random>

#pragma warning(pop)
#pragma warning(disable : 4302 4514 4061 4711 4324 4057 4201 4100 4245 4996 4710 4311 4701 4189; error : 4668)

using namespace std;

#include "common.h"
#include "feature.h"
#include "debug.h"
#include "mutex.h"
#include "nand.h"
#include "simbase.h"
#include "verbose.h"
#include "sata.h"
#include "memory.h"
#include "ftl.h"
#include "fil.h"
#include "mu.h"
#include "sim_nand.h"
#include "insight.h"

#endif	// DROLL_H
