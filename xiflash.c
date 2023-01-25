/*************************************************************************
 * xiflash.c - BIOS flash ROM utility for Xi 8088 and Micro 8088 computers
 *
 *
 * Copyright (C) 2012 - 2023 Sergey Kiselev.
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
#include <malloc.h>
#include <dos.h>

#define VERSION			"0.5"
#define DEFAULT_ROM_SIZE	32768

#define MODE_READ		1
#define MODE_PROG		(1 << 1)
#define MODE_VERIFY		(1 << 2)
#define MODE_CHECKSUM		(1 << 3)


#define TICKS_PER_SEC 1193182					/* 8254 PIT ticks per second */
#define IDENTIFY_DELAY (TICKS_PER_SEC/100)			/* flash ID delay is 1/100 = 10 ms */
#define WRITE_DELAY (TICKS_PER_SEC/20000)			/* write/erase delay is 1/20000 = 50 us */
#define ERASE_TIMEOUT (TICKS_PER_SEC/10/WRITE_DELAY)		/* page erase timeout is 1/10 = 100 ms */
#define PAGE_WRITE_TIMEOUT (TICKS_PER_SEC/10/WRITE_DELAY)	/* page write timeout is 1/10 = 100 ms */
#define BYTE_WRITE_TIMEOUT (TICKS_PER_SEC/100/WRITE_DELAY)	/* byte write timeout is 1/100 = 10 ms */

#define NUM_DEVICES 5

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

void interrupts_disable()
{
	__asm {
		cli
		push	ax
		mov	al,0
		out	0xA0,al
		pop	ax
	}
}

void interrupts_enable()
{
	__asm {
		push	ax
		mov	al,0x80
		out	0xA0,al
		pop	ax
		sti
	}
}

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
	printf("          Must be in C000-FFFF range. The default is FA00 (Micro 8088 BIOS\n");
	printf("          address) for 24 KiB images, F800 (BIOS address) for 32 KiB images,\n");
	printf("          F000 for 64 KiB images, and E000 for 128 KiB images.\n");
	printf("   -s   - Specifies ROM size for -r and -c options.\n");
	printf("	  The default is %u.\n\n", DEFAULT_ROM_SIZE);
	exit(1);
}

void error(char *message)
{
	printf("ERROR: %s\n\n", message);
	usage();
}

