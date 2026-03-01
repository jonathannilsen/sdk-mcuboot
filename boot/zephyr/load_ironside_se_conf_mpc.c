/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "bootutil/bootutil.h"
#include "bootutil/bootutil_log.h"
#include "flash_map_backend/flash_map_backend.h"
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <ironside/se/api.h>
#include <ironside/se/mpcconf.h>

#include "tmp_mpcconf_macros.h"

BOOT_LOG_MODULE_DECLARE(mcuboot);

BUILD_ASSERT(MCUBOOT_IMAGE_NUMBER <= 2,
	     "The MPC override configuration used to provide write protection for the image "
	     "partitions does not currently support more than two images.");

/* clang-format off */
#define MIN_START_ADDR(_label0, _label1)                                                           \
	MIN(DT_REG_ADDR(DT_NODELABEL(_label0)),                                                    \
	    COND_CODE_1(DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(_label1)),                            \
	        (DT_REG_ADDR(DT_NODELABEL(_label1))),                                              \
	        (UINTPTR_MAX)))                                                                    \

#define MAX_END_ADDR(_label0, _label1)                                                             \
	MAX((DT_REG_ADDR(DT_NODELABEL(_label0)) + DT_REG_SIZE(DT_NODELABEL(_label0))),             \
	    COND_CODE_1(DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(_label1)),                            \
	        ((DT_REG_ADDR(DT_NODELABEL(_label1)) + DT_REG_SIZE(DT_NODELABEL(_label1)))),       \
	        (0)))
/* clang-format on */

/* TODO: these should correspond to PROTECTEDMEM, not necessarily the bootloader code region */
#define ACCESSIBLE_MRAM_START DT_REG_ADDR(DT_CHOSEN(zephyr_code_partition))

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(secure_storage_partition))
BUILD_ASSERT((DT_REG_ADDR(DT_NODELABEL(secure_storage_partition)) +
	      DT_REG_SIZE(DT_NODELABEL(secure_storage_partition))) ==
		     (DT_REG_ADDR(DT_NODELABEL(mram1x)) + DT_REG_SIZE(DT_NODELABEL(mram1x))),
	     "The MPC override configuration used to provide write protection for the image "
	     "partitions currently requires that the secure storage partitions are placed at the "
	     "end of MRAM.");

#define ACCESSIBLE_MRAM_END DT_REG_ADDR(DT_NODELABEL(secure_storage_partition))
#else
#define ACCESSIBLE_MRAM_END (DT_REG_ADDR(DT_NODELABEL(mram1x)) + DT_REG_SIZE(DT_NODELABEL(mram1x)))
#endif

/* TODO: should it be independent of the code partition (?)
 * It should be set to the protectedmem region ideally.
 */
#define BOOT_PARTITION_END                                                                         \
	(DT_REG_ADDR(DT_CHOSEN(zephyr_code_partition)) +                                           \
	 DT_REG_SIZE(DT_CHOSEN(zephyr_code_partition)))

/* Address ranges of the "active" images when booting slot 0.
 * We define:
 * - "active 0" as the slot 0 image with the lowest address.
 * - "active 1" as the slot 0 image with the highest address (if it exists).
 */
#define SLOT0_ACTIVE0_START MIN_START_ADDR(slot0_partition, slot2_partition)
#define SLOT0_ACTIVE0_END   MAX_END_ADDR(slot0_partition, slot2_partition)
#if MCUBOOT_IMAGE_NUMBER > 1
#define SLOT0_ACTIVE1_START MIN_START_ADDR(slot0_partition, slot2_partition)
#define SLOT0_ACTIVE1_END   MAX_END_ADDR(slot0_partition, slot2_partition)
#endif

/* Address ranges of the "active" images when booting slot 1.
 * We define:
 * - "active 0" as the slot 1 image with the lowest address.
 * - "active 1" as the slot 1 image with the highest address (if it exists) .
 */
#define SLOT1_ACTIVE0_START MIN_START_ADDR(slot1_partition, slot3_partition)
#define SLOT1_ACTIVE0_END   MAX_END_ADDR(slot1_partition, slot3_partition)
#if MCUBOOT_IMAGE_NUMBER > 1
#define SLOT1_ACTIVE1_START MIN_START_ADDR(slot1_partition, slot3_partition)
#define SLOT1_ACTIVE1_END   MAX_END_ADDR(slot1_partition, slot3_partition)
#endif

/* The MPC override setup assumes a partition layout as described below.
 * '.' represents a possible space between the adjacent partitions.
 *
 * TODO
 *
 * |-------------------------------------------------------|
 * | MRAM                                                  |
 * | Bootloader | . |  | Slot 1 | "free" | SECURESTORAGE |
 * |------------|---|-------|--------|--------|---------------|
 *
 The following MPC110 overrides are used to provide write protection for the firmware partitions.
 * The goal of the override configuration is to disable write access to the active firmware regions
 * (bootloader + images contained in the slot) and keep write access enabled for other parts of
 * MRAM.
 *
 * Override 1: R_X | [boot partition start - active 1 partition  end]
 * Override 2: RWX | [boot partition end - active 0 partition start]
 * Override 6: RWX | [active 0 end - active 1 start]
 * Override 7: RWX | [active 1 end - end of accessible MRAM]
 *
 * The effect of this configuration is that the
 */
