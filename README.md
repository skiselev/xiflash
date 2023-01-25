# xiflash - Flash ROM Utility for Xi 8088 and Micro 8088 systems

## Description

The xiflash utility is a flash ROM utility for Xi 8088 and Micro 8088 systems. The main purpose is in-system flash ROM programming, for example upgrading the system BIOS image in these systems.

The utility can program any series of consecutive flash ROM sectors, also known as pages or blocks, mapped to the system's memory with the provided image file. Thus it is possible to reprogram other parts of the flash ROM, for example program a BIOS extension ROM, like XT-IDE BIOS, to the available ROM space.

By default xiflash works on 0xF8000-0xFFFFF range, which is normally used by system's BIOS, unless progamming or verifying 64 KiB images or 128 KiB images, in which case the default is to use 0xF0000-0xFFFFF range or 0xE0000-0xFFFFF range respectively.

## Important Notes

* This is a "work in progress" code. It was tested to some extent and all weirdness that was discovered is described in the "Known Issues and Limitations" section below. Yet it is not guaranteed to work on your board, and it is quite possible that it will destroy the contents of your flash ROM. Please keep an EEPROM programmer handy.

* By design the xiflash utility has a minimalistic UNIX-like user interface. It is limited to command line options and arguments, some output on the screen, and exit codes. It will not prompt for user confirmation, it will simply try to perform the action specified in the command line. So please make yourself familiar with the command line options and double check command line options before running the program. Run program without any options to see the detailed usage information.

## Usage Examples

1. Re-program the system BIOS with the image from bios.bin file:
> xiflash -i bios.bin -p

Note: It is assumed that bios.bin is a valid system BIOS ROM image, and its size is 24 KiB, 32 KiB or 64 KiB. xiflash will program 24 KiB images starting from address 0xFA000, 32 KiB images starting from address 0xF8000, and 64 KiB images starting from address 0xF0000

2. Read the current system BIOS code from the code and save it to the backup.bin file:
> xiflash -o backup.bin -r

Note: This command will read 32 KiB from the ROM starting from address 0xF8000 and write it to the file. The -s option can be used to specify a different image size, and -a option can be used to specify a different starting address.

3. Program the XT-IDE BIOS extension (xt-ide.bin) into system's flash ROM,
starting from address 0xF0000:
> xiflash -i xt-ide.bin -a F000 -p

Note: It is assumed that xt-ide.bin is smaller that 32 KiB and won't overwrite system's BIOS. Is is also assumed that 0xF0000 area is available.

## Release Notes

### Version 0.5 - January 25, 2023
* Use 0xFA00 as the default address for 24 KiB images. That's the image size for Micro 8088 BIOS

### Version 0.4 - November 29, 2021
* Fixed programming ROM images larger than 64 KiB

### Version 0.3 - November 22, 2021
* Updated delay function to use 8254 PIT for timing
* Added a check to set starting ROM address to E000 when 128 KiB image is programmed

### Version 0.2 - November 21, 2021
* Updated delay function to work both on Xi 8088 and Micro 8088 systems
* Implemented support for 64 KiB flash images, with some ideas borrowed from from uflash by Aitor Gomez (spark2k06)
* Added progress bar for the flash programming process (only in text modes)
* Added a check to set starting ROM address to F000 when 64 KiB image is programmed

### Version 0.1 - December 25, 2012.
This is the initial release. It supports the following flash ROM devices:
* AMD Am29F010 (not tested yet)
* Atmel AT29C010
* Winbond W29EE011
* SST SST29EE010 / Greenliant GLS29EE010
* SST / Microchip SST39SF010

## Known Issues and Limitations

* Winbond W29EE011A-15 - On Xi 8088, flash ROM fails to exit vendor/device identification mode when system is in turbo mode. System will be stuck and will need to be powered down/up to reset the flash ROM, regular hard reset won't help. Apparently the chip is not fast enough. Make sure to turn off the turbo mode when programming this IC.

* The utility doesn't specifically support images larger than 64 KiB, and most likely will fail or behave unpredictably.

* On the Sergey's XT board it might be necessary to turn OFF jumpers SW2.5-SW2.7. This will map the entire flash ROM to the 0xE0000-0xFFFFF area. This is not necessary on the Xi 8088 and Micro 8088 systems, as the flash ROM starts at 0xF0000, which is always mapped to the system's memory.
