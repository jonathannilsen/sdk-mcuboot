#pragma once

#include <nrfx.h>
#include <ironside/se/mpcconf.h>

#ifndef NRF_MPC110
#define NRF_MPC110 ((NRF_MPC_Type *)0x5F081000)
#endif

#define MASTERPORT_DEFAULT (BIT(2) | BIT(3) | BIT(6))

/** @brief Encode config0 (LOCK, ENABLE, REGPTR).
 *
 * @param _lock If non-zero, set LOCK.
 * @param _enable If non-zero, set ENABLE.
 * @param _regptr Register pointer (stored in bits 4..31).
 */
#define MPCCONF_ENTRY_CONFIG0_VALUE(_lock, _enable, _regptr)                                       \
	(uint32_t)((((_lock) ? MPCCONF_ENTRY_CONFIG0_LOCK_Msk : 0UL)) |                            \
		   (((_enable) ? MPCCONF_ENTRY_CONFIG0_ENABLE_Msk : 0UL)) |                        \
		   ((_regptr) & MPCCONF_ENTRY_CONFIG0_REGPTR_Msk))

/** @brief Encode config1 (READ, WRITE, EXECUTE, SECATTR, STARTADDR).
 *
 * @param _read If non-zero, set READ.
 * @param _write If non-zero, set WRITE.
 * @param _execute If non-zero, set EXECUTE.
 * @param _secattr If non-zero, set SECATTR.
 * @param _startaddr STARTADDR field (bits 5..31).
 */
#define MPCCONF_ENTRY_CONFIG1_VALUE(_read, _write, _execute, _secattr, _startaddr)                 \
	(uint32_t)(((_read) ? MPCCONF_ENTRY_CONFIG1_READ_Msk : 0UL) |                              \
		   ((_write) ? MPCCONF_ENTRY_CONFIG1_WRITE_Msk : 0UL) |                            \
		   ((_execute) ? MPCCONF_ENTRY_CONFIG1_EXECUTE_Msk : 0UL) |                        \
		   ((_secattr) ? MPCCONF_ENTRY_CONFIG1_SECATTR_Msk : 0UL) |                        \
		   ((_startaddr) & MPCCONF_ENTRY_CONFIG1_STARTADDR_Msk))

/** @brief Encode config2 (OWNERID, ENDADDR_OR_MASK).
 *
 * @param _ownerid OWNERID field (bits 0..3).
 * @param _endaddr_or_mask ENDADDR_OR_MASK field (bits 5..31).
 */
#define MPCCONF_ENTRY_CONFIG2_VALUE(_ownerid, _endaddr_or_mask)                                    \
	(uint32_t)((((_ownerid) << MPCCONF_ENTRY_CONFIG2_OWNERID_Pos) &                            \
		    MPCCONF_ENTRY_CONFIG2_OWNERID_Msk) |                                           \
		   ((_endaddr_or_mask) & MPCCONF_ENTRY_CONFIG2_ENDADDR_OR_MASK_Msk))

/** @brief Encode config3 (MASTERPORT).
 *
 * @param _masterport MASTERPORT field value.
 */
#define MPCCONF_ENTRY_CONFIG3_VALUE(_masterport)                                                   \
	(uint32_t)(((_masterport) << MPCCONF_ENTRY_CONFIG3_MASTERPORT_Pos) &                       \
		   MPCCONF_ENTRY_CONFIG3_MASTERPORT_Msk)
