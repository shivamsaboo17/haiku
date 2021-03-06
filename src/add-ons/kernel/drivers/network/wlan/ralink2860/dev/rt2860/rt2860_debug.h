
/*-
 * Copyright (c) 2009-2010 Alexander Egorenkov <egorenar@gmail.com>
 * Copyright (c) 2009 Damien Bergamini <damien.bergamini@free.fr>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _RT2860_DEBUG_H_
#define _RT2860_DEBUG_H_

#ifdef RT2860_DEBUG

enum
{
	RT2860_DEBUG_EEPROM = 0x00000001,
	RT2860_DEBUG_RX = 0x00000002,
	RT2860_DEBUG_TX = 0x00000004,
	RT2860_DEBUG_INTR = 0x00000008,
	RT2860_DEBUG_STATE = 0x00000010,
	RT2860_DEBUG_CHAN = 0x00000020,
	RT2860_DEBUG_NODE = 0x00000040,
	RT2860_DEBUG_KEY = 0x00000080,
	RT2860_DEBUG_PROT = 0x00000100,
	RT2860_DEBUG_WME = 0x00000200,
	RT2860_DEBUG_BEACON = 0x00000400,
	RT2860_DEBUG_BA = 0x00000800,
	RT2860_DEBUG_STATS = 0x00001000,
	RT2860_DEBUG_RATE = 0x00002000,
	RT2860_DEBUG_PERIODIC = 0x00004000,
	RT2860_DEBUG_WATCHDOG = 0x00008000,
	RT2860_DEBUG_ANY = 0xffffffff
};

#define RT2860_DPRINTF(sc, m, fmt, ...)		do { if ((sc)->debug & (m)) printf(fmt, __VA_ARGS__); } while (0)

#else

#define RT2860_DPRINTF(sc, m, fmt, ...)

#endif /* #ifdef RT2860_DEBUG */

#endif /* #ifndef _RT2860_DEBUG_H_ */
