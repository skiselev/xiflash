/*************************************************************************
 * xiflash.c - BIOS flash ROM utility for Xi 8088 and Micro 8088 computers
 *
 *
 * Copyright (C) 2012, 2021 Sergey Kiselev.
 * 64 KiB image support ideas borrowed from uflash by Aitor Gomez (spark2k06)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <conio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <malloc.h>
#include <dos.h>

#define VERSION		"0.2"
#define CHUNK_SIZE	32768

#define MODE_READ	1
#define MODE_PROG	(1 << 1)
#define MODE_VERIFY	(1 << 2)
#define MODE_CHECKSUM	(1 << 3)
#define NUM_DEVICES 5

/* number of calibration loops to run */
#define CALIBRATION_LOOPS 50000
#define WRITE_TIMEOUT 10000

struct{
	unsigned char vendor_id;
	unsigned char device_id;
	char *vendor_name;
	char *device_name;
	unsigned int page_size;
	unsigned int need_erase;	/* 1 = needs erase before write */
	unsigned int page_write;	/* 0 = byte write operation is required (page write not supported) */
} eeproms[NUM_DEVICES] = {
	{0x01, 0x20, "AMD",		"Am29F010",			16384,	1, 0},
	{0x1F, 0xD5, "Atmel",		"AT29C010",			128,	0, 1},
	{0xDA, 0xC1, "Winbond",		"W29EE011",			128,	0, 1},
	{0xBF, 0x07, "SST/Greenliant",	"SST29EE010/GLS29EE010",	128,	0, 1},
	{0xBF, 0xB5, "SST/Microchip",	"SST39SF010",			4096,	1, 0}
};

char *exec_name;

unsigned int cmd_addr1 = 0x5555, cmd_addr2 = 0x2AAA;
unsigned int loops_per_1ms = 1;		/* calibrated later by calibrate_delay() */

/* FIXME: should disable NMI too */
void interrupts_disable();
#pragma aux interrupts_disable = "cli";

void interrupts_enable();
#pragma aux interrupts_enable = "sti";

void usage()
{
	printf("Usage: %s [-r|-p|-v|-c] [-i <input_file>] [-o <output_file>] [-a <address>] [-s <size>]\n\n", exec_name);
	printf("Options:\n");
	printf("   -r   - Read mode. Save current flash ROM content into <output_file>.\n");
	printf("   -p   - Program mode. Program flash ROM with <input_file> data.\n");
	printf("   -v   - Verify mode. Compare current flash ROM content with <input_file>.\n");
	printf("   -c   - Print a checksum. If <input_file> specified, its checksum will\n");
	printf("          be printed. Otherwise the current flash ROM checksum is printed.\n");
	printf("   -i   - Specifies input file for -p, -v, and, -c options.\n");
	printf("   -o   - Specifies output file for -r option.\n");
	printf("   -a   - Segment address of flash ROM area to work on in hexadecimal format.\n");
	printf("          Must be in E000-F000 range. The default is F800 (BIOS address).\n");
	printf("          When programming 64 KiB images, the default is F000.\n");
	printf("   -s   - Specifies ROM size for -r and -c options.\n");
	printf("	  The default is %u.\n\n", CHUNK_SIZE);
	exit(1);
}

void error(char *message) {
	printf("ERROR: %s\n\n", message);
	usage();
}

void delay(unsigned int delay)
{
	unsigned long loops = loops_per_1ms;
	while (loops--) {
		__asm {
			push	cx
			mov	cx,delay
			delay_loop:
			nop
			loop	delay_loop
			pop	cx
		}
	}
}

