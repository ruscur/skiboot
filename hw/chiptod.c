/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Handle ChipTOD chip & configure core timebases
 */
#include <skiboot.h>
#include <chiptod.h>
#include <chip.h>
#include <xscom.h>
#include <io.h>
#include <cpu.h>
#include <timebase.h>
#include <opal-api.h>

/* -- TOD primary/secondary master/slave control register -- */
#define TOD_PSMS_CTRL			0x00040007
#define  TOD_PSMSC_PM_TOD_SELECT	PPC_BIT(1)  /* Primary Master TOD */
#define  TOD_PSMSC_PM_DRAW_SELECT	PPC_BIT(2)  /* Primary Master Drawer */
#define  TOD_PSMSC_SM_TOD_SELECT	PPC_BIT(9)  /* Secondary Master TOD */
#define  TOD_PSMSC_SM_DRAW_SELECT	PPC_BIT(10) /* Secondary Master Draw */

/* -- TOD primary/secondary master/slave status register -- */
#define TOD_STATUS			0x00040008
#define   TOD_ST_TOPOLOGY_SELECT	PPC_BITMASK(0, 2)
#define   TOD_ST_MPATH0_STEP_VALID	PPC_BIT(6)  /* MasterPath0 step valid */
#define   TOD_ST_MPATH1_STEP_VALID	PPC_BIT(7)  /* MasterPath1 step valid */
#define   TOD_ST_SPATH0_STEP_VALID	PPC_BIT(8)  /* SlavePath0 step valid */
#define   TOD_ST_SPATH1_STEP_VALID	PPC_BIT(10) /* SlavePath1 step valid */
/* Primary master/slave path select (0 = PATH_0, 1 = PATH_1) */
#define   TOD_ST_PRI_MPATH_SELECT	PPC_BIT(12) /* Primary MPath Select */
#define   TOD_ST_PRI_SPATH_SELECT	PPC_BIT(15) /* Primary SPath Select */
/* Secondary master/slave path select (0 = PATH_0, 1 = PATH_1) */
#define   TOD_ST_SEC_MPATH_SELECT	PPC_BIT(16) /* Secondary MPath Select */
#define   TOD_ST_SEC_SPATH_SELECT	PPC_BIT(19) /* Secondary SPath Select */
#define   TOD_ST_ACTIVE_MASTER		PPC_BIT(23)
#define   TOD_ST_BACKUP_MASTER		PPC_BIT(24)

/* TOD chip XSCOM addresses */
#define TOD_TTYPE_0			0x00040011
#define TOD_TTYPE_1			0x00040012 /* PSS switch */
#define TOD_TTYPE_2			0x00040013 /* Enable step checkers */
#define TOD_TTYPE_3			0x00040014 /* Request TOD */
#define TOD_TTYPE_4			0x00040015 /* Send TOD */
#define TOD_TTYPE_5			0x00040016 /* Invalidate TOD */
#define TOD_CHIPTOD_TO_TB		0x00040017
#define TOD_LOAD_TOD_MOD		0x00040018
#define TOD_CHIPTOD_VALUE		0x00040020
#define TOD_CHIPTOD_LOAD_TB		0x00040021
#define TOD_CHIPTOD_FSM			0x00040024

/* -- TOD PIB Master reg -- */
#define TOD_PIB_MASTER			0x00040027
#define   TOD_PIBM_ADDR_CFG_MCAST	PPC_BIT(25)
#define   TOD_PIBM_ADDR_CFG_SLADDR	PPC_BITMASK(26,31)

/* -- TOD Error interrupt register -- */
#define TOD_ERROR			0x00040030
/* SYNC errors */
#define   TOD_ERR_CRMO_PARITY		PPC_BIT(0)
#define   TOD_ERR_OSC0_PARITY		PPC_BIT(1)
#define   TOD_ERR_OSC1_PARITY		PPC_BIT(2)
#define   TOD_ERR_CRITC_PARITY		PPC_BIT(13)
#define   TOD_ERR_PSS_HAMMING_DISTANCE	PPC_BIT(18)
#define	  TOD_ERR_DELAY_COMPL_PARITY	PPC_BIT(22)
/* CNTR errors */
#define   TOD_ERR_CTCR_PARITY		PPC_BIT(32)
#define   TOD_ERR_TOD_SYNC_CHECK	PPC_BIT(33)
#define   TOD_ERR_TOD_FSM_PARITY	PPC_BIT(34)
#define   TOD_ERR_TOD_REGISTER_PARITY	PPC_BIT(35)
#define   TOD_ERR_OVERFLOW_YR2042	PPC_BIT(36)
#define   TOD_ERR_TOD_WOF_LSTEP_PARITY	PPC_BIT(37)
#define   TOD_ERR_TTYPE0_RECVD		PPC_BIT(38)
#define   TOD_ERR_TTYPE1_RECVD		PPC_BIT(39)
#define   TOD_ERR_TTYPE2_RECVD		PPC_BIT(40)
#define   TOD_ERR_TTYPE3_RECVD		PPC_BIT(41)
#define   TOD_ERR_TTYPE4_RECVD		PPC_BIT(42)
#define   TOD_ERR_TTYPE5_RECVD		PPC_BIT(43)

/* Magic TB value. One step cycle ahead of sync */
#define INIT_TB	0x000000000001ff0

/* Number of iterations for the various timeouts */
#define TIMEOUT_LOOPS		20000000

/* TOD active Primary/secondary configuration */
#define TOD_PRI_CONF_IN_USE	0	/* Tod using primary topology*/
#define TOD_SEC_CONF_IN_USE	7	/* Tod using secondary topo */

/* Timebase State Machine error state */
#define TBST_STATE_ERROR	9

static enum chiptod_type {
	chiptod_unknown,
	chiptod_p7,
	chiptod_p8
} chiptod_type;

enum chiptod_topology {
	chiptod_topo_unknown = -1,
	chiptod_topo_primary = 0,
	chiptod_topo_secondary = 1,
};

