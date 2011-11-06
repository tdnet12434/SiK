// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2011 Michael Smith, All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  o Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  o Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.
//

///
/// @file	bootloader.c
///
/// A UART bootloader for the SiLabs Si1000 SoC.
///
/// Protocol inspired by the STK500 protocol by way of Arduino.
///

#include <compiler_defs.h>
#include <Si1000_defs.h>
#include <stdio.h>
#include <stdint.h>

#include "board.h"
#include "bootloader.h"
#include "board_info.h"
#include "flash.h"
#include "util.h"

#if 0
# define trace(_x)	cout(_x);
#else
# define trace(_x)
#endif

// Send the default 'in sync' response.
//
static void	sync_response(void);

// Read a 16-bit unsigned quantity in little-endian form
//
static uint16_t	get_uint16(void);

// Initialise the Si1000 and board hardware
//
static void	hardware_init(void);

// Patchbay for the board frequency byte.
// This is patched in the hex file(s) after building.
//
__at(0x3ff) uint8_t __code board_frequency = FREQ_NONE;

uint8_t __data	buf[PROTO_PROG_MULTI_MAX];

// Bootloader entry logic
//
void
bootloader(void)
{
	uint8_t		c;
	uint16_t	address;
	uint8_t		count, i;

	// Do early hardware init
	hardware_init();

	// Turn on the LED to indicate the bootloader is running
	//
	LED_BOOTLOADER = LED_ON;

	trace('R');
	trace(RSTSRC);

	// Boot the application if:
	//
	// - the reset was not due to a flash error
	// - the signature is valid
	// - the boot-to-bootloader strap/button is not in the active state
	//
	if (!(RSTSRC & (1 << 6)) &&
	    flash_app_valid() &&
	    (BUTTON_BOOTLOAD != BUTTON_ACTIVE)) {

		// Stash board info in SFRs for the application to find later
		//
		BOARD_FREQUENCY_REG = board_frequency;
		BOARD_BL_VERSION_REG = BL_VERSION;

		// And jump
		((void (__code *)(void))FLASH_APP_START)();
	}

	trace('B');

	// Main bootloader loop
	//
	address = 0;
	for (;;) {

		// Wait for a command byte
		LED_BOOTLOADER = LED_ON;
		c = cin();
		LED_BOOTLOADER = LED_OFF;

		// common tests for EOC
		switch (c) {
		case PROTO_GET_SYNC:
		case PROTO_GET_DEVICE:
		case PROTO_CHIP_ERASE:
		case PROTO_PARAM_ERASE:
		case PROTO_READ_FLASH:
			if (cin() != PROTO_EOC)
				goto cmd_bad;
		}

		switch (c) {

		case PROTO_GET_SYNC:		// sync
			break;

		case PROTO_GET_DEVICE:
			cout(BOARD_ID);
			cout(board_frequency);
			break;

		case PROTO_CHIP_ERASE:		// erase the program area
			flash_erase_app();
			break;

		case PROTO_PARAM_ERASE:
			flash_erase_scratch();
			break;

		case PROTO_LOAD_ADDRESS:	// set address
			address = get_uint16();
			if (cin() != PROTO_EOC)
				goto cmd_bad;
			break;

		case PROTO_PROG_FLASH:		// program byte
			c = cin();
			if (cin() != PROTO_EOC)
				goto cmd_bad;
			flash_write_byte(address++, c);
			break;

		case PROTO_READ_FLASH:		// readback byte
			c = flash_read_byte(address++);
			cout(c);
			break;

		case PROTO_PROG_MULTI:
			count = cin();
			if (count > sizeof(buf))
				goto cmd_bad;
			for (i = 0; i < count; i++)
				buf[i] = cin();
			if (cin() != PROTO_EOC)
				goto cmd_bad;
			for (i = 0; i < count; i++)
				flash_write_byte(address++, buf[i]);
			break;

		case PROTO_READ_MULTI:
			count = cin();
			if (cin() != PROTO_EOC)
				goto cmd_bad;
			for (i = 0; i < count; i++) {
				c = flash_read_byte(address++);
				cout(c);
			}
			break;

		case PROTO_REBOOT:
			// generate a software reset, which will boot to the application
			RSTSRC |= (1 << 4);

		default:
			goto cmd_bad;
		}
cmd_ok:
		sync_response();
		continue;
cmd_bad:
		continue;
	}
}

static void
sync_response(void)
{
	cout(PROTO_INSYNC);	// "in sync"
	cout(PROTO_OK);		// "OK"
}

static uint16_t
get_uint16(void)
{
	uint16_t	val;

	val = cin();
	val |= (uint16_t)cin() << 8;

	trace(val & 0xff);
	trace(val >> 8);

	return val;
}

// Do minimal hardware init required for bootloader
//
static void
hardware_init(void)
{
	int i = 0;

	// Disable interrupts - we run with them off permanently
	// as all interrupts vector to the application.
	EA	 =  0x00;

	// Disable the watchdog timer
	PCA0MD	&= ~0x40;

	// Select the internal oscillator, prescale by 1
	FLSCL	 =  0x40;
	OSCICN	 =  0x8F;
	CLKSEL	 =  0x00;

	// Configure Timers
	TCON	 =  0x40;		// Timer1 on
	TMOD	 =  0x20;		// Timer1 8-bit auto-reload
	CKCON	 =  0x08;		// Timer1 from SYSCLK
	TH1	 =  0x96;		// 115200 bps

	// Configure UART
	SCON0	 =  0x12;		// enable receiver, set TX ready

	// Configure the VDD brown out detector
	VDM0CN	 =  0x80;
	for (i = 0; i < 350; i++);	// Wait 100us for initialization
	RSTSRC	 =  0x06;		// enable brown out and missing clock reset sources

	// Configure crossbar for UART
	P0MDOUT	 =  0x10;		// UART Tx push-pull
	SFRPAGE	 =  CONFIG_PAGE;
	P0DRV	 =  0x10;		// UART TX
	SFRPAGE	 =  LEGACY_PAGE;
	XBR0	 =  0x01;		// UART enable

	// Hardware-specific init for LED and button
	HW_INIT;

	XBR2	 =  0x40;		// Crossbar (GPIO) enable
}