unsigned char __far *get_video_address()
{
	unsigned char video_mode;
	unsigned char num_columns;
	unsigned char video_page;
	unsigned char column;
	unsigned char row;
	unsigned char __far *video_address = 0;
	union REGS r;
	r.h.ah = 0x0F;
	int86(0x10, &r, &r);	/* INT 0x10 function 0x0F - Get current video mode */
	video_mode = r.h.al;
	num_columns = r.h.ah;
	video_page = r.h.bh;
	r.h.ah = 0x03;
	int86(0x10, &r, &r);	/* INT 0x10 function 0x03 - Get cursor position and size */
	column = r.h.dl;
	row = r.h.dh;
	if (video_mode <= 3) { /* CGA-compatible text modes */
		video_address = 0xB800:>0;	/* Video buffer start address for the color text modes */
		if (num_columns == 40) {
			video_address += 2048 * video_page; /* add page offset for 40 column modes */
		} else {
			video_address += 4096 * video_page; /* add page offset for 80 column modes */
		}
		video_address += (unsigned int) (num_columns * row + column) * 2;
	} else if (video_mode == 7) { /* MDA-compatible text mode */
		video_address = 0xB000:>0;	/* Video buffer start for the monochrome text mode */
		video_address += (unsigned int) (num_columns * row + column) * 2;
	}
	return video_address;
}

void video_write_char(unsigned char __far *video_address, unsigned char ch, unsigned char attr)
{
	if (video_address != 0) {
		video_address[0] = ch;
		video_address[1] = attr;
	}
}

void rom_verify(unsigned char __far *rom_addr, unsigned char __far *buf, unsigned long rom_size) {
	unsigned long i, diff = 0;

	for (i = 0; i < rom_size; i++) {
		if (rom_addr[i] != buf[i]) {
			printf("WARNING: Difference found at 0x%05X: ROM = 0x%02X; file 0x%02X\n", i, rom_addr[i], buf[i]);
			diff++;
		}	
	}

	if (diff > 0) {
		printf("WARNING: %d differences found\n", diff);
	} else {
		printf("No differences found\n");
	}
}

/* rom_read - DUMP ROM content to a file */
void rom_read(unsigned char __far *rom_addr, char *out_file, unsigned long rom_size) {
	FILE *fp_out;
	size_t count, write_size;
	unsigned long bytes_to_write = rom_size;

	printf("Saving ROM content to %s, size %lu bytes.\n",
		out_file, rom_size);

	if ((fp_out = fopen(out_file, "wb")) == NULL) {
		printf("ERROR: Failed to create %s: %s.\n",
		       out_file, strerror(errno));
		exit(2);
	}
	
	while (bytes_to_write > 0) {
		if (bytes_to_write > CHUNK_SIZE) {
			write_size = CHUNK_SIZE;
		} else {
			write_size = bytes_to_write;
		}
		if ((count = fwrite(rom_addr, 1, write_size, fp_out)) != write_size) {
			printf("ERROR: Short write while writing %s. Wrote %u bytes, expected to write %u bytes.\n",
				out_file, count, rom_size);
			exit(3);
		}
		rom_addr += write_size;
		bytes_to_write -= write_size;
	}
	fclose(fp_out);
}

unsigned char __far *load_file(char *in_file, unsigned long *rom_size) {
	FILE *fp_in;
	size_t count, read_size;
	unsigned long bytes_to_read;
	unsigned char __far *buf, *read_pointer;
	struct stat st;

	if (stat(in_file, &st) == -1) {
		printf("ERROR: Failed to stat %s: %s.\n",
			in_file, strerror(errno));
		exit(4);
	}

	*rom_size = st.st_size;
	if (*rom_size == 0) {
		printf("ERROR: File %s is empty.\n", in_file);
		exit(4);
	}

	printf("Loading flash ROM image from %s, size %lu bytes.\n",
		in_file, *rom_size);

	if ((fp_in = fopen(in_file, "rb")) == NULL) {
		printf("ERROR: Failed to open %s for reading: %s.\n",
		       in_file, strerror(errno));
		exit(4);
	}
	
	if ((buf = (unsigned char __far *) halloc (*rom_size, 1)) == NULL) {
		printf("ERROR: Failed to allocate %lu bytes for input buffer.\n",
		       *rom_size);
		exit(5);
	}

	bytes_to_read = *rom_size;
	read_pointer = buf;
	while (bytes_to_read > 0) {
		if (bytes_to_read > CHUNK_SIZE) {
			read_size = CHUNK_SIZE;
		} else {
			read_size = bytes_to_read;
		}
		if ((count = fread(read_pointer, 1, read_size, fp_in)) != read_size) {
			printf("ERROR: Short read while reading %s. Read %u bytes, expected to read %u bytes.\n",
			       in_file, count, read_size);
			exit(6);
		}
		read_pointer += read_size;
		bytes_to_read -= read_size;
	}
	fclose(fp_in);
	return buf;
}
	               