enum chiptod_chip_role {
	chiptod_chip_role_UNKNOWN = -1,
	chiptod_chip_role_MDMT = 0,	/* Master Drawer Master TOD */
	chiptod_chip_role_MDST,		/* Master Drawer Slave TOD */
	chiptod_chip_role_SDMT,		/* Slave Drawer Master TOD */
	chiptod_chip_role_SDST,		/* Slave Drawer Slave TOD */
};

enum chiptod_chip_status {
	chiptod_active_master = 0,	/* Chip TOD is Active master */
	chiptod_backup_master = 1,	/* Chip TOD is backup master */
	chiptod_backup_disabled,	/* Chip TOD is backup but disabled */
};

struct chiptod_chip_config_info {
	int32_t id;				/* chip id */
	enum chiptod_chip_role role;		/* Chip role */
	enum chiptod_chip_status status;	/* active/backup/disabled */
};

static int32_t chiptod_primary = -1;
static int32_t chiptod_secondary = -1;
static enum chiptod_topology current_topology = chiptod_topo_unknown;

/*
 * chiptod_topology_info holds primary/secondary chip configuration info.
 * This info is initialized during chiptod_init(). This is an array of two:
 *	[0] = [chiptod_topo_primary] = Primary topology config info
 *	[1] = [chiptod_topo_secondary] = Secondary topology config info
 */
static struct chiptod_chip_config_info chiptod_topology_info[2];

/* The base TFMR value is the same for the whole machine
 * for now as far as I can tell
 */
static uint64_t base_tfmr;

/*
 * For now, we use a global lock for runtime chiptod operations,
 * eventually make this a per-core lock for wakeup rsync and
 * take all of them for RAS cases.
 */
static struct lock chiptod_lock = LOCK_UNLOCKED;

static void print_topo_info(enum chiptod_topology topo)
{
	const char *role[] = { "Unknown", "MDMT", "MDST", "SDMT", "SDST" };
	const char *status[] = { "Unknown",
		"Active Master", "Backup Master", "Backup Master Disabled" };

	prlog(PR_DEBUG, "CHIPTOD:    chip id: %d, Role: %s, Status: %s\n",
				chiptod_topology_info[topo].id,
				role[chiptod_topology_info[topo].role + 1],
				status[chiptod_topology_info[topo].status + 1]);
}

static void print_topology_info(void)
{
	const char *topo[] = { "Unknown", "Primary", "Secondary" };

	if (current_topology < 0)
		return;

	prlog(PR_DEBUG, "CHIPTOD: TOD Topology in Use: %s\n",
						topo[current_topology+1]);
	prlog(PR_DEBUG, "CHIPTOD:   Primary configuration:\n");
	print_topo_info(chiptod_topo_primary);
	prlog(PR_DEBUG, "CHIPTOD:   Secondary configuration:\n");
	print_topo_info(chiptod_topo_secondary);
}

static enum chiptod_topology query_current_topology(void)
{
	uint64_t tod_status;

	if (xscom_readme(TOD_STATUS, &tod_status) != 0) {
		prerror("CHIPTOD: XSCOM error reading TOD_STATUS reg\n");
		return chiptod_topo_unknown;
	}

	/*
	 * Tod status register bit [0-2] tells configuration in use.
	 *	000 <= primary configuration in use
	 *	111 <= secondary configuration in use
	 */
	if ((tod_status & TOD_ST_TOPOLOGY_SELECT) == TOD_PRI_CONF_IN_USE)
		return chiptod_topo_primary;
	else
		return chiptod_topo_secondary;
}

static enum chiptod_chip_role
chiptod_get_chip_role(enum chiptod_topology topology, int32_t chip_id)
{
	uint64_t tod_ctrl;
	enum chiptod_chip_role role = chiptod_chip_role_UNKNOWN;

	if (chip_id < 0)
		return role;

	if (xscom_read(chip_id, TOD_PSMS_CTRL, &tod_ctrl) != 0) {
		prerror("CHIPTOD: XSCOM error reading TOD_PSMS_CTRL\n");
		return chiptod_chip_role_UNKNOWN;
	}

	switch (topology) {
	case chiptod_topo_primary:
		if (tod_ctrl & TOD_PSMSC_PM_DRAW_SELECT) {
			if (tod_ctrl & TOD_PSMSC_PM_TOD_SELECT)
				role = chiptod_chip_role_MDMT;
			else
				role = chiptod_chip_role_MDST;
		} else {
			if (tod_ctrl & TOD_PSMSC_PM_TOD_SELECT)
				role = chiptod_chip_role_SDMT;
			else
				role = chiptod_chip_role_SDST;
		}
		break;
	case chiptod_topo_secondary:
		if (tod_ctrl & TOD_PSMSC_SM_DRAW_SELECT) {
			if (tod_ctrl & TOD_PSMSC_SM_TOD_SELECT)
				role = chiptod_chip_role_MDMT;
			else
				role = chiptod_chip_role_MDST;
		} else {
			if (tod_ctrl & TOD_PSMSC_SM_TOD_SELECT)
				role = chiptod_chip_role_SDMT;
			else
				role = chiptod_chip_role_SDST;
		}
		break;
	case chiptod_topo_unknown:
	default:
		break;
	}
	return role;
}

/*
 * Check and return the status of sync step network for a given
 * topology configuration.
 * Return values:
 *	true:	Sync Step network is running
 *	false:	Sync Step network is not running
 */
