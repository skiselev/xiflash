# Makefile for xiflash
#
# Copyright (C) 2012 Sergey Kiselev.
# Provided for hobbyist use on the Xi 8088 board.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

CC = open-watcom.owcc
CFLAGS = -I/snap/open-watcom/current/h -bdos -mcmodel=c -Os -s -march=i86 -W -Wall -Wextra
RM = rm

all:	xiflash.exe

xiflash.exe:	xiflash.c
	$(CC) $(CFLAGS) -o $@ xiflash.c

clean:
	$(RM) *.exe
	$(RM) *.o