unsigned int checksum (unsigned char __far *buf_addr, unsigned long rom_size)
{
	unsigned int checksum = 0;
	unsigned long i;
	for (i = 0; i < rom_size; i++)
		checksum += buf_addr[i];
	return checksum;
}

/* rom_identify - Identify flash ROM type, return index in eeprom table and start segment */
int rom_identify(unsigned char __far *rom_start)
{
	int index = -1;
	volatile unsigned char __far *rom_st = rom_start;
	unsigned char byte0, byte1, vendor_id, device_id;

	byte0 = rom_st[0];
	byte1 = rom_st[1];

	cmd_addr1 = 0x5555;
	cmd_addr2 = 0x2AAA;

	/* Enter software ID mode */
	interrupts_disable();
	rom_st[cmd_addr1] = 0xAA;
	rom_st[cmd_addr2] = 0x55;
	rom_st[cmd_addr1] = 0x90;
	delay(1);

	vendor_id = rom_st[0];
	device_id = rom_st[1];

	if (vendor_id == byte0 && device_id == byte1) {
		/* Try alternate software ID mode */
		rom_st[cmd_addr1] = 0xAA;
		rom_st[cmd_addr2] = 0x55;
		rom_st[cmd_addr1] = 0x80;
		rom_st[cmd_addr1] = 0xAA;
		rom_st[cmd_addr2] = 0x55;
		rom_st[cmd_addr1] = 0x60;
		delay(1);

		vendor_id = rom_st[0];
		device_id = rom_st[1];
	}

	if (vendor_id == byte0 && device_id == byte1) {
		/* Try 0x555 and 0x2AA addresses for commands */
		cmd_addr1 = 0x555;
		cmd_addr2 = 0x2AA;

		rom_st[cmd_addr1] = 0xAA;
		rom_st[cmd_addr2] = 0x55;
		rom_st[cmd_addr1] = 0x80;
		rom_st[cmd_addr1] = 0xAA;
		rom_st[cmd_addr2] = 0x55;
		rom_st[cmd_addr1] = 0x60;
		delay(1);

		vendor_id = rom_st[0];
		device_id = rom_st[1];
	}

	/* Exit software ID mode */
	rom_st[cmd_addr1] = 0xAA;
	rom_st[cmd_addr2] = 0x55;
	rom_st[cmd_addr1] = 0xF0;
	delay(1);

	interrupts_enable();

	if (vendor_id == byte0 && device_id == byte1) {
		index = -1;
	} else {
		for (index = 0; index < NUM_DEVICES; index++)
			if (eeproms[index].vendor_id == vendor_id && eeproms[index].device_id == device_id)
				break;
		if (NUM_DEVICES == index) {
			printf("ERROR: Unsupported flash ROM type. Vendor ID = 0x%02X; Device ID = 0x%02X\n",
			       vendor_id, device_id);
			index = -1;
		}
	}

	return index;
}

int rom_erase_block(unsigned char __far *rom_start, unsigned char __far *rom_addr)
{
	unsigned int timeout;
	volatile unsigned char __far *rom_st = rom_start;
	volatile unsigned char __far *rom_ad = rom_addr;

	/* Enter page erase mode */
	rom_st[cmd_addr1] = 0xAA;
	rom_st[cmd_addr2] = 0x55;
	rom_st[cmd_addr1] = 0x80;
	rom_st[cmd_addr1] = 0xAA;
	rom_st[cmd_addr2] = 0x55;
	rom_ad[0] = 0x30;

	/* poll EPROM - wait for erase operation to complete */
	for (timeout = 0; timeout < 1000; timeout++) {
		if (rom_ad[0] == 0xFF)
			return 0;
		delay(1);
	}

	return 1;
}