#define REGPTR_OVERRIDE_RX                                  (uintptr_t)&NRF_MPC110->OVERRIDE[1]
#define REGPTR_OVERRIDE_END_PROTECTEDMEM_START_ACTIVE0      (uintptr_t)&NRF_MPC110->OVERRIDE[2]
#define REGPTR_OVERRIDE_END_ACTIVE0_START_ACTIVE1_IDX       (uintptr_t)&NRF_MPC110->OVERRIDE[6]
#define REGPTR_OVERRIDE_END_ACTIVE1_END_ACCESSIBLE_MRAM_IDX (uintptr_t)&NRF_MPC110->OVERRIDE[7]

/* MPC override configuration set in UICR MPCCONF, which is loaded before MCUboot is started.
 * This configuration is overrwritten again by MCUboot before jumping to the application, so
 * it is only active between when MCUboot is started at cold boot and before jumping to the
 * application. The goal of this configuration is to write protect the bootloader code partition
 * from before the Application core is booted by IronSide SE:
 *
 * Override 1: R_X | [boot partition start - boot partition end]
 * Override 2: RW  | [boot partition end - end of accessible MRAM]
 */
static const struct mpcconf_entry uicr_entries[] __used Z_GENERIC_DOT_SECTION(mpcconf_entry) = {
	/* R_X | [boot start - boot end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ false,
					    /* ENABLE */ true, REGPTR_OVERRIDE_RX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ false,
					    /* X */ true,
					    /* S */ false, ACCESSIBLE_MRAM_START),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, BOOT_PARTITION_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#if ACCESSIBLE_MRAM_END > BOOT_PARTITION_END
	/* RW | [boot end - user end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ false, /* ENABLE */ true,
					    REGPTR_OVERRIDE_END_ACTIVE1_END_ACCESSIBLE_MRAM_IDX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ false,
					    /* S */ false, BOOT_PARTITION_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, ACCESSIBLE_MRAM_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
	/* List terminator entry - IronSide SE stops processing MPCCONF once it encounters this. */
	{
		0xFFFFFFFF,
		0xFFFFFFFF,
		0xFFFFFFFF,
		0xFFFFFFFF,
	},
};

#if MCUBOOT_IMAGE_NUMBER > 1
#define SLOT0_RX_END SLOT0_ACTIVE1_END
#define SLOT1_RX_END SLOT1_ACTIVE1_END
#else
#define SLOT0_RX_END SLOT0_ACTIVE0_END
#define SLOT1_RX_END SLOT1_ACTIVE0_END
#endif

/* MPC override configuration set before booting slot 0.
 * The MPC override setup assumes a partition layout as described below.
 * '.' represents a possible space between the adjacent partitions.
 *
 * TODO
 *
 * |-------------------------------------------------------|
 * | MRAM                                                  |
 * | Bootloader | . |  | Slot 1 | "free" | SECURESTORAGE |
 * |------------|---|-------|--------|--------|---------------|
 *
 * The following MPC110 overrides are used to provide write protection for the firmware partitions.
 * The goal of the override configuration is to disable write access to the active firmware regions
 * (bootloader + images contained in the slot) and keep write access enabled for other parts of
 * MRAM.
 *
 * Override 1: R_X | [boot partition start - active 1 partition  end]
 * Override 2: RWX | [boot partition end - active 0 partition start]
 * Override 6: RWX | [active 0 end - active 1 start]
 * Override 7: RWX | [active 1 end - end of accessible MRAM]
 *
 * The effect of this configuration is that the
 */
static const struct mpcconf_entry slot0_entries[] = {
	/* R_X | [boot start - active 1 end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ true,
					    /* ENABLE */ true, REGPTR_OVERRIDE_RX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ false,
					    /* X */ true,
					    /* S */ false, ACCESSIBLE_MRAM_START),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, SLOT0_RX_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#if SLOT0_ACTIVE0_START > BOOT_PARTITION_END
	/* RWX | [boot end - active 0 start] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true, /* ENABLE */ true,
			REGPTR_OVERRIDE_END_PROTECTEDMEM_START_ACTIVE0),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, BOOT_PARTITION_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, SLOT0_ACTIVE0_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#if MCUBOOT_IMAGE_NUMBER > 1
#if SLOT0_ACTIVE1_START > SLOT0_ACTIVE0_END
	/* RWX | [active 0 end - active 1 start] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ true,
					    /* ENABLE */ true,
					    REGPTR_OVERRIDE_END_ACTIVE0_START_ACTIVE1_IDX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, SLOT0_ACTIVE0_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, SLOT0_ACTIVE1_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#endif