static bool chiptod_sync_step_check_running(enum chiptod_topology topology)
{
	uint64_t tod_status;
	enum chiptod_chip_role role;
	bool running = false;
	int32_t chip_id = chiptod_topology_info[topology].id;

	/* Sanity check */
	if (chip_id < 0)
		return false;

	if (xscom_read(chip_id, TOD_STATUS, &tod_status) != 0) {
		prerror("CHIPTOD: XSCOM error reading TOD_STATUS reg\n");
		return false;
	}

	switch (topology) {
	case chiptod_topo_primary:
		/* Primary configuration */
		role = chiptod_topology_info[topology].role;
		if (role == chiptod_chip_role_MDMT) {
			/*
			 * Chip is using Master path.
			 * Check if it is using path_0/path_1 and then
			 * validity of that path.
			 *
			 * TOD_STATUS[12]: 0 = PATH_0, 1 = PATH_1
			 */
			if (tod_status & TOD_ST_PRI_MPATH_SELECT) {
				if (tod_status & TOD_ST_MPATH1_STEP_VALID)
					running = true;
			} else {
				if (tod_status & TOD_ST_MPATH0_STEP_VALID)
					running = true;
			}
		} else {
			/*
			 * Chip is using Slave path.
			 *
			 * TOD_STATUS[15]: 0 = PATH_0, 1 = PATH_1
			 */
			if (tod_status & TOD_ST_PRI_SPATH_SELECT) {
				if (tod_status & TOD_ST_SPATH1_STEP_VALID)
					running = true;
			} else {
				if (tod_status & TOD_ST_SPATH0_STEP_VALID)
					running = true;
			}
		}
		break;
	case chiptod_topo_secondary:
		/* Secondary configuration */
		role = chiptod_topology_info[topology].role;
		if (role == chiptod_chip_role_MDMT) {
			/*
			 * Chip is using Master path.
			 * Check if it is using path_0/path_1 and then
			 * validity of that path.
			 *
			 * TOD_STATUS[12]: 0 = PATH_0, 1 = PATH_1
			 */
			if (tod_status & TOD_ST_SEC_MPATH_SELECT) {
				if (tod_status & TOD_ST_MPATH1_STEP_VALID)
					running = true;
			} else {
				if (tod_status & TOD_ST_MPATH0_STEP_VALID)
					running = true;
			}
		} else {
			/*
			 * Chip is using Slave path.
			 *
			 * TOD_STATUS[15]: 0 = PATH_0, 1 = PATH_1
			 */
			if (tod_status & TOD_ST_SEC_SPATH_SELECT) {
				if (tod_status & TOD_ST_SPATH1_STEP_VALID)
					running = true;
			} else {
				if (tod_status & TOD_ST_SPATH0_STEP_VALID)
					running = true;
			}
		}
		break;
	default:
		break;
	}
	return running;
}

static enum chiptod_chip_status _chiptod_get_chip_status(int32_t chip_id)
{
	uint64_t tod_status;
	enum chiptod_chip_status status = -1;

	if (xscom_read(chip_id, TOD_STATUS, &tod_status) != 0) {
		prerror("CHIPTOD: XSCOM error reading TOD_STATUS reg\n");
		goto out;
	}

	if (tod_status & TOD_ST_ACTIVE_MASTER)
		status = chiptod_active_master;
	else if (tod_status & TOD_ST_BACKUP_MASTER)
		status = chiptod_backup_master;

out:
	return status;
}

static enum chiptod_chip_status
chiptod_get_chip_status(enum chiptod_topology topology)
{
	return _chiptod_get_chip_status(chiptod_topology_info[topology].id);
}

static void chiptod_update_topology(enum chiptod_topology topo)
{
	int32_t chip_id = chiptod_topology_info[topo].id;

	chiptod_topology_info[topo].role = chiptod_get_chip_role(topo, chip_id);
	chiptod_topology_info[topo].status = chiptod_get_chip_status(topo);

	/*
	 * If chip TOD on this topology is a backup master then check if
	 * sync/step network is running on this topology. If not,
	 * then mark status as backup not valid.
	 */
	if ((chiptod_topology_info[topo].status == chiptod_backup_master) &&
			!chiptod_sync_step_check_running(topo))
		chiptod_topology_info[topo].status = chiptod_backup_disabled;
}

static void chiptod_setup_base_tfmr(void)
{
	struct dt_node *cpu = this_cpu()->node;
	uint64_t core_freq, tod_freq;
	uint64_t mcbs;

	base_tfmr = SPR_TFMR_TB_ECLIPZ;

	/* Get CPU and TOD freqs in Hz */
	if (dt_has_node_property(cpu,"ibm,extended-clock-frequency", NULL))
		core_freq = dt_prop_get_u64(cpu,"ibm,extended-clock-frequency");
	else
		core_freq = dt_prop_get_u32(cpu, "clock-frequency");
	tod_freq = 32000000;

	/* Calculate the "Max Cycles Between Steps" value according
	 * to the magic formula:
	 *
	 * mcbs = (core_freq * max_jitter_factor) / (4 * tod_freq) / 100;
	 *
	 * The max jitter factor is set to 240 based on what pHyp uses.
	 */
	mcbs = (core_freq * 240) / (4 * tod_freq) / 100;
	prlog(PR_INFO, "CHIPTOD: Calculated MCBS is 0x%llx"
	      " (Cfreq=%lld Tfreq=%lld)\n",
	      mcbs, core_freq, tod_freq);

	/* Bake that all into TFMR */
	base_tfmr = SETFIELD(SPR_TFMR_MAX_CYC_BET_STEPS, base_tfmr, mcbs);
	base_tfmr = SETFIELD(SPR_TFMR_N_CLKS_PER_STEP, base_tfmr, 0);
	base_tfmr = SETFIELD(SPR_TFMR_SYNC_BIT_SEL, base_tfmr, 4);
}

static bool chiptod_mod_tb(void)
{
	uint64_t tfmr = base_tfmr;
	uint64_t timeout = 0;

	/* Switch timebase to "Not Set" state */
	mtspr(SPR_TFMR, tfmr | SPR_TFMR_LOAD_TOD_MOD);
	do {
		if (++timeout >= (TIMEOUT_LOOPS*2)) {
			prerror("CHIPTOD: TB \"Not Set\" timeout\n");
			return false;
		}
		tfmr = mfspr(SPR_TFMR);
		if (tfmr & SPR_TFMR_TFMR_CORRUPT) {
			prerror("CHIPTOD: TB \"Not Set\" TFMR corrupt\n");
			return false;
		}
		if (GETFIELD(SPR_TFMR_TBST_ENCODED, tfmr) == 9) {
			prerror("CHIPTOD: TB \"Not Set\" TOD in error state\n");
			return false;
		}
	} while(tfmr & SPR_TFMR_LOAD_TOD_MOD);

	return true;
}