int rom_program_block(unsigned char __far *rom_start, unsigned char __far *rom_addr, unsigned char __far *buf, unsigned int block_size, unsigned char page_write)
{
	unsigned long offset;
	unsigned int timeout;
	volatile unsigned char __far *rom_st = rom_start;
	volatile unsigned char __far *rom_ad = rom_addr;

	if (page_write) {
		/* Enter page write mode */
		rom_st[cmd_addr1] = 0xAA;
		rom_st[cmd_addr2] = 0x55;
		rom_st[cmd_addr1] = 0xA0;

		/* write page */
		for (offset = 0; offset < block_size; offset++)
			rom_ad[offset] = buf[offset];

		/* poll EPROM - wait for write operation to complete */
		for (timeout = 0; timeout < WRITE_TIMEOUT; timeout++) {
			if (rom_ad[block_size - 1] == buf[block_size - 1])
				return 0;
		}
	} else {
		for (offset = 0; offset < block_size; offset++) {
			/* Enter write mode */
			rom_st[cmd_addr1] = 0xAA;
			rom_st[cmd_addr2] = 0x55;
			rom_st[cmd_addr1] = 0xA0;

			/* write byte */
			rom_ad[offset] = buf[offset];

			/* verify byte */
			for (timeout = 0; timeout < WRITE_TIMEOUT; timeout++) {
				if (rom_ad[offset] == buf[offset])
					break;
			}
			if (timeout == WRITE_TIMEOUT)
				break;
		}
		if (offset == block_size)
			return 0;
	}

	return 1;
}

void rom_program(__segment rom_addr, unsigned char __far *buf, unsigned long rom_size)
{
	unsigned int eeprom_index;
	__segment rom_start;
	unsigned int page, page_paragraph, pages_per_column = 1;
	unsigned long num_pages;
	unsigned char __far *video_address;

	/* try 0xF000 first */
	rom_start = 0xF000;
	if ((eeprom_index = rom_identify(rom_start:>0)) == -1) {
		/* try 0xE000 */
		rom_start = 0xE000;
		if ((eeprom_index = rom_identify(rom_start:>0)) == -1) {
			error("Cannot detect flash ROM type.\nOn Sergey's XT Version 1.0 systems make sure that SW2.6 - SW2.7 are OFF.");
		}
	}

	printf("Detected flash ROM at 0x%04X, type: %s %s, page size: %u bytes.\n",
		rom_start, eeproms[eeprom_index].vendor_name, eeproms[eeprom_index].device_name,
		eeproms[eeprom_index].page_size);

	/* check that requested ROM segment is on the page boundary */
	page_paragraph = eeproms[eeprom_index].page_size / 16;
	if ((rom_addr / page_paragraph) * page_paragraph != rom_addr) {
		printf("ERROR: Specified ROM address (0x%04X) doesn't start on the page boundary.\n",
			rom_addr);
	}

	/* figure out number of pages to program */
	num_pages = rom_size / eeproms[eeprom_index].page_size;
	if (num_pages * eeproms[eeprom_index].page_size != rom_size) {
		printf("ERROR: ROM image size (%lu) is is not a multiply of the flash page size.\n",
			rom_size);
		exit(10);
	}

	printf("Programming the flash ROM with %lu bytes starting at address 0x%04X.\n", rom_size, rom_addr);
	printf("Please wait... Do not reboot the system...\n");
	video_address = get_video_address();
	if (num_pages > 40) {
		pages_per_column = num_pages / 32;
	}
	for (page = 0; page < num_pages; page++) {
		video_write_char(video_address + (page / pages_per_column) * 2, 0xB0, 0x07);
	}
	interrupts_disable();

	for (page = 0; page < num_pages; page++) {
		outp(0x80, page);
		if (eeproms[eeprom_index].need_erase) {
			video_write_char(video_address + (page / pages_per_column) * 2, 0xB1, 0x07);
			rom_erase_block(rom_start:>0, rom_addr:>(eeproms[eeprom_index].page_size * page));
		}
		video_write_char(video_address + (page / pages_per_column) * 2, 0xB2, 0x07);
		rom_program_block(rom_start:>0, rom_addr:>(eeproms[eeprom_index].page_size * page), buf + (eeproms[eeprom_index].page_size * page), eeproms[eeprom_index].page_size, eeproms[eeprom_index].page_write);
		video_write_char(video_address + (page / pages_per_column) * 2, 0xDB, 0x07);
	}

	interrupts_enable();
	printf("Flash ROM has been programmed successfully.\nPlease reboot the system.\n");
}