void pit_delay(unsigned int ticks)
{
	__asm {
		push	ax
		push	bx
		in	al,0x61
		or	al,0x01			/* enable 8254 PIT channel 2 */
		out	0x61,al			/* write to 8255 PPI port B */
		mov	bl,al			/* save written value for XT/AT test */
		mov	al,0xB0			/* set PIT channel 2 to mode 0 */
		out	0x43,al			/* write control word to PIT */
		mov	ax,ticks
		out	0x42,al			/* set PIT channel 2 inital count - low byte */
		mov	al,ah
		out	0x42,al			/* set PIT channel 2 inital count - high byte */
	delay_loop:
		in	al,0x61			/* try reading port B first */
		cmp	al,bl
		jne	delay_test		/* port B value had changed, must be Xi 8088 (or an AT) */
		in	al,0x62			/* read PPI port C */
	delay_test:
		test	al,0x20			/* check if bit 5 set - PIT channel 2 output */
		jz	delay_loop		/* not set, keep waiting */
		pop	bx
		pop	ax
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

void rom_verify(__segment rom_seg, __segment file_seg, unsigned long rom_size) {
	unsigned long bytes_to_verify, verify_size, offset, diff = 0;
	unsigned char rom_data, file_data;

	bytes_to_verify = rom_size;
	while (bytes_to_verify > 0) {
		/* verify up to 64 KiB at a time */
		if (bytes_to_verify > 0x10000) {
			verify_size = 0x10000;
		} else {
			verify_size = bytes_to_verify;
		}
		for (offset = 0; offset < verify_size; offset++) {
			rom_data = ((unsigned char __far *)rom_seg:>0)[offset];
			file_data = ((unsigned char __far *)file_seg:>0)[offset];

			if (rom_data != file_data) {
				printf("WARNING: Difference found at 0x%04X:%04X: ROM = 0x%02X; file 0x%02X\n", rom_seg, (unsigned int) offset, rom_data, file_data);
				diff++;
			}
		}
		/* advance addresses by 64 KiB by incrementing the segment */
		rom_seg += 0x1000;
		file_seg += 0x1000;
		bytes_to_verify -= verify_size;
	}

	if (diff > 0) {
		printf("WARNING: %d differences found\n", diff);
	} else {
		printf("No differences found\n");
	}
}

/* rom_read - DUMP ROM content to a file */
void rom_read(__segment rom_seg, char *out_file, unsigned long rom_size) {
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
		if (bytes_to_write > 0x8000) {
			write_size = 0x8000;
		} else {
			write_size = bytes_to_write;
		}
		if ((count = fwrite(rom_seg:>0, 1, write_size, fp_out)) != write_size) {
			printf("ERROR: Short write while writing %s. Wrote %u bytes, expected to write %u bytes.\n",
				out_file, count, write_size);
			exit(3);
		}
		/* advance write address by 32 KiB by incrementing the segment */
		rom_seg += 0x0800;
		bytes_to_write -= write_size;
	}
	fclose(fp_out);
}

__segment load_file(char *in_file, unsigned long *rom_size) {
	FILE *fp_in;
	size_t count, read_size;
	unsigned long bytes_to_read, buf_addr;
	__segment file_segment, read_segment;
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
	
	if ((buf_addr = (unsigned long) halloc (*rom_size, 1)) == 0x0) {
		printf("ERROR: Failed to allocate %lu bytes for input buffer.\n",
		       *rom_size);
		exit(5);
	}
	/* calculate the segment and the offset of the buffer */
	file_segment = buf_addr >> 16;
	if ((buf_addr & 0xFFFF) != 0) {
		printf("ERROR: halloc() returned address with non-zero offset: 0x%08lX\n, buf_addr");
		exit(5);

	}

	bytes_to_read = *rom_size;
	read_segment = file_segment;
	while (bytes_to_read > 0) {
		/* read up to 32 KiB at a time */
		if (bytes_to_read > 0x8000) {
			read_size = 0x8000;
		} else {
			read_size = bytes_to_read;
		}
		if ((count = fread(read_segment:>0, 1, read_size, fp_in)) != read_size) {
			printf("ERROR: Short read while reading %s. Read %u bytes, expected to read %u bytes.\n",
			       in_file, count, read_size);
			exit(6);
		}
		/* advance read address by 32 KiB by incrementing the segment */
		read_segment += 0x0800;
		bytes_to_read -= read_size;
	}
	fclose(fp_in);
	return file_segment;
}
	               
unsigned int checksum (__segment data_seg, unsigned long rom_size)
{
	unsigned int checksum_size, checksum = 0;
	unsigned long bytes_to_checksum = rom_size, offset;
	while (bytes_to_checksum > 0) {
		if (bytes_to_checksum > 0x8000) {
			checksum_size = 0x8000;
		} else {
			checksum_size = bytes_to_checksum;
		}
		for (offset = 0; offset < checksum_size; offset++) {
			checksum += ((unsigned char __far *) data_seg:>0)[offset];
		}
		/* advance checksum address by 32 KiB by incrementing the segment */
		data_seg += 0x0800;
		bytes_to_checksum -= checksum_size;
	}
	return checksum;
}

/* rom_identify - Identify flash ROM type, return index in eeprom table and start segment */
int rom_identify(__segment rom_seg)
{
	int index = -1;
	volatile unsigned char __far *rom_start = rom_seg:>0;
	unsigned char byte0, byte1, vendor_id, device_id;

	byte0 = rom_start[0];
	byte1 = rom_start[1];

	cmd_addr1 = 0x5555;
	cmd_addr2 = 0x2AAA;

	/* Enter software ID mode */
	interrupts_disable();
	rom_start[cmd_addr1] = 0xAA;
	rom_start[cmd_addr2] = 0x55;
	rom_start[cmd_addr1] = 0x90;
	pit_delay(IDENTIFY_DELAY);

	vendor_id = rom_start[0];
	device_id = rom_start[1];

	if (vendor_id == byte0 && device_id == byte1) {
		/* Try alternate software ID mode */
		rom_start[cmd_addr1] = 0xAA;
		rom_start[cmd_addr2] = 0x55;
		rom_start[cmd_addr1] = 0x80;
		rom_start[cmd_addr1] = 0xAA;
		rom_start[cmd_addr2] = 0x55;
		rom_start[cmd_addr1] = 0x60;
		pit_delay(IDENTIFY_DELAY);

		vendor_id = rom_start[0];
		device_id = rom_start[1];
	}

	if (vendor_id == byte0 && device_id == byte1) {
		/* Try 0x555 and 0x2AA addresses for commands */
		cmd_addr1 = 0x555;
		cmd_addr2 = 0x2AA;

		rom_start[cmd_addr1] = 0xAA;
		rom_start[cmd_addr2] = 0x55;
		rom_start[cmd_addr1] = 0x80;
		rom_start[cmd_addr1] = 0xAA;
		rom_start[cmd_addr2] = 0x55;
		rom_start[cmd_addr1] = 0x60;
		pit_delay(IDENTIFY_DELAY);

		vendor_id = rom_start[0];
		device_id = rom_start[1];
	}

	/* Exit software ID mode */
	rom_start[cmd_addr1] = 0xAA;
	rom_start[cmd_addr2] = 0x55;
	rom_start[cmd_addr1] = 0xF0;
	pit_delay(IDENTIFY_DELAY);

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

int rom_erase_page(__segment rom_seg, __segment page_seg)
{
	unsigned int timeout;
	volatile unsigned char __far *rom_start = rom_seg:>0;
	volatile unsigned char __far *rom_address = page_seg:>0;

	/* Enter page erase mode */
	rom_start[cmd_addr1] = 0xAA;
	rom_start[cmd_addr2] = 0x55;
	rom_start[cmd_addr1] = 0x80;
	rom_start[cmd_addr1] = 0xAA;
	rom_start[cmd_addr2] = 0x55;
	rom_address[0] = 0x30;

	/* poll EPROM - wait for erase operation to complete */
	for (timeout = 0; timeout < ERASE_TIMEOUT; timeout++) {
		if (rom_address[0] == 0xFF)
			return 0;
		pit_delay(WRITE_DELAY);
	}

	return 1;
}

int rom_program_page(__segment rom_seg, __segment page_seg, __segment file_seg, unsigned int page_size, unsigned char page_write)
{
	unsigned int offset, timeout;
	volatile unsigned char __far *rom_start = rom_seg:>0;
	volatile unsigned char __far *rom_address = page_seg:>0;
	unsigned char __far *file_address = file_seg:>0;

	if (page_write) {
		/* Enter page write mode */
		rom_start[cmd_addr1] = 0xAA;
		rom_start[cmd_addr2] = 0x55;
		rom_start[cmd_addr1] = 0xA0;

		/* write page */
		for (offset = 0; offset < page_size; offset++)
			rom_address[offset] = file_address[offset];

		/* poll EPROM - wait for write operation to complete */
		for (timeout = 0; timeout < PAGE_WRITE_TIMEOUT; timeout++) {
			if (rom_address[page_size - 1] == file_address[page_size - 1])
				return 0;
			pit_delay(WRITE_DELAY);
		}
	} else {
		for (offset = 0; offset < page_size; offset++) {
			/* Enter write mode */
			rom_start[cmd_addr1] = 0xAA;
			rom_start[cmd_addr2] = 0x55;
			rom_start[cmd_addr1] = 0xA0;

			/* write byte */
			rom_address[offset] = file_address[offset];

			/* verify byte */
			for (timeout = 0; timeout < BYTE_WRITE_TIMEOUT; timeout++) {
				if (rom_address[offset] == file_address[offset])
					break;
				pit_delay(WRITE_DELAY);
			}
			if (timeout == BYTE_WRITE_TIMEOUT)
				break;
		}
		if (offset == page_size)
			return 0;
	}

	return 1;
}

void rom_program(__segment rom_seg, __segment file_seg, unsigned long rom_size)
{
	unsigned int eeprom_index;
	__segment rom_start;
	unsigned int page, page_size, num_pages, page_paragraph, pages_per_column = 1;
	unsigned char __far *video_address;

	if (rom_seg < 0xE000) {
		/* if not flashing system ROM BIOS area, assume that the ROM starts at the ROM segment */
		rom_start = rom_seg;
		if ((eeprom_index = rom_identify(rom_start)) == -1) {
			error("Cannot detect flash ROM type.\nMake sure that flash ROM is not write protected.");
		}
	} else {
		/* for system ROM BIOS - try 0xF000 first */
		rom_start = 0xF000;
		if ((eeprom_index = rom_identify(rom_start)) == -1) {
			/* try 0xE000 */
			rom_start = 0xE000;
			if ((eeprom_index = rom_identify(rom_start)) == -1) {
				error("Cannot detect flash ROM type.\nOn Sergey's XT Version 1.0 systems make sure that SW2.6 - SW2.7 are OFF.");
			}
		}
	}

	printf("Detected flash ROM at 0x%04X, type: %s %s, page size: %u bytes.\n",
		rom_start, eeproms[eeprom_index].vendor_name, eeproms[eeprom_index].device_name,
		eeproms[eeprom_index].page_size);

	page_size = eeproms[eeprom_index].page_size;
	/* check that requested ROM segment is on the page boundary */
	page_paragraph = page_size >> 4;
	if ((rom_seg / page_paragraph) * page_paragraph != rom_seg) {
		printf("ERROR: Specified ROM segment (0x%04X) doesn't start on the page boundary.\n",
			rom_seg);
	}

	/* figure out number of pages to program */
	num_pages = rom_size / page_size;
	if ((unsigned long) num_pages * page_size != rom_size) {
		printf("ERROR: ROM image size (%lu) is is not a multiply of the flash page size.\n",
			rom_size);
		exit(10);
	}

	printf("Programming the flash ROM with %lu bytes starting at address 0x%04X:0000.\n", rom_size, rom_seg);
	printf("Please wait. Do not reboot the system!\n");
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
			video_write_char(video_address + (page / pages_per_column) * 2, 'E', 0x07);
			rom_erase_page(rom_start, rom_seg);
			/* note: not checking the exit code, will try to program the flash ROM anyway */
		}
		video_write_char(video_address + (page / pages_per_column) * 2, 'P', 0x07);
		rom_program_page(rom_start, rom_seg, file_seg, page_size, eeproms[eeprom_index].page_write);
		video_write_char(video_address + (page / pages_per_column) * 2, 0xDB, 0x07);
		rom_seg += page_size >> 4;
		file_seg += page_size >> 4;
	}

	interrupts_enable();
	printf("Flash ROM has been programmed successfully. Please reboot the system.\n");
}