#if ACCESSIBLE_MRAM_END > SLOT0_RX_END
	/* RWX | [active 1 end - user end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ true, /* ENABLE */ true,
					    REGPTR_OVERRIDE_END_ACTIVE1_END_ACCESSIBLE_MRAM_IDX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, SLOT0_RX_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, ACCESSIBLE_MRAM_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
};

#if defined(MCUBOOT_DIRECT_XIP)
/* MPC override configuration set before booting slot 0.
 * The MPC override setup assumes a partition layout as described below.
 * '.' represents a possible space between the adjacent partitions.
 *
 * TODO
 *
 * |-------------------------------------------------------|
 * | MRAM                                                  |
 * | Bootloader | . |  | Slot 1 | "free" | SECURESTORAGE |
 * |------------|---|-------|--------|--------|---------------|
 *
 The following MPC110 overrides are used to provide write protection for the firmware partitions.
 * The goal of the override configuration is to disable write access to the active firmware regions
 * (bootloader + images contained in the slot) and keep write access enabled for other parts of
 * MRAM.
 *
 * Override 1: R_X | [boot partition start - active 1 partition  end]
 * Override 2: RWX | [boot partition end - active 0 partition start]
 * Override 6: RWX | [active 0 end - active 1 start]
 * Override 7: RWX | [active 1 end - end of accessible MRAM]
 *
 * The effect of this configuration is that the
 */
static const struct mpcconf_entry slot1_entries[] = {
	/* R_X | [boot start - active 1 end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ true, /* ENABLE */ true, REGPTR_OVERRIDE_RX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ false,
					    /* X */ true,
					    /* S */ false, ACCESSIBLE_MRAM_START),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, SLOT1_RX_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#if SLOT1_ACTIVE0_START > BOOT_PARTITION_END
	/* RWX | [boot end - active 0 start] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true, /* ENABLE */ true,
			REGPTR_OVERRIDE_END_PROTECTEDMEM_START_ACTIVE0),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, BOOT_PARTITION_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, SLOT1_ACTIVE0_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#if MCUBOOT_IMAGE_NUMBER > 1
#if SLOT1_ACTIVE1_START > SLOT1_ACTIVE0_END
	/* RWX | [active 0 end - active 1 start] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true,
			/* ENABLE */ true, REGPTR_OVERRIDE_END_ACTIVE0_START_ACTIVE1_IDX),
		MPCCONF_ENTRY_CONFIG1_VALUE(
			/* R */ true, /* W */ true, /* X */ true,
			/* S */ false, SLOT1_ACTIVE0_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, SLOT1_ACTIVE1_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#endif
#if ACCESSIBLE_MRAM_END > SLOT1_RX_END
	/* RWX | [active 1 end - user end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true, /* ENABLE */ true,
			REGPTR_OVERRIDE_END_ACTIVE1_END_ACCESSIBLE_MRAM_IDX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, SLOT1_RX_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(NRF_OWNER_NONE, ACCESSIBLE_MRAM_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
};
#endif

#ifdef MCUBOOT_DIRECT_XIP
/* Slot to load MPCCONF for. */
static enum boot_slot mpcconf_slot = BOOT_SLOT_PRIMARY;

int nrf_load_mpcconf_update_active_slot(const struct boot_rsp *rsp)
{
	int rc;
	uintptr_t flash_base;

	rc = flash_device_base(rsp->br_flash_dev_id, &flash_base);
	if (rc != 0) {
		return -1;
	}

	const uintptr_t abs_addr = flash_base + rsp->br_image_off;

	if (IN_RANGE(abs_addr, SLOT1_ACTIVE0_START, SLOT1_ACTIVE0_END - 1)
#if MCUBOOT_IMAGE_NUMBER > 1
	    || IN_RANGE(abs_addr, SLOT1_ACTIVE1_START, SLOT1_ACTIVE1_END - 1)
#endif
	) {
		mpcconf_slot = BOOT_SLOT_SECONDARY;
	} else {
		mpcconf_slot = BOOT_SLOT_PRIMARY;
	}

	return 0;
}
#endif

int __ramfunc nrf_load_mpcconf(void)
{
	int rc;
	struct ironside_se_mpcconf_status status = {0};

	size_t num_entries;
	const struct mpcconf_entry *entries = NULL;

#ifdef MCUBOOT_DIRECT_XIP
	switch (mpcconf_slot) {
	case BOOT_SLOT_PRIMARY:
		entries = slot0_entries;
		num_entries = ARRAY_SIZE(slot0_entries);
		break;
	case BOOT_SLOT_SECONDARY:
		entries = slot1_entries;
		num_entries = ARRAY_SIZE(slot1_entries);
		break;
	default:
		/* Should not be possible. */
		return -1;
	}
#else
	entries = slot0_entries;
	num_entries = ARRAY_SIZE(slot0_entries);
#endif /* MCUBOOT_DIRECT_XIP */

	status = ironside_se_mpcconf_write(entries, num_entries);

	/* Finalization should be done regardless of the write result. */
	rc = ironside_se_mpcconf_finish_init();

	if (rc != 0 || status.status != 0) {
		return -1;
	}

	return 0;
}