void calibrate_delay() {
	clock_t start_time, end_time;

	start_time = clock();
	delay(CALIBRATION_LOOPS);
	end_time = clock();
	loops_per_1ms = (long) 1000*CALIBRATION_LOOPS/CLOCKS_PER_SEC/(end_time - start_time);
}

int main(int argc, char *argv[])
{
 	int i;
	unsigned int mode = 0;
	__segment rom_seg = 0xF800;
	char *in_file = NULL, *out_file = NULL;
	unsigned char __far *buf;
	unsigned long rom_size = CHUNK_SIZE;

	exec_name = argv[0];

	printf("xiflash, Version %s. Copyright (C) 2012, 2021 Sergey Kiselev\n", VERSION);
	printf("Distributed under the terms of the GNU General Public License\n\n");

	if (1 == argc) usage ();

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-i")) {
			if (++i < argc) {
				in_file = argv[i];
			} else {
				error("Option -i requires an argument.");
			}
			continue;
		}
		if (!strcmp(argv[i], "-o")) {
			if (++i < argc) {
				out_file = argv[i];
			} else {
				error("Option -o requires an argument.");
			}
			continue;
		}
		if (!strcmp(argv[i], "-a")) {
			if (++i < argc) {
				sscanf(argv[i], "%x", &rom_seg);
				if (rom_seg < 0xE000)
					error("Invalid address specified (it has to be in E000-FFFF range).");
			} else {
				error("Option -a requires an argument.");
			}
			continue;
		}
		if (!strcmp(argv[i], "-s")) {
			if (++i < argc) {
				sscanf(argv[i], "%lu", &rom_size);
			} else {
				error("Option -s requires an argument.");
			}
			continue;
		}
		if (!strcmp(argv[i], "-r")) {
			mode |= MODE_READ;
			continue;
		}
		if (!strcmp(argv[i], "-p")) {
			mode |= MODE_PROG;
			continue;
		}
		if (!strcmp(argv[i], "-v")) {
			mode |= MODE_VERIFY;
			continue;
		}
		if (!strcmp(argv[i], "-c")) {
			mode |= MODE_CHECKSUM;
			continue;
		}
		error("Invalid command line argument.");
	}
	if (!mode)
		error("Nothing to do. Please specify one of the following options: -r, -w, -v, -c.");

	if ((mode & MODE_READ) && NULL == out_file)
		error("No output file specified for read mode.");

	if ((mode & MODE_PROG) && NULL == in_file)
		error("No input file specified for program mode.");

	if ((mode & MODE_VERIFY) && NULL == in_file)
		error("No input file specified for verify mode.");

	if (mode & MODE_READ)
		rom_read(rom_seg:>0, out_file, rom_size);
	
	if ((mode & MODE_PROG) || (mode & MODE_VERIFY) ||
	    ((mode & MODE_CHECKSUM) && in_file != NULL)) {
		buf = load_file(in_file, &rom_size);
		if (rom_size == 65536 && rom_seg == 0xF800) {
			rom_seg = 0xF000;	/* set default ROM segment to F0000 for 64 KiB images */
		}
		/* check if ROM size extends beyond 1 MiB */
		if ((rom_size + 15) / 16 - 1 + rom_seg < rom_seg) {
			error("ROM image extends beyond 1 MiB. Make sure that the correct image file is specified. Also check -a option's argument (if specified).");
		}
	}

	if (mode & MODE_CHECKSUM) {
		if (NULL == in_file)
			printf("Current ROM checksum at 0x%X is 0x%X\n", rom_seg,
			       checksum(rom_seg:>0, rom_size));
		else
			printf("The checksum of %s is 0x%X\n", in_file,
			       checksum(buf, rom_size));
	}

	if (mode & MODE_PROG) {
		calibrate_delay();
		rom_program(rom_seg, buf, rom_size);
	}

	if (mode & MODE_VERIFY) {
		rom_verify(rom_seg:>0, buf, rom_size);
	}

	return 0;
}