int main(int argc, char *argv[])
{
 	int i;
	unsigned int mode = 0;
	__segment rom_seg = 0xF800, file_seg;
	char *in_file = NULL, *out_file = NULL;
	unsigned long rom_size = DEFAULT_ROM_SIZE;

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
				if (rom_seg < 0xC000)
					error("Invalid ROM segment specified (must be in C000-FFFF range).");
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
		rom_read(rom_seg, out_file, rom_size);
	
	if ((mode & MODE_PROG) || (mode & MODE_VERIFY) ||
	    ((mode & MODE_CHECKSUM) && in_file != NULL)) {
		file_seg = load_file(in_file, &rom_size);
		if (rom_seg == 0xF800) {
			if (rom_size == 65536) {
				rom_seg = 0xF000;	/* set default ROM segment to F0000 for 64 KiB images */
			} else if (rom_size == 131072) {
				rom_seg = 0xE000;
			} else if (rom_size == 24576) {
				rom_seg = 0xFA00;	/* special case for Micro 8088, 24 KiB ROM images */
			}
		}
		/* check if ROM size extends beyond 1 MiB */
		if ((rom_size + 15) / 16 - 1 + rom_seg < rom_seg) {
			error("ROM image extends beyond 1 MiB. Make sure that the correct image file is specified. Also check -a option's argument (if specified).");
		}
	}

	if (mode & MODE_CHECKSUM) {
		if (NULL == in_file)
			printf("Current ROM checksum at 0x%X:0000 is 0x%X\n", rom_seg,
			       checksum(rom_seg, rom_size));
		else
			printf("The checksum of %s is 0x%X\n", in_file,
			       checksum(file_seg, rom_size));
	}

	if (mode & MODE_PROG) {
		rom_program(rom_seg, file_seg, rom_size);
	}

	if (mode & MODE_VERIFY) {
		rom_verify(rom_seg, file_seg, rom_size);
	}

	return 0;
}

