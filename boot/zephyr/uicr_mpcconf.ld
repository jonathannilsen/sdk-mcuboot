/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* This linker script is used together with a zephyr,memory-region named "UICR_MPCCONF"
 * in devicetree to place the .mpcconf_entry into a loadable (PROGBITS) output section
 * so it is included in zephyr.hex. This is done because the automatically generated
 * devicetree sections have (NOLOAD), causing the data to not be included in zephyr.hex. 
 */
SECTION_PROLOGUE(.mpcconf_entry,,)
{
	KEEP(*(.mpcconf_entry))
	*(".mpcconf_entry.*")
} > UICR_MPCCONF AT > UICR_MPCCONF
