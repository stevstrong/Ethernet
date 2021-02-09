/*
 * Copyright 2018 Paul Stoffregen
 * Copyright (c) 2010 by Cristian Maglie <c.maglie@bug.st>
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of either the GNU General Public License version 2
 * or the GNU Lesser General Public License version 2.1, both as
 * published by the Free Software Foundation.
 */

#include <Arduino.h>
#include "Ethernet.h"
#include "w5500.h"


/***************************************************/
/**            Default SS pin setting             **/
/***************************************************/

// If variant.h or other headers specifically define the
// default SS pin for ethernet, use it.
#if defined(PIN_SPI_SS_ETHERNET_LIB)
#define SS_PIN_DEFAULT  PIN_SPI_SS_ETHERNET_LIB

// MKR boards default to pin 5 for MKR ETH
// Pins 8-10 are MOSI/SCK/MISO on MRK, so don't use pin 10
#elif defined(USE_ARDUINO_MKR_PIN_LAYOUT) || defined(ARDUINO_SAMD_MKRZERO) || defined(ARDUINO_SAMD_MKR1000) || defined(ARDUINO_SAMD_MKRFox1200) || defined(ARDUINO_SAMD_MKRGSM1400) || defined(ARDUINO_SAMD_MKRWAN1300)
#define SS_PIN_DEFAULT  5

// For boards using AVR, assume shields with SS on pin 10
// will be used.  This allows for Arduino Mega (where
// SS is pin 53) and Arduino Leonardo (where SS is pin 17)
// to work by default with Arduino Ethernet Shield R2 & R3.
#elif defined(__AVR__)
#define SS_PIN_DEFAULT  10

// If variant.h or other headers define these names
// use them if none of the other cases match
#elif defined(PIN_SPI_SS)
#define SS_PIN_DEFAULT  PIN_SPI_SS
#elif defined(CORE_SS0_PIN)
#define SS_PIN_DEFAULT  CORE_SS0_PIN

// As a final fallback, use pin 10
#else
#define SS_PIN_DEFAULT  10
#endif




// W5500 controller instance
uint8_t  W5500Class::CH_BASE_MSB;
uint8_t  W5500Class::ss_pin = SS_PIN_DEFAULT;
#ifdef ETHERNET_LARGE_BUFFERS
uint16_t W5500Class::SSIZE = 2048;
uint16_t W5500Class::SMASK = 0x07FF;
#endif
W5500Class W5500;

// pointers and bitmasks for optimized SS pin
#if 1 //defined(__AVR__)
  volatile uint32 * W5500Class::ss_pin_reg;
  uint32 W5500Class::ss_pin_mask;
//	static volatile uint32 *ss_pin_reg;
//	static uint32 ss_pin_mask;
#endif

