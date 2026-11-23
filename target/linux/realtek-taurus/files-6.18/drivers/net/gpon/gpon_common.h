/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * Shared types for the hardware-independent GPON protocol core: the ONU
 * state encoding, G.984/G.988 wire enumerations, per-chip parameters and
 * the op table through which the core asks a target driver (the "shell")
 * to touch hardware. The core never does I/O itself: time is passed in,
 * per-chip facts arrive through struct gpon_chip_cfg, and multi-octet wire
 * fields are assembled with explicit byte math so the same source works on
 * big- and little-endian CPUs.
 */
#ifndef GPON_COMMON_H
#define GPON_COMMON_H

#include <linux/types.h>

/*
 * The kernel provides fallthrough; this fallback is for host builds of the
 * core. Only the attribute satisfies -Wimplicit-fallthrough=5.
 */
#ifndef fallthrough
# if defined(__GNUC__) && __GNUC__ >= 7
#  define fallthrough	__attribute__((__fallthrough__))
# else
#  define fallthrough	do {} while (0)
# endif
#endif

/* G.984 wire constants */

/* Unassigned ONU-ID, used until Assign_ONU-ID and after every re-range. */
#define GPON_ONU_ID_BROADCAST	0xffu

/* G.984 broadcast GEM Port-ID. */
#define GPON_MCAST_GEM_PORT	4095u

/*
 * Octets of an upstream PLOAM message handed to ->ploam_tx(): ONU-ID,
 * Message-ID and 10 data octets. The CRC octet is appended by hardware.
 */
#define GPON_PLOAM_US_LEN	12u

/*
 * ONU activation state, G.984.3 clause 10 numbering (1-based). Each target
 * maps its own hardware state field to this at the boundary.
 */
enum gpon_ostate {
	GPON_O1_INITIAL		= 1,	/* no downstream sync */
	GPON_O2_STANDBY		= 2,	/* DS sync, awaiting parameters */
	GPON_O3_SERIAL		= 3,	/* serial-number acquisition */
	GPON_O4_RANGING		= 4,	/* ranging (EqD assignment) */
	GPON_O5_OPERATION	= 5,	/* operational */
	GPON_O6_POPUP		= 6,	/* intermittent LOS/LOF, popup */
	GPON_O7_EMERGENCY	= 7,	/* emergency stop (SN disabled) */
};

/* ME 268 (GEM port network CTP) direction attribute, G.988 9.2.3. */
enum gpon_gem_dir {
	GPON_GEM_US	= 1,	/* upstream only   (UNI -> ANI) */
	GPON_GEM_DS	= 2,	/* downstream only (ANI -> UNI) */
	GPON_GEM_BIDIR	= 3,	/* bidirectional, the data GEM */
};

/*
 * Upstream PLOAM queue class for a message. Abstract: the shell maps it to
 * its own hardware queue.
 */
enum gpon_ploam_q {
	GPON_PLOAM_Q_SN,	/* Serial_Number_ONU: the auto-SN slot */
	GPON_PLOAM_Q_URGENT,	/* Acknowledge, Password, Encryption_Key */
	GPON_PLOAM_Q_NOMSG,	/* the idle No_message keepalive slot */
};

/* CRC-32 variant used for the OMCI MIC (bytes 44..47). */
enum gpon_mic_variant {
	GPON_MIC_CRC32_BE_AAL5,	/* ~crc32_be(~0, m, 44), G.984.4 */
	GPON_MIC_CRC32_LE_ZLIB,	/* crc32_le(~0, m, 44) ^ ~0 */
};

/*
 * Per-chip parameters the core needs to know. Not read by the common OMCI
 * responder yet, which uses the G.988 conformant values.
 */
struct gpon_chip_cfg {
	/* OMCI MIC polynomial */
	enum gpon_mic_variant	mic;

	/*
	 * One-past-the-end offset of the Get response value area. 36 is
	 * conformant: G.988 always reserves octets 36-39 for the optional
	 * and execution attribute masks.
	 */
	u16			get_val_end;

	/* Get-Next message type; 0x1a per G.988 Table 11.2.2-1. */
	u8			mt_get_next;
};

/*
 * Op table: everything the core cannot do for itself.
 *
 * @sh is the shell's opaque handle.
 *
 * Calling context: *_decide() core entry points may run in softirq and must
 * not sleep or take locks; *_apply() entry points run in process context
 * and are the only callers of ops that may block or poll.
 *
 * Int-returning ops return 0 or a negative errno (-ETIMEDOUT is possible
 * from table-write completion polls). A NULL op means the target has no such
 * hardware step and the core skips it. Ordering of hardware steps (e.g. the
 * data teardown order) is the shell's business.
 */
struct gpon_shell_ops {
	/* PLOAM activation (targets with a software PLOAM state machine) */

	/* Queue one upstream PLOAM of GPON_PLOAM_US_LEN octets. */
	void (*ploam_tx)(void *sh, enum gpon_ploam_q q,
			 const u8 msg[GPON_PLOAM_US_LEN]);

	/* Publish the activation state to hardware. */
	void (*set_hw_state)(void *sh, enum gpon_ostate st);

	/*
	 * Publish the OLT-assigned ONU-ID (GPON_ONU_ID_BROADCAST on teardown)
	 * to every hardware copy of it.
	 */
	void (*set_hw_onu_id)(void *sh, u8 onu_id);

	/*
	 * Program the upstream equalization delay, in bits, from the OLT's
	 * Ranging_Time. The conversion to register units is chip specific and
	 * stays in the shell.
	 */
	void (*set_eqd)(void *sh, u32 eqd_bits);

	/* Apply burst-overhead parameters for the pre-ranged or ranged case. */
	void (*apply_boh)(void *sh, bool ranged);

	/*
	 * Relock the transmit PLL at O3 entry; without it a cold boot may
	 * never range.
	 */
	void (*analog_relock)(void *sh);

	/*
	 * Load a 128-bit AES key into the staged key bank. Hardware promotes
	 * it to active at the switch time set below.
	 */
	void (*aes_stage_key)(void *sh, const u8 key[16]);

	/* Superframe counter at which the staged key becomes active. */
	void (*aes_set_switch_time)(void *sh, u32 superframe);

	/* Fill @out with @len random octets (injected for deterministic tests). */
	void (*rng)(void *sh, u8 *out, unsigned int len);

	/*
	 * GEM / T-CONT provisioning. The core passes wire identities
	 * (Alloc-ID, GEM Port-ID); the shell owns every hardware index and
	 * performs each install as one transaction.
	 */

	/* Bind the OMCC: management Alloc-ID and its GEM Port-ID. */
	int (*omcc_install)(void *sh, u16 alloc, u16 gem);

	/* Bind the user-data path from the ME 262 / ME 268 values. */
	int (*data_install)(void *sh, u16 alloc, u16 gem);

	/* Tear down the user-data path; NULL if the target has no teardown. */
	void (*data_teardown)(void *sh);

	/* OMCI */

	/* Transmit one OMCI PDU upstream. */
	int (*omci_tx)(void *sh, const u8 *pdu, unsigned int len);

	/*
	 * Diagnostics only: a NULL ->trace must not change any decision or
	 * emitted byte.
	 */
	void (*trace)(void *sh, unsigned int ev, u32 a, u32 b);
};

/*
 * Each core keeps its state in a context struct starting with this base and
 * takes a pointer to it on every entry point, never file-scope globals.
 */
struct gpon_core_base {
	const struct gpon_shell_ops	*ops;
	void				*sh;	/* opaque shell handle */
	const struct gpon_chip_cfg	*chip;
};

#endif /* GPON_COMMON_H */
