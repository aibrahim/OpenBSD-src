/*	$OpenBSD: version.c,v 1.13 2013/02/02 13:34:29 miod Exp $	*/

/*
 * Record major changes in the boot code here, and increment the version
 * number.
 */

/*
 * 2.0	INITIAL REVISION
 * 2.1	APCI support, config changes from NetBSD.
 * 2.2	Grand reorganization.
 * 2.3	Added CD9660 boot support.
 * 2.4	Added/fixed 425e support.
 * 2.5	Added SYS_CDBOOT.
 * 2.6	Fixed RTC reading for 2001.
 * 2.7	Minor syncs with the kernel (recognize more models and use the same
 *	logic to pick the console on 425e).
 * 2.8	TurboVRX frame buffer support.
 * 2.9	SGC frame buffers support, bug fixes and code cleanup.
 * 2.10	Blind SGC support on models 362 and 382, turned out to be useless.
 * 2.11	sti@dio frame buffer support (for models 362 and 382), and various
 *	cleanups.
 * 2.12	Switch to MI loadfile code.
 * 2.13	Allow kernels with uppercase characters in their names to be loaded
 *	from the default boot device without an explicit device or a leading
 *	`/'.
 * 2.14 Build with the ELF toolchain.
 */

const char version[] = "2.14";