//-----------------------------------------------------------------------------
uint8_t W5500Class::isW5500(void)
{
	//Serial.println("w5500.cpp: detect W5500 chip");
	if (!softReset()) return 0;
	writeMR(0x08);
	if (readMR() != 0x08) return 0;
	writeMR(0x10);
	if (readMR() != 0x10) return 0;
	writeMR(0x00);
	if (readMR() != 0x00) return 0;
	int ver = readVERSIONR();
	//Serial.print("version=");
	//Serial.println(ver);
	if (ver != 4) return 0;
	//Serial.println("chip is W5500");
	return 1;
}
//-----------------------------------------------------------------------------
uint8_t W5500Class::init(void)
{
	static bool initialized = false;

	if (initialized) return 1;

	// Many Ethernet shields have a CAT811 or similar reset chip
	// connected to W5500 or W5200 chips.  The W5200 will not work at
	// all, and may even drive its MISO pin, until given an active low
	// reset pulse!  The CAT811 has a 240 ms typical pulse length, and
	// a 400 ms worst case maximum pulse length.  MAX811 has a worst
	// case maximum 560 ms pulse length.  This delay is meant to wait
	// until the reset pulse is ended.  If your hardware has a shorter
	// reset time, this can be edited or removed.
	delay(560);
	//Serial.println("w5500 init");

	initSS();
	resetSS();
	SPI.beginTransaction(SPI_ETHERNET_SETTINGS);

	// plausability check
	if (! isW5500()) return 0;

	CH_BASE_MSB = 0x10;
#ifdef ETHERNET_LARGE_BUFFERS
#if MAX_SOCK_NUM <= 1
	SSIZE = 16384;
#elif MAX_SOCK_NUM <= 2
	SSIZE = 8192;
#elif MAX_SOCK_NUM <= 4
	SSIZE = 4096;
#else
	SSIZE = 2048;
#endif
	SMASK = SSIZE - 1;
	for (i=0; i<MAX_SOCK_NUM; i++) {
		writeSnRX_SIZE(i, SSIZE >> 10);
		writeSnTX_SIZE(i, SSIZE >> 10);
	}
	for (; i<8; i++) {
		writeSnRX_SIZE(i, 0);
		writeSnTX_SIZE(i, 0);
	}
#endif

//	SPI.endTransaction();
	initialized = true;
	return 1; // successful init
}
//-----------------------------------------------------------------------------
// Soft reset the Wiznet chip, by writing to its MR register reset bit
uint8_t W5500Class::softReset(void)
{
	uint16_t count=0;

	//Serial.println("Wiznet soft reset");
	// write to reset bit
	writeMR(0x80);
	// then wait for soft reset to complete
	do {
		uint8_t mr = readMR();
		//Serial.print("mr=");
		//Serial.println(mr, HEX);
		if (mr == 0) return 1;
		delay(1);
	} while (++count < 20);
	return 0;
}
//-----------------------------------------------------------------------------
W5500Linkstatus W5500Class::getLinkStatus()
{
	uint8_t phystatus;

	if (!init()) return UNKNOWN;
	phystatus = readPHYCFGR();
	if (phystatus & 0x01) return LINK_ON;
	return LINK_OFF;
}
//-----------------------------------------------------------------------------
uint16_t W5500Class::write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
	uint8_t cmd[8];

	// chip == 55
	setSS();
	if (addr < 0x100) {
		// common registers 00nn
		cmd[0] = 0;
		cmd[1] = addr & 0xFF;
		cmd[2] = 0x04;
	} else if (addr < 0x8000) {
		// socket registers  10nn, 11nn, 12nn, 13nn, etc
		cmd[0] = 0;
		cmd[1] = addr & 0xFF;
		cmd[2] = ((addr >> 3) & 0xE0) | 0x0C;
	} else if (addr < 0xC000) {
		// transmit buffers  8000-87FF, 8800-8FFF, 9000-97FF, etc
		//  10## #nnn nnnn nnnn
		cmd[0] = addr >> 8;
		cmd[1] = addr & 0xFF;
		#if defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 1
		cmd[2] = 0x14;                       // 16K buffers
		#elif defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 2
		cmd[2] = ((addr >> 8) & 0x20) | 0x14; // 8K buffers
		#elif defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 4
		cmd[2] = ((addr >> 7) & 0x60) | 0x14; // 4K buffers
		#else
		cmd[2] = ((addr >> 6) & 0xE0) | 0x14; // 2K buffers
		#endif
	} else {
		// receive buffers
		cmd[0] = addr >> 8;
		cmd[1] = addr & 0xFF;
		#if defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 1
		cmd[2] = 0x1C;                       // 16K buffers
		#elif defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 2
		cmd[2] = ((addr >> 8) & 0x20) | 0x1C; // 8K buffers
		#elif defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 4
		cmd[2] = ((addr >> 7) & 0x60) | 0x1C; // 4K buffers
		#else
		cmd[2] = ((addr >> 6) & 0xE0) | 0x1C; // 2K buffers
		#endif
	}
	if (len <= 5) {
		for (uint8_t i=0; i < len; i++) {
			cmd[i + 3] = buf[i];
		}
		SPI.write(cmd, len + 3);
	} else {
		SPI.write(cmd, 3);
#ifdef SPI_HAS_TRANSFER_BUF
		SPI.write(buf, len);
#else
		// TODO: copy 8 bytes at a time to cmd[] and block transfer
		for (uint16_t i=0; i < len; i++) {
			SPI.write(buf[i]);
		}
#endif
	}
	resetSS();
	return len;
}
//-----------------------------------------------------------------------------
uint16_t W5500Class::read(uint16_t addr, uint8_t *buf, uint16_t len)
{
	uint8_t cmd[4];

	// chip == 55
	setSS();
	if (addr < 0x100) {
		// common registers 00nn
		cmd[0] = 0;
		cmd[1] = addr & 0xFF;
		cmd[2] = 0x00;
	} else if (addr < 0x8000) {
		// socket registers  10nn, 11nn, 12nn, 13nn, etc
		cmd[0] = 0;
		cmd[1] = addr & 0xFF;
		cmd[2] = ((addr >> 3) & 0xE0) | 0x08;
	} else if (addr < 0xC000) {
		// transmit buffers  8000-87FF, 8800-8FFF, 9000-97FF, etc
		//  10## #nnn nnnn nnnn
		cmd[0] = addr >> 8;
		cmd[1] = addr & 0xFF;
		#if defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 1
		cmd[2] = 0x10;                       // 16K buffers
		#elif defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 2
		cmd[2] = ((addr >> 8) & 0x20) | 0x10; // 8K buffers
		#elif defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 4
		cmd[2] = ((addr >> 7) & 0x60) | 0x10; // 4K buffers
		#else
		cmd[2] = ((addr >> 6) & 0xE0) | 0x10; // 2K buffers
		#endif
	} else {
		// receive buffers
		cmd[0] = addr >> 8;
		cmd[1] = addr & 0xFF;
		#if defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 1
		cmd[2] = 0x18;                       // 16K buffers
		#elif defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 2
		cmd[2] = ((addr >> 8) & 0x20) | 0x18; // 8K buffers
		#elif defined(ETHERNET_LARGE_BUFFERS) && MAX_SOCK_NUM <= 4
		cmd[2] = ((addr >> 7) & 0x60) | 0x18; // 4K buffers
		#else
		cmd[2] = ((addr >> 6) & 0xE0) | 0x18; // 2K buffers
		#endif
	}
	SPI.write(cmd, 3);
	memset(buf, 0, len);
	SPI.transfer(buf, len);
	resetSS();

	return len;
}
//-----------------------------------------------------------------------------
void W5500Class::execCmdSn(SOCKET s, SockCMD _cmd)
{
	// Send command to socket
	writeSnCR(s, _cmd);
	// Wait for command to complete
	while (readSnCR(s)) ;
}