static bool chiptod_interrupt_check(void)
{
	uint64_t tfmr = mfspr(SPR_TFMR);
	uint64_t timeout = 0;

	do {
		if (++timeout >= TIMEOUT_LOOPS) {
			prerror("CHIPTOD: Interrupt check fail\n");
			return false;
		}
		tfmr = mfspr(SPR_TFMR);
		if (tfmr & SPR_TFMR_TFMR_CORRUPT) {
			prerror("CHIPTOD: Interrupt check TFMR corrupt !\n");
			return false;
		}
	} while(tfmr & SPR_TFMR_CHIP_TOD_INTERRUPT);

	return true;
}

static bool chiptod_running_check(void)
{
	uint64_t tval;

	if (xscom_readme(TOD_CHIPTOD_FSM, &tval) != 0) {
		prerror("CHIPTOD: XSCOM error polling run\n");
		return false;
	}
	if (tval & 0x0800000000000000UL)
		return true;
	else
		return false;
}

static bool chiptod_poll_running(void)
{
	uint64_t timeout = 0;
	uint64_t tval;

	/* Chip TOD running check */
	do {
		if (++timeout >= TIMEOUT_LOOPS) {
			prerror("CHIPTOD: Running check fail timeout\n");
			return false;
		}
		if (xscom_readme(TOD_CHIPTOD_FSM, &tval) != 0) {
			prerror("CHIPTOD: XSCOM error polling run\n");
			return false;
		}
	} while(!(tval & 0x0800000000000000UL));

	return true;
}

static bool chiptod_to_tb(void)
{
	uint64_t tval, tfmr, tvbits;
	uint64_t timeout = 0;

	/* Tell the ChipTOD about our fabric address
	 *
	 * The pib_master value is calculated from the CPU core ID, given in
	 * the PIR. Because we have different core/thread arrangements in the
	 * PIR between p7 and p8, we need to do the calculation differently.
	 *
	 * p7: 0b00001 || 3-bit core id
	 * p8: 0b0001 || 4-bit core id
	 */

	if (xscom_readme(TOD_PIB_MASTER, &tval) != 0) {
		prerror("CHIPTOD: XSCOM error reading PIB_MASTER\n");
		return false;
	}
	if (chiptod_type == chiptod_p8) {
		tvbits = (this_cpu()->pir >> 3) & 0xf;
		tvbits |= 0x10;
	} else {
		tvbits = (this_cpu()->pir >> 2) & 0x7;
		tvbits |= 0x08;
	}
	tval &= ~TOD_PIBM_ADDR_CFG_MCAST;
	tval = SETFIELD(TOD_PIBM_ADDR_CFG_SLADDR, tval, tvbits);
	if (xscom_writeme(TOD_PIB_MASTER, tval) != 0) {
		prerror("CHIPTOD: XSCOM error writing PIB_MASTER\n");
		return false;
	}

	/* Make us ready to get the TB from the chipTOD */
	mtspr(SPR_TFMR, base_tfmr | SPR_TFMR_MOVE_CHIP_TOD_TO_TB);

	/* Tell the ChipTOD to send it */
	if (xscom_writeme(TOD_CHIPTOD_TO_TB, (1ULL << 63)) != 0) {
		prerror("CHIPTOD: XSCOM error writing CHIPTOD_TO_TB\n");
		return false;
	}

	/* Wait for it to complete */
	timeout = 0;
	do {
		if (++timeout >= TIMEOUT_LOOPS) {
			prerror("CHIPTOD: Chip to TB timeout\n");
			return false;
		}
		tfmr = mfspr(SPR_TFMR);
		if (tfmr & SPR_TFMR_TFMR_CORRUPT) {
			prerror("CHIPTOD: MoveToTB: corrupt TFMR !\n");
			return false;
		}
	} while(tfmr & SPR_TFMR_MOVE_CHIP_TOD_TO_TB);

	return true;
}

static bool chiptod_check_tb_running(void)
{
	/* We used to wait for two SYNC pulses in TFMR but that
	 * doesn't seem to occur in sim, so instead we use a
	 * method similar to what pHyp does which is to check for
	 * TFMR SPR_TFMR_TB_VALID and not SPR_TFMR_TFMR_CORRUPT
	 */
#if 0
	uint64_t tfmr, timeout;
	unsigned int i;

	for (i = 0; i < 2; i++) {
		tfmr = mfspr(SPR_TFMR);
		tfmr &= ~SPR_TFMR_TB_SYNC_OCCURED;
		mtspr(SPR_TFMR, tfmr);
		timeout = 0;
		do {
			if (++timeout >= TIMEOUT_LOOPS) {
				prerror("CHIPTOD: No sync pulses\n");
				return false;
			}
			tfmr = mfspr(SPR_TFMR);
		} while(!(tfmr & SPR_TFMR_TB_SYNC_OCCURED));
	}
#else
	uint64_t tfmr = mfspr(SPR_TFMR);

	return (tfmr & SPR_TFMR_TB_VALID) &&
		!(tfmr & SPR_TFMR_TFMR_CORRUPT);
#endif
	return true;
}

static bool chiptod_reset_tb_errors(void)
{
	uint64_t tfmr;
	unsigned long timeout = 0;

	/* Ask for automatic clear of errors */
	tfmr = base_tfmr | SPR_TFMR_CLEAR_TB_ERRORS;

	/* Additionally pHyp sets these (write-1-to-clear ?) */
	tfmr |= SPR_TFMR_TB_MISSING_SYNC;
	tfmr |= SPR_TFMR_TB_MISSING_STEP;
	tfmr |= SPR_TFMR_TB_RESIDUE_ERR;
	mtspr(SPR_TFMR, tfmr);

	/* We have to write "Clear TB Errors" again */
	tfmr = base_tfmr | SPR_TFMR_CLEAR_TB_ERRORS;
	mtspr(SPR_TFMR, tfmr);

	do {
		if (++timeout >= TIMEOUT_LOOPS) {
			/* Don't actually do anything on error for
			 * now ... not much we can do, panic maybe ?
			 */
			prerror("CHIPTOD: TB error reset timeout !\n");
			return false;
		}
		tfmr = mfspr(SPR_TFMR);
		if (tfmr & SPR_TFMR_TFMR_CORRUPT) {
			prerror("CHIPTOD: TB error reset: corrupt TFMR !\n");
			return false;
		}
	} while(tfmr & SPR_TFMR_CLEAR_TB_ERRORS);
	return true;
}

static void chiptod_cleanup_thread_tfmr(void)
{
	uint64_t tfmr = base_tfmr;

	tfmr |= SPR_TFMR_PURR_PARITY_ERR;
	tfmr |= SPR_TFMR_SPURR_PARITY_ERR;
	tfmr |= SPR_TFMR_DEC_PARITY_ERR;
	tfmr |= SPR_TFMR_TFMR_CORRUPT;
	tfmr |= SPR_TFMR_PURR_OVERFLOW;
	tfmr |= SPR_TFMR_SPURR_OVERFLOW;
	mtspr(SPR_TFMR, tfmr);
}

static void chiptod_reset_tod_errors(void)
{
	uint64_t terr;

	/*
	 * At boot, we clear the errors that the firmware is
	 * supposed to handle. List provided by the pHyp folks.
	 */
	
	terr = TOD_ERR_CRITC_PARITY;
	terr |= TOD_ERR_PSS_HAMMING_DISTANCE;
	terr |= TOD_ERR_DELAY_COMPL_PARITY;
	terr |= TOD_ERR_CTCR_PARITY;
	terr |= TOD_ERR_TOD_SYNC_CHECK;
	terr |= TOD_ERR_TOD_FSM_PARITY;
	terr |= TOD_ERR_TOD_REGISTER_PARITY;

	if (xscom_writeme(TOD_ERROR, terr) != 0) {
		prerror("CHIPTOD: XSCOM error writing TOD_ERROR !\n");
		/* Not much we can do here ... abort ? */
	}
}

static void chiptod_sync_master(void *data)
{
	bool *result = data;

	prlog(PR_DEBUG, "CHIPTOD: Master sync on CPU PIR 0x%04x...\n",
	      this_cpu()->pir);

	/* Apply base tfmr */
	mtspr(SPR_TFMR, base_tfmr);

	/* From recipe provided by pHyp folks, reset various errors
	 * before attempting the sync
	 */
	chiptod_reset_tb_errors();

	/* Cleanup thread tfmr bits */
	chiptod_cleanup_thread_tfmr();

	/* Reset errors in the chiptod itself */
	chiptod_reset_tod_errors();

	/* Switch timebase to "Not Set" state */
	if (!chiptod_mod_tb())
		goto error;
	prlog(PR_INSANE, "SYNC MASTER Step 2 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	/* Chip TOD step checkers enable */
	if (xscom_writeme(TOD_TTYPE_2, (1UL << 63)) != 0) {
		prerror("CHIPTOD: XSCOM error enabling steppers\n");
		goto error;
	}

	prlog(PR_INSANE, "SYNC MASTER Step 3 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	/* Chip TOD interrupt check */
	if (!chiptod_interrupt_check())
		goto error;	
	prlog(PR_INSANE, "SYNC MASTER Step 4 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	/* Switch local chiptod to "Not Set" state */
	if (xscom_writeme(TOD_LOAD_TOD_MOD, (1UL << 63)) != 0) {
		prerror("CHIPTOD: XSCOM error sending LOAD_TOD_MOD\n");
		goto error;
	}

	/* Switch all remote chiptod to "Not Set" state */
	if (xscom_writeme(TOD_TTYPE_5, (1UL << 63)) != 0) {
		prerror("CHIPTOD: XSCOM error sending TTYPE_5\n");
		goto error;
	}

	/* Chip TOD load initial value */
	if (xscom_writeme(TOD_CHIPTOD_LOAD_TB, INIT_TB) != 0) {
		prerror("CHIPTOD: XSCOM error setting init TB\n");
		goto error;
	}

	prlog(PR_INSANE, "SYNC MASTER Step 5 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	if (!chiptod_poll_running())
		goto error;
	prlog(PR_INSANE, "SYNC MASTER Step 6 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	/* Move chiptod value to core TB */
	if (!chiptod_to_tb())
		goto error;
	prlog(PR_INSANE, "SYNC MASTER Step 7 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	/* Send local chip TOD to all chips TOD */
	if (xscom_writeme(TOD_TTYPE_4, (1ULL << 63)) != 0) {
		prerror("CHIPTOD: XSCOM error sending TTYPE_4\n");
		goto error;
	}

	/* Check if TB is running */
	if (!chiptod_check_tb_running())
		goto error;

	prlog(PR_INSANE, "Master sync completed, TB=%lx\n", mfspr(SPR_TBRL));

	/*
	 * A little delay to make sure the remote chips get up to
	 * speed before we start syncing them.
	 *
	 * We have to do it here because we know our TB is running
	 * while the boot thread TB might not yet.
	 */
	time_wait_ms(1);

	*result = true;
	return;
 error:
	prerror("CHIPTOD: Master sync failed! TFMR=0x%016lx\n",
		mfspr(SPR_TFMR));
	*result = false;
}

static void chiptod_sync_slave(void *data)
{
	bool *result = data;

	/* Only get primaries, not threads */
	if (this_cpu()->is_secondary) {
		/* On secondaries we just cleanup the TFMR */
		chiptod_cleanup_thread_tfmr();
		*result = true;
		return;
	}

	prlog(PR_DEBUG, "CHIPTOD: Slave sync on CPU PIR 0x%04x...\n",
	      this_cpu()->pir);

	/* Apply base tfmr */
	mtspr(SPR_TFMR, base_tfmr);

	/* From recipe provided by pHyp folks, reset various errors
	 * before attempting the sync
	 */
	chiptod_reset_tb_errors();

	/* Cleanup thread tfmr bits */
	chiptod_cleanup_thread_tfmr();

	/* Switch timebase to "Not Set" state */
	if (!chiptod_mod_tb())
		goto error;
	prlog(PR_INSANE, "SYNC SLAVE Step 2 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	/* Chip TOD running check */
	if (!chiptod_poll_running())
		goto error;
	prlog(PR_INSANE, "SYNC SLAVE Step 3 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	/* Chip TOD interrupt check */
	if (!chiptod_interrupt_check())
		goto error;
	prlog(PR_INSANE, "SYNC SLAVE Step 4 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	/* Move chiptod value to core TB */
	if (!chiptod_to_tb())
		goto error;
	prlog(PR_INSANE, "SYNC SLAVE Step 5 TFMR=0x%016lx\n", mfspr(SPR_TFMR));

	/* Check if TB is running */
	if (!chiptod_check_tb_running())
		goto error;

	prlog(PR_INSANE, "Slave sync completed, TB=%lx\n", mfspr(SPR_TBRL));

	*result = true;
	return;
 error:
	prerror("CHIPTOD: Slave sync failed ! TFMR=0x%016lx\n",
		mfspr(SPR_TFMR));
	*result = false;
}

bool chiptod_wakeup_resync(void)
{
	if (chiptod_primary < 0)
		return 0;

	lock(&chiptod_lock);

	/* Apply base tfmr */
	mtspr(SPR_TFMR, base_tfmr);

	/* From recipe provided by pHyp folks, reset various errors
	 * before attempting the sync
	 */
	chiptod_reset_tb_errors();

	/* Cleanup thread tfmr bits */
	chiptod_cleanup_thread_tfmr();

	/* Switch timebase to "Not Set" state */
	if (!chiptod_mod_tb())
		goto error;

	/* Move chiptod value to core TB */
	if (!chiptod_to_tb())
		goto error;

	unlock(&chiptod_lock);

	return true;
 error:
	prerror("CHIPTOD: Resync failed ! TFMR=0x%16lx\n", mfspr(SPR_TFMR));
	unlock(&chiptod_lock);
	return false;
}

static int chiptod_recover_tod_errors(void)
{
	uint64_t terr;

	/* Read TOD error register */
	if (xscom_readme(TOD_ERROR, &terr) != 0) {
		prerror("CHIPTOD: XSCOM error reading TOD_ERROR reg\n");
		return 0;
	}
	/* Check for sync check error and recover */
	if ((terr & TOD_ERR_TOD_SYNC_CHECK) ||
		(terr & TOD_ERR_TOD_FSM_PARITY) ||
		(terr & TOD_ERR_CTCR_PARITY) ||
		(terr & TOD_ERR_PSS_HAMMING_DISTANCE) ||
		(terr & TOD_ERR_DELAY_COMPL_PARITY) ||
		(terr & TOD_ERR_TOD_REGISTER_PARITY)) {
		chiptod_reset_tod_errors();
		return 1;
	}
	return 0;
}

/*
 * Sync up TOD with other chips and get TOD in running state.
 * For non-master, we request TOD value from another chip.
 * For master chip, Switch the topology to recover.
 */
static int chiptod_start_tod(void)
{
	/*  Handle TOD recovery on master chip. */
	if (this_cpu()->chip_id == chiptod_primary) {
		/*
		 * TOD is not running on master chip. We need to sync with
		 * secondary chip TOD. But before we do that we need to
		 * switch topology to make backup master as the new
		 * active master. Once we switch the topology we can
		 * then request TOD value from new master chip TOD.
		 * But make sure we move local chiptod to Not Set before
		 * request TOD value.
		 */
		if (xscom_writeme(TOD_TTYPE_1, (1UL << 63)) != 0) {
			prerror("CHIPTOD: XSCOM error switching primary/secondary\n");
			return 0;
		}
		chiptod_primary = chiptod_secondary;
		chiptod_secondary = this_cpu()->chip_id;
	}

	/* Switch local chiptod to "Not Set" state */
	if (xscom_writeme(TOD_LOAD_TOD_MOD, (1UL << 63)) != 0) {
		prerror("CHIPTOD: XSCOM error sending LOAD_TOD_MOD\n");
		return 0;
	}

	/*
	 * Request the current TOD value from another chip.
	 * This will move TOD in running state
	 */
	if (xscom_writeme(TOD_TTYPE_3, (1UL << 63)) != 0) {
		prerror("CHIPTOD: XSCOM error sending TTYPE_3\n");
		return 0;
	}

	/* Check if chip TOD is running. */
	if (!chiptod_poll_running())
		return 0;

	return 1;
}

static bool tfmr_recover_tb_errors(uint64_t tfmr)
{
	uint64_t tfmr_reset_error;
	unsigned long timeout = 0;

	/* Ask for automatic clear of errors */
	tfmr_reset_error = base_tfmr | SPR_TFMR_CLEAR_TB_ERRORS;

	/* Additionally pHyp sets these (write-1-to-clear ?) */
	if (tfmr & SPR_TFMR_TB_MISSING_SYNC)
		tfmr_reset_error |= SPR_TFMR_TB_MISSING_SYNC;

	if (tfmr & SPR_TFMR_TB_MISSING_STEP)
		tfmr_reset_error |= SPR_TFMR_TB_MISSING_STEP;

	/*
	 * write 1 to bit 45 to clear TB residue the error.
	 * TB register has already been reset to zero as part pre-recovery.
	 */
	if (tfmr & SPR_TFMR_TB_RESIDUE_ERR)
		tfmr_reset_error |= SPR_TFMR_TB_RESIDUE_ERR;

	if (tfmr & SPR_TFMR_FW_CONTROL_ERR)
		tfmr_reset_error |= SPR_TFMR_FW_CONTROL_ERR;

	if (tfmr & SPR_TFMR_TBST_CORRUPT)
		tfmr_reset_error |= SPR_TFMR_TBST_CORRUPT;

	mtspr(SPR_TFMR, tfmr_reset_error);

	/* We have to write "Clear TB Errors" again */
	tfmr_reset_error = base_tfmr | SPR_TFMR_CLEAR_TB_ERRORS;
	mtspr(SPR_TFMR, tfmr_reset_error);

	do {
		if (++timeout >= TIMEOUT_LOOPS) {
			prerror("CHIPTOD: TB error reset timeout !\n");
			return false;
		}
		tfmr = mfspr(SPR_TFMR);
		if (tfmr & SPR_TFMR_TFMR_CORRUPT) {
			prerror("CHIPTOD: TB error reset: corrupt TFMR !\n");
			return false;
		}
	} while (tfmr & SPR_TFMR_CLEAR_TB_ERRORS);
	return true;
}

static bool tfmr_recover_non_tb_errors(uint64_t tfmr)
{
	uint64_t tfmr_reset_errors = 0;

	/*
	 * write 1 to bit 26 to clear TFMR HDEC parity error.
	 * HDEC register has already been reset to zero as part pre-recovery.
	 */
	if (tfmr & SPR_TFMR_HDEC_PARITY_ERROR)
		tfmr_reset_errors |= SPR_TFMR_HDEC_PARITY_ERROR;

	if (tfmr & SPR_TFMR_DEC_PARITY_ERR) {
		/* Set DEC with all ones */
		mtspr(SPR_DEC, ~0);

		/* set bit 59 to clear TFMR DEC parity error. */
		tfmr_reset_errors |= SPR_TFMR_DEC_PARITY_ERR;
	}

	/*
	 * Reset PURR/SPURR to recover. We also need help from KVM
	 * layer to handle this change in PURR/SPURR. That needs
	 * to be handled in kernel KVM layer. For now, to recover just
	 * reset it.
	 */
	if (tfmr & SPR_TFMR_PURR_PARITY_ERR) {
		/* set PURR register with sane value or reset it. */
		mtspr(SPR_PURR, 0);

		/* set bit 57 to clear TFMR PURR parity error. */
		tfmr_reset_errors |= SPR_TFMR_PURR_PARITY_ERR;
	}

	if (tfmr & SPR_TFMR_SPURR_PARITY_ERR) {
		/* set PURR register with sane value or reset it. */
		mtspr(SPR_SPURR, 0);

		/* set bit 58 to clear TFMR PURR parity error. */
		tfmr_reset_errors |= SPR_TFMR_SPURR_PARITY_ERR;
	}

	/* Write TFMR twice to clear the error */
	mtspr(SPR_TFMR, base_tfmr | tfmr_reset_errors);
	mtspr(SPR_TFMR, base_tfmr | tfmr_reset_errors);

	/* Get fresh copy of TFMR */
	tfmr = mfspr(SPR_TFMR);

	/* Check if TFMR non-TB errors still present. */
	if (tfmr & tfmr_reset_errors) {
		prerror(
		"CHIPTOD: TFMR non-TB error recovery failed! TFMR=0x%016lx\n",
							mfspr(SPR_TFMR));
		return false;
	}
	return true;
}

/*
 * TFMR parity error recovery as per pc_workbook:
 *	MT(TFMR) bits 11 and 60 are b’1’
 *	MT(HMER) all bits 1 except for bits 4,5
 */
static bool chiptod_recover_tfmr_error(void)
{
	uint64_t tfmr;

	/* Get the base TFMR */
	tfmr = base_tfmr;

	/* Set bit 60 to clear TFMR parity error. */
	tfmr |= SPR_TFMR_TFMR_CORRUPT;
	mtspr(SPR_TFMR, tfmr);

	/* Write twice to clear the error */
	mtspr(SPR_TFMR, tfmr);

	/* Get fresh copy of TFMR */
	tfmr = mfspr(SPR_TFMR);

	/* Check if TFMR parity error still present. */
	if (tfmr & SPR_TFMR_TFMR_CORRUPT) {
		prerror("CHIPTOD: TFMR error recovery: corrupt TFMR !\n");
		return false;
	}

	/*
	 * Now that we have sane value in TFMR, check if Timebase machine
	 * state is in ERROR state. If yes, clear TB errors so that
	 * Timebase machine state changes to RESET state. Once in RESET state
	 * then we can then load TB with TOD value.
	 */
	if (GETFIELD(SPR_TFMR_TBST_ENCODED, tfmr) == TBST_STATE_ERROR) {
		if (!chiptod_reset_tb_errors())
			return false;
	}
	return true;
}

/*
 * Recover from TB and TOD errors.
 * Timebase register is per core and first thread that gets chance to
 * handle interrupt would fix actual TFAC errors and rest of the threads
 * from same core would see no errors. Return -1 if no errors have been
 * found. The caller (handle_hmi_exception) of this function would not
 * send an HMI event to host if return value is -1.
 *
 * Return values:
 *	0	<= Failed to recover from errors
 *	1	<= Successfully recovered from errors
 *	-1	<= No errors found. Errors are already been fixed.
 */
int chiptod_recover_tb_errors(void)
{
	uint64_t tfmr;
	int rc = -1;

	if (chiptod_primary < 0)
		return 0;

	lock(&chiptod_lock);

	/* Get fresh copy of TFMR */
	tfmr = mfspr(SPR_TFMR);

	/*
	 * Check for TFMR parity error and recover from it.
	 * We can not trust any other bits in TFMR If it is corrupt. Fix this
	 * before we do anything.
	 */
	if (tfmr & SPR_TFMR_TFMR_CORRUPT) {
		if (!chiptod_recover_tfmr_error()) {
			rc = 0;
			goto error_out;
		}
	}

	/* Get fresh copy of TFMR */
	tfmr = mfspr(SPR_TFMR);

	/*
	 * Check for TB errors.
	 * On Sync check error, bit 44 of TFMR is set. Check for it and
	 * clear it.
	 */
	if ((tfmr & SPR_TFMR_TB_MISSING_STEP) ||
		(tfmr & SPR_TFMR_TB_RESIDUE_ERR) ||
		(tfmr & SPR_TFMR_FW_CONTROL_ERR) ||
		(tfmr & SPR_TFMR_TBST_CORRUPT) ||
		(tfmr & SPR_TFMR_TB_MISSING_SYNC)) {
		if (!tfmr_recover_tb_errors(tfmr)) {
			rc = 0;
			goto error_out;
		}
	}

	/*
	 * Check for TOD sync check error.
	 * On TOD errors, bit 51 of TFMR is set. If this bit is on then we
	 * need to fetch TOD error register and recover from TOD errors.
	 * Bit 33 of TOD error register indicates sync check error.
	 */
	if (tfmr & SPR_TFMR_CHIP_TOD_INTERRUPT)
		rc = chiptod_recover_tod_errors();

	/* Check if TB is running. If not then we need to get it running. */
	if (!(tfmr & SPR_TFMR_TB_VALID)) {
		rc = 0;

		/* Place TB in Notset state. */
		if (!chiptod_mod_tb())
			goto error_out;

		/*
		 * Before we move TOD to core TB check if TOD is running.
		 * If not, then get TOD in running state.
		 */
		if (!chiptod_running_check())
			if (!chiptod_start_tod())
				goto error_out;

		/* Move chiptod value to core TB */
		if (!chiptod_to_tb())
			goto error_out;

		/* We have successfully able to get TB running. */
		rc = 1;
	}

	/*
	 * Now that TB is running, check for TFMR non-TB errors.
	 */
	if ((tfmr & SPR_TFMR_HDEC_PARITY_ERROR) ||
		(tfmr & SPR_TFMR_PURR_PARITY_ERR) ||
		(tfmr & SPR_TFMR_SPURR_PARITY_ERR) ||
		(tfmr & SPR_TFMR_DEC_PARITY_ERR)) {
		if (!tfmr_recover_non_tb_errors(tfmr)) {
			rc = 0;
			goto error_out;
		}
		rc = 1;
	}

error_out:
	unlock(&chiptod_lock);
	return rc;
}

static int64_t opal_resync_timebase(void)
{
       if (!chiptod_wakeup_resync()) {
               prerror("OPAL: Resync timebase failed on CPU 0x%04x\n",
		      this_cpu()->pir);
               return OPAL_HARDWARE;
       }
       return OPAL_SUCCESS;
}
opal_call(OPAL_RESYNC_TIMEBASE, opal_resync_timebase, 0);

static void chiptod_print_tb(void *data __unused)
{
	prlog(PR_DEBUG, "CHIPTOD: PIR 0x%04x TB=%lx\n",
	      this_cpu()->pir, mfspr(SPR_TBRL));
}

static bool chiptod_probe(void)
{
	struct dt_node *np;

	dt_for_each_compatible(dt_root, np, "ibm,power-chiptod") {
		uint32_t chip;

		/* Old DT has chip-id in chiptod node, newer only in the
		 * parent xscom bridge
		 */
		chip = dt_get_chip_id(np);

		if (dt_has_node_property(np, "primary", NULL)) {
		    chiptod_primary = chip;
		    if (dt_node_is_compatible(np,"ibm,power7-chiptod"))
			    chiptod_type = chiptod_p7;
		    if (dt_node_is_compatible(np,"ibm,power8-chiptod"))
			    chiptod_type = chiptod_p8;
		}

		if (dt_has_node_property(np, "secondary", NULL))
		    chiptod_secondary = chip;

	}

	if (chiptod_type == chiptod_unknown) {
		prerror("CHIPTOD: Unknown TOD type !\n");
		return false;
	}

	return true;
}

static void chiptod_init_topology_info(void)
{
	/* Find and update current topology in use. */
	current_topology = query_current_topology();

	/* Initialized primary topology chip config info */
	chiptod_topology_info[chiptod_topo_primary].id = chiptod_primary;
	chiptod_update_topology(chiptod_topo_primary);

	/* Initialized secondary topology chip config info */
	chiptod_topology_info[chiptod_topo_secondary].id = chiptod_secondary;
	chiptod_update_topology(chiptod_topo_secondary);

	print_topology_info();
}

void chiptod_init(void)
{
	struct cpu_thread *cpu0, *cpu;
	bool sres;

	/* Mambo and qemu doesn't simulate the chiptod */
	if (chip_quirk(QUIRK_NO_CHIPTOD))
		return;

	op_display(OP_LOG, OP_MOD_CHIPTOD, 0);

	if (!chiptod_probe()) {
		prerror("CHIPTOD: Failed ChipTOD detection !\n");
		op_display(OP_FATAL, OP_MOD_CHIPTOD, 0);
		abort();
	}

	op_display(OP_LOG, OP_MOD_CHIPTOD, 1);

	/* Pick somebody on the primary */
	cpu0 = find_cpu_by_chip_id(chiptod_primary);

	/* Calculate the base TFMR value used for everybody */
	chiptod_setup_base_tfmr();

	prlog(PR_DEBUG, "CHIPTOD: Base TFMR=0x%016llx\n", base_tfmr);

	/* Schedule master sync */
	sres = false;
	cpu_wait_job(cpu_queue_job(cpu0, "chiptod_sync_master",
				   chiptod_sync_master, &sres), true);
	if (!sres) {
		op_display(OP_FATAL, OP_MOD_CHIPTOD, 2);
		abort();
	}

	op_display(OP_LOG, OP_MOD_CHIPTOD, 2);

	/* Schedule slave sync */
	for_each_available_cpu(cpu) {
		/* Skip master */
		if (cpu == cpu0)
			continue;

		/* Queue job */
		sres = false;
		cpu_wait_job(cpu_queue_job(cpu, "chiptod_sync_slave",
					   chiptod_sync_slave, &sres),
			     true);
		if (!sres) {
			op_display(OP_WARN, OP_MOD_CHIPTOD, 3|(cpu->pir << 8));

			/* Disable threads */
			cpu_disable_all_threads(cpu);
		}
		op_display(OP_LOG, OP_MOD_CHIPTOD, 3|(cpu->pir << 8));
	}

	/* Display TBs */
	for_each_available_cpu(cpu) {
		/* Only do primaries, not threads */
		if (cpu->is_secondary)
			continue;
		cpu_wait_job(cpu_queue_job(cpu, "chiptod_print_tb",
					   chiptod_print_tb, NULL), true);
	}

	chiptod_init_topology_info();
	op_display(OP_LOG, OP_MOD_CHIPTOD, 4);
}
