/* This file contains the device dependent part of a driver for the IBM-AT
 * winchester controller.
 * It was written by Adri Koppes.
 *
 * The file contains one entry point:
 *
 *   at_winchester_task:	main entry when system is brought up
 *
 *
 * Changes:
 *	23 Nov 2020 by Luke Skywalker: adding XT-CF-lite suuport
 *	13 Apr 1992 by Kees J. Bot: device dependent/independent split.
 */
#include "at_wini.h"
#if ENABLE_AT_WINI
/* Variables. */
PRIVATE struct wini wini[MAX_DRIVES], *w_wn;

PRIVATE struct trans {
  struct iorequest_s *iop;	/* belongs to this I/O request */
  unsigned long block;		/* first sector to transfer */
  unsigned count;		/* byte count */
  phys_bytes phys;		/* user physical address */
} wtrans[NR_IOREQS];

PRIVATE struct trans *w_tp;		/* to add transfer requests */
PRIVATE unsigned w_count;		/* number of bytes to transfer */
PRIVATE unsigned long w_nextblock;	/* next block on disk to transfer */
PRIVATE int w_opcode;			/* DEV_READ or DEV_WRITE */
PRIVATE int w_command;			/* current command in execution */
PRIVATE int w_status;			/* status after interrupt */
PRIVATE int w_drive;			/* selected drive */
PRIVATE struct device *w_dv;		/* device's base and size */

FORWARD _PROTOTYPE( void init_params, (void) );
FORWARD _PROTOTYPE( int w_do_open, (struct driver *dp, message *m_ptr) );
FORWARD _PROTOTYPE( struct device *w_prepare, (int device) );
FORWARD _PROTOTYPE( int w_identify, (void) );
FORWARD _PROTOTYPE( char *w_name, (void) );
FORWARD _PROTOTYPE( int w_specify, (void) );
FORWARD _PROTOTYPE( int w_schedule, (int proc_nr, struct iorequest_s *iop) );
FORWARD _PROTOTYPE( int w_finish, (void) );
FORWARD _PROTOTYPE( void w_need_reset, (void) );
FORWARD _PROTOTYPE( int w_do_close, (struct driver *dp, message *m_ptr) );
FORWARD _PROTOTYPE( int com_simple, (struct command *cmd) );
FORWARD _PROTOTYPE( void w_timeout, (void) );
FORWARD _PROTOTYPE( int w_reset, (void) );
FORWARD _PROTOTYPE( int w_intr_wait, (void) );
FORWARD _PROTOTYPE( int w_waitfor, (int mask, int value) );
FORWARD _PROTOTYPE( int w_handler, (int irq) );
FORWARD _PROTOTYPE( void w_geometry, (struct partition *entry) );
PRIVATE struct driver w_dtab = {
  w_name,		/* current device's name */
  w_do_open,		/* open or mount request, initialize device */
  w_do_close,		/* release device */
  do_diocntl,		/* get or set a partition's geometry */
  w_prepare,		/* prepare for I/O on a given minor device */
  w_schedule,		/* precompute cylinder, head, sector, etc. */
  w_finish,		/* do the I/O */
  nop_cleanup,		/* nothing to clean up */
  w_geometry,		/* tell the geometry of the disk */
};
/* Entry points to this driver. */
/*===========================================================================*
 *				at_winchester_task			     *
 *===========================================================================*/
PUBLIC void at_winchester_task()
{
/* Set special disk parameters then call the generic main loop. */

  init_params();

  driver_task(&w_dtab);
}

/*============================================================================*
 *				init_params				      *
 *============================================================================*/
PRIVATE void init_params()
{
/* This routine is called at startup to initialize the drive parameters. */

  u16_t parv[2];
  unsigned int vector;
  int drive, nr_drives, i;
  struct wini *wn;
  u8_t params[16];
  phys_bytes param_phys = vir2phys(params);

  /* Get the number of drives from the BIOS data area */
  phys_copy(0x475L, param_phys, 1L);
  if ((nr_drives = params[0]) > 2) nr_drives = 2;

  for (drive = 0, wn = wini; drive < MAX_DRIVES; drive++, wn++) {
	if (drive < nr_drives) {
		/* Copy the BIOS parameter vector */
		vector = drive == 0 ? WINI_0_PARM_VEC : WINI_1_PARM_VEC;
		phys_copy(vector * 4L, vir2phys(parv), 4L);

		/* Calculate the address of the parameters and copy them */
		phys_copy(hclick_to_physb(parv[1]) + parv[0], param_phys, 16L);

		/* Copy the parameters to the structures of the drive */
		wn->lcylinders = bp_cylinders(params);
		wn->lheads = 0x0f&bp_heads(params);
		wn->lsectors = bp_sectors(params);
#if IF_IDE
		wn->precomp = bp_precomp(params) >> 2;
#endif /* IF_IDE */
	}
	wn->max_count = MAX_SECS << SECTOR_SHIFT;
	wn->ldhpref = ldh_init(drive);
#if IF_IDE
	if (drive < 2) {
		/* Controller 0. */
		wn->base = REG_BASE0;
		wn->irq = AT_IRQ0;
	} else {
		/* Controller 1. */
		wn->base = REG_BASE1;
		wn->irq = AT_IRQ1;
	}
#elif IF_CF_XT
	wn->base = REG_BASE;
#endif /* IF_IDE */
  }
}


/*============================================================================*
 *				w_do_open				      *
 *============================================================================*/
PRIVATE int w_do_open(dp, m_ptr)
struct driver *dp;
message *m_ptr;
{
/* Device open: Initialize the controller and read the partition table. */

  int r;
  struct wini *wn;
  struct command cmd;

  if (w_prepare(m_ptr->DEVICE) == NIL_DEV) return(ENXIO);
  wn = w_wn;

  if (wn->state == 0) {
	/* Try to identify the device. */
	if (w_identify() != OK) {
		printf("%s: probe failed\n", w_name());
		if (wn->state & DEAF) w_reset();
		wn->state = 0;
		return(ENXIO);
	}
  }
  if (wn->open_ct++ == 0) {
	/* Partition the disk. */
	partition(&w_dtab, w_drive * DEV_PER_DRIVE, P_PRIMARY);
  }
  return(OK);
}


/*===========================================================================*
 *				w_prepare				     *
 *===========================================================================*/
PRIVATE struct device *w_prepare(device)
int device;
{
/* Prepare for I/O on a device. */

  /* Nothing to transfer as yet. */
  w_count = 0;

  if (device < NR_DEVICES) {			/* hd0, hd1, ... */
	w_drive = device / DEV_PER_DRIVE;	/* save drive number */
	w_wn = &wini[w_drive];
	w_dv = &w_wn->part[device % DEV_PER_DRIVE];
  } else
  if ((unsigned) (device -= MINOR_hd1a) < NR_SUBDEVS) {	/* hd1a, hd1b, ... */
	w_drive = device / SUB_PER_DRIVE;
	w_wn = &wini[w_drive];
	w_dv = &w_wn->subpart[device % SUB_PER_DRIVE];
  } else {
	return(NIL_DEV);
  }
  return(w_dv);
}


/*===========================================================================*
 *				w_identify				     *
 *===========================================================================*/
PRIVATE int w_identify()
{
/* Find out if a device exists, if it is an old AT disk, or a newer ATA
 * drive, a removable media device, etc.
 * 2020-11-22: update: take some of 2.0.4 version of this function
 */

  struct wini *wn = w_wn;
  struct command cmd;
  char id_string[40];
  int i, r;
  unsigned long size;
#define id_byte(n)	(&tmp_buf[2 * (n)])
#define id_word(n)	(((u16_t) id_byte(n)[0] <<  0) \
			|((u16_t) id_byte(n)[1] <<  8))
#define id_longword(n)	(((u32_t) id_byte(n)[0] <<  0) \
			|((u32_t) id_byte(n)[1] <<  8) \
			|((u32_t) id_byte(n)[2] << 16) \
			|((u32_t) id_byte(n)[3] << 24))

  /* Try to identify the device */
  cmd.ldh     = wn->ldhpref;
  cmd.command = ATA_IDENTIFY;
  if (com_simple(&cmd) == OK) {
	/* This is an ATA device. */
	wn->state |= SMART;

	/* Device information. */
	port_read(wn->base + REG_DATA, tmp_phys, SECTOR_SIZE);

	/* Why are the strings byte swapped??? */
	for (i = 0; i < 40; i++) id_string[i] = id_byte(27)[i^1];

	/* Preferred CHS translation mode. */
	wn->pcylinders = id_word(1);
	wn->pheads = id_word(3);
	wn->psectors = id_word(6);
	size = (u32_t) wn->pcylinders * wn->pheads * wn->psectors;

	if ((id_byte(49)[1] & 0x02) && size > 512L*1024*2) {
		/* Drive is LBA capable and is big enough to trust it to
		 * not make a mess of it.
		 */
		wn->ldhpref |= LDH_LBA;
		size = id_longword(60);
	}

	if ((wn->lcylinders != wn->pcylinders)
		|| (wn->lheads != wn->pheads)
		|| (wn->lsectors != wn->psectors)) {
		
		/* Wrong BIOS parameters?  Then make some up. */
		wn->lcylinders = wn->pcylinders;
		wn->lheads = wn->pheads;
		wn->lsectors = wn->psectors;
		while (wn->lcylinders > 1024) {
			wn->lheads *= 2;
			wn->lcylinders /= 2;
		}
	}
  /* Here should be ATAPI implementation, but I'm lazy */
  } else {
	/* Not an ATA device; no translations, no special features.  Don't
	 * touch it unless the BIOS knows about it.
	 */
	return(ERR);
  }
  /* The fun ends at 4 GB, don't use such a disk in MINIX, it's mini  */
  if (size > ((u32_t) -1) / SECTOR_SIZE) size = ((u32_t) -1) / SECTOR_SIZE;

  /* Base and size of the whole drive */
  wn->part[0].dv_base = 0;
  wn->part[0].dv_size = size << SECTOR_SHIFT;
#if IF_IDE
  /* if you want to kill your disk you are welcome
	to issue this command twice */
  if (w_specify() != OK && w_specify() != OK) return(ERR);
#elif IF_CF_XT
	wn->state |= INITIALIZED;
#endif /* IF_IDE */

  printf("%s: ", w_name());
  if (wn->state & SMART) {
	printf("%.40s\n", id_string);
  }
  printf("%s: CHS = %ux%ux%u\n", w_name (),
	wn->pcylinders, wn->pheads, wn->psectors);
#ifdef IF_CF_XT_TEST
  w_test_and_panic (w_wn);
#endif
  /* Looks OK; register IRQ and try an ATA identify command. */
#if IF_IDE
  put_irq_handler(wn->irq, w_handler);
  enable_irq(wn->irq);
#elif IF_CF_XT	/* yeah, the interrupt wire is not soldered */
  out_byte (wn->base + REG_CTL, CTL_INTDISABLE); /* bit2 = 0, bit1=0 */ 
#endif /* IF_IDE */
  return(OK);
}


/*===========================================================================*
 *				w_name					     *
 *===========================================================================*/
PRIVATE char *w_name()
{
/* Return a name for the current device. */
  static char name[] = "at-hd15";
  unsigned device = w_drive * DEV_PER_DRIVE;

  if (device < 10) {
	name[5] = '0' + device;
	name[6] = 0;
  } else {
	name[5] = '0' + device / 10;
	name[6] = '0' + device % 10;
  }
  return name;
}


/*===========================================================================*
 *				w_specify				     *
 *===========================================================================*/
PRIVATE int w_specify()
{
/* Routine to initialize the drive after boot or when a reset is needed. */

  struct wini *wn = w_wn;
  struct command cmd;

  if ((wn->state & DEAF) && w_reset() != OK) return(ERR);

  /* Specify parameters: precompensation, number of heads and sectors. */
#if IF_IDE
  cmd.precomp = wn->precomp;
#endif /* IF_IDE */
  /* for CF: feature register is ignored */
  cmd.count   = wn->psectors;
  cmd.ldh     = w_wn->ldhpref | (wn->pheads - 1);
  cmd.command = CMD_SPECIFY;		/* Specify some parameters */

  if (com_simple(&cmd) != OK) return(ERR);

  if (!(wn->state & SMART)) {
	/* Calibrate an old disk. */
	cmd.sector  = 0;
	cmd.cyl_lo  = 0;
	cmd.cyl_hi  = 0;
	cmd.ldh     = w_wn->ldhpref;
	cmd.command = CMD_RECALIBRATE;

	if (com_simple(&cmd) != OK) return(ERR);
  }

  wn->state |= INITIALIZED;
  return(OK);
}


/*===========================================================================*
 *				w_schedule				     *
 *===========================================================================*/
PRIVATE int w_schedule(proc_nr, iop)
int proc_nr;			/* process doing the request */
struct iorequest_s *iop;	/* pointer to read or write request */
{
/* Gather I/O requests on consecutive blocks so they may be read/written
 * in one controller command.  (There is enough time to compute the next
 * consecutive request while an unwanted block passes by.)
 */
  struct wini *wn = w_wn;
  int r, opcode;
  unsigned long pos;
  unsigned nbytes, count;
  unsigned long block;
  phys_bytes user_phys;

  /* This many bytes to read/write */
  nbytes = iop->io_nbytes;
  if ((nbytes & SECTOR_MASK) != 0) return(iop->io_nbytes = EINVAL);

  /* From/to this position on the device */
  pos = iop->io_position;
  if ((pos & SECTOR_MASK) != 0) return(iop->io_nbytes = EINVAL);

  /* To/from this user address */
  user_phys = numap(proc_nr, (vir_bytes) iop->io_buf, nbytes);
  if (user_phys == 0) return(iop->io_nbytes = EINVAL);

  /* Read or write? */
  opcode = iop->io_request & ~OPTIONAL_IO;

  /* Which block on disk and how close to EOF? */
  if (pos >= w_dv->dv_size) return(OK);		/* At EOF */
  if (pos + nbytes > w_dv->dv_size) nbytes = w_dv->dv_size - pos;
  block = (w_dv->dv_base + pos) >> SECTOR_SHIFT;

  if (w_count > 0 && block != w_nextblock) {
	/* This new request can't be chained to the job being built */
	if ((r = w_finish()) != OK) return(r);
  }

  /* The next consecutive block */
  w_nextblock = block + (nbytes >> SECTOR_SHIFT);

  /* While there are "unscheduled" bytes in the request: */
  do {
	count = nbytes;

	if (w_count == wn->max_count) {
		/* The drive can't do more then max_count at once */
		if ((r = w_finish()) != OK) return(r);
	}

	if (w_count + count > wn->max_count)
		count = wn->max_count - w_count;

	if (w_count == 0) {
		/* The first request in a row, initialize. */
		w_opcode = opcode;
		w_tp = wtrans;
	}

	/* Store I/O parameters */
	w_tp->iop = iop;
	w_tp->block = block;
	w_tp->count = count;
	w_tp->phys = user_phys;

	/* Update counters */
	w_tp++;
	w_count += count;
	block += count >> SECTOR_SHIFT;
	user_phys += count;
	nbytes -= count;
  } while (nbytes > 0);

  return(OK);
}


/*===========================================================================*
 *				w_finish				     *
 *===========================================================================*/
PRIVATE int w_finish()
{
/* Carry out the I/O requests gathered in wtrans[]. */

  struct trans *tp = wtrans;
  struct wini *wn = w_wn;
  int r, errors;
  struct command cmd;
  unsigned cylinder, head, sector, secspcyl;

  if (w_count == 0) return(OK);	/* Spurious finish. */

  r = ERR;	/* Trigger the first com_out */
  errors = 0;

  do {
	if (r != OK) {
		/* The controller must be (re)programmed. */

		/* First check to see if a reinitialization is needed. */
		if (!(wn->state & INITIALIZED))
		{
			printf ("%s: check if is initialized\n", w_name ());
#if IF_IDE /* For CF we go into error immediately */
			if (w_specify() != OK) /* There's a hope for IDE */
#endif /* IF_IDE */
			return(tp->iop->io_nbytes = EIO);
		}
		/* Tell the controller to transfer w_count bytes */
#if IF_IDE
		cmd.precomp = wn->precomp;
#endif /* IF_IDE */
		/* No feature */
		cmd.count   = (w_count >> SECTOR_SHIFT) & BYTE;
		if (wn->ldhpref & LDH_LBA) {
			cmd.sector  = (tp->block >>  0) & 0xFF;
			cmd.cyl_lo  = (tp->block >>  8) & 0xFF;
			cmd.cyl_hi  = (tp->block >> 16) & 0xFF;
			cmd.ldh     = wn->ldhpref | ((tp->block >> 24) & 0xF);
		} else {
			secspcyl = wn->pheads * wn->psectors;
			cylinder = tp->block / secspcyl;
			head = (tp->block % secspcyl) / wn->psectors;
			sector = tp->block % wn->psectors;
			cmd.sector  = sector + 1;
			cmd.cyl_lo  = cylinder & BYTE;
			cmd.cyl_hi  = (cylinder >> 8) & BYTE;
			cmd.ldh     = wn->ldhpref | head;
		}
		cmd.command = w_opcode == DEV_WRITE ? CMD_WRITE : CMD_READ;
		if ((r = com_out(&cmd)) != OK) {
			printf ("%s: error sending command %X\n", w_name (),
				cmd.command);
			if (++errors == MAX_ERRORS) {
				w_command = CMD_IDLE;
				return(tp->iop->io_nbytes = EIO);
			}
			continue;	/* Retry */
		}
	}

	/* For each sector, wait for an interrupt and fetch the data (read),
	 * or supply data to the controller and wait for an interrupt (write).
	 */

	if (w_opcode == DEV_READ) {
		if ((r = w_intr_wait()) == OK) {
			/* Copy data from the device's buffer to user space. */

			port_read(wn->base + REG_DATA, tp->phys, SECTOR_SIZE);

			tp->phys += SECTOR_SIZE;
			tp->iop->io_nbytes -= SECTOR_SIZE;
			w_count -= SECTOR_SIZE;
			if ((tp->count -= SECTOR_SIZE) == 0) tp++;
		} else {
			/* Any faulty data? */
			if (w_status & STATUS_DRQ) {
				port_read(wn->base + REG_DATA, tmp_phys,
								SECTOR_SIZE);
			}
		}
	} else {
		/* Wait for data requested. */
		if (!waitfor(STATUS_DRQ, STATUS_DRQ)) {
			r = ERR;
		} else {
			/* Fill the buffer of the drive. */

			port_write(wn->base + REG_DATA, tp->phys, SECTOR_SIZE);
			r = w_intr_wait();
		}

		if (r == OK) {
			/* Book the bytes successfully written. */

			tp->phys += SECTOR_SIZE;
			tp->iop->io_nbytes -= SECTOR_SIZE;
			w_count -= SECTOR_SIZE;
			if ((tp->count -= SECTOR_SIZE) == 0) tp++;
		}
	}

	if (r != OK) {
		/* Don't retry if sector marked bad or too many errors */
		if (r == ERR_BAD_SECTOR || ++errors == MAX_ERRORS) {
			w_command = CMD_IDLE;
			return(tp->iop->io_nbytes = EIO);
		}

		/* Reset if halfway, but bail out if optional I/O. */
		if (errors == MAX_ERRORS / 2) {
			w_need_reset();
			if (tp->iop->io_request & OPTIONAL_IO) {
				w_command = CMD_IDLE;
				return(tp->iop->io_nbytes = EIO);
			}
		}
		continue;	/* Retry */
	}
	errors = 0;
  } while (w_count > 0);

  w_command = CMD_IDLE;
  return(OK);
}


/*============================================================================*
 *				com_out					      *
 *============================================================================*/
PUBLIC int com_out(cmd)
struct command *cmd;		/* Command block */
{
/* Output the command block to the winchester controller and return status */

  struct wini *wn = w_wn;
  unsigned base = wn->base;

  if (!waitfor(STATUS_BSY, 0)) {
	printf("%s: controller 0x%X not ready\n", w_name(), base);
	return(ERR);
  }

  /* Select drive. */
  out_byte(base + REG_LDH, cmd->ldh);

  if (!waitfor(STATUS_BSY, 0)) {
	printf("%s: drive not ready\n", w_name());
	return(ERR);
  }

  /* Schedule a wakeup call, some controllers are flaky. */
  clock_mess(WAKEUP, w_timeout);

#if IF_IDE
  out_byte(base + REG_CTL, wn->pheads >= 8 ? CTL_EIGHTHEADS : 0);
  out_byte(base + REG_PRECOMP, cmd->precomp);
#elif IF_CF_XT
  out_byte(base + REG_FEATURE, cmd->feature);
#endif /* IF_IDE */
  out_byte(base + REG_COUNT, cmd->count);
  out_byte(base + REG_SECTOR, cmd->sector);
  out_byte(base + REG_CYL_LO, cmd->cyl_lo);
  out_byte(base + REG_CYL_HI, cmd->cyl_hi);
  lock();
  out_byte(base + REG_COMMAND, cmd->command);
  w_command = cmd->command;
  w_status = STATUS_BSY;
  unlock();
  return(OK);
}


/*===========================================================================*
 *				w_need_reset				     *
 *===========================================================================*/
PRIVATE void w_need_reset()
{
/* The controller needs to be reset. */
  struct wini *wn;

  for (wn = wini; wn < &wini[MAX_DRIVES]; wn++) {
	wn->state |= DEAF;
	wn->state &= ~INITIALIZED;
  }
}


/*============================================================================*
 *				w_do_close				      *
 *============================================================================*/
PRIVATE int w_do_close(dp, m_ptr)
struct driver *dp;
message *m_ptr;
{
/* Device close: Release a device. */

  if (w_prepare(m_ptr->DEVICE) == NIL_DEV) return(ENXIO);
  w_wn->open_ct--;
  return(OK);
}


/*============================================================================*
 *				com_simple				      *
 *============================================================================*/
PRIVATE int com_simple(cmd)
struct command *cmd;		/* Command block */
{
/* A simple controller command, only one interrupt and no data-out phase. */
  int r;

  if ((r = com_out(cmd)) == OK) r = w_intr_wait();

  w_command = CMD_IDLE;
  return(r);
}


/*===========================================================================*
 *				w_timeout				     *
 *===========================================================================*/
PRIVATE void w_timeout()
{
  struct wini *wn = w_wn;

  switch (w_command) {
  case CMD_IDLE:
	break;		/* fine */
  case CMD_READ:
  case CMD_WRITE:
	/* Impossible, but not on PC's:  The controller does not respond. */

	/* Limiting multisector I/O seems to help. */
	if (wn->max_count > 8 * SECTOR_SIZE) {
		wn->max_count = 8 * SECTOR_SIZE;
	} else {
		wn->max_count = SECTOR_SIZE;
	}
	/*FALL THROUGH*/
  default:
	/* Some other command. */
	printf("%s: timeout on command %02x\n", w_name(), w_command);
	w_need_reset();
	w_status = 0;
	interrupt(WINCHESTER);
  }
}


/*===========================================================================*
 *				w_reset					     *
 *===========================================================================*/
PRIVATE int w_reset()
{
/* Issue a reset to the controller.  This is done after any catastrophe,
 * like the controller refusing to respond.
 */

  struct wini *wn;
  int err;

  /* Wait for any internal drive recovery. */
  milli_delay(RECOVERYTIME);

  /* Strobe reset bit */
  out_byte(w_wn->base + REG_CTL, CTL_RESET);
  milli_delay(1);
  out_byte(w_wn->base + REG_CTL, 0);
  milli_delay(1);

  /* Wait for controller ready */
  if (!w_waitfor(STATUS_BSY | STATUS_RDY, STATUS_RDY)) {
	printf("%s: reset failed, drive busy\n", w_name());
	return(ERR);
  }

  /* The error register should be checked now, but some drives mess it up. */

  for (wn = wini; wn < &wini[MAX_DRIVES]; wn++)
	if (wn->base == w_wn->base) {
		wn->state &= ~DEAF;
#if IF_CF_XT
		wn->state |= INITIALIZED;
#endif /* IF_CF_XT */
  	}
  return(OK);
}


/*============================================================================*
 *				w_intr_wait				      *
 *============================================================================*/
PRIVATE int w_intr_wait()
{
/* Wait for a task completion interrupt and return results. */

  message mess;
  int r;
  unsigned i;	/* Needed for CF_XT only */

#if IF_IDE
  /* Wait for an interrupt that sets w_status to "not busy". */
  while (w_status & STATUS_BSY)
	receive(HARDWARE, &mess);

#elif IF_CF_XT
  /*			Polling
   * CF IRQ wire is not connected, so just wait
   * The idea is stolen from XTIDE Un. BIOS (Src/Device/IDE/IdeWait) */
  for (i=0; i < 4; i++) in_byte (w_wn->base + 2*0xe); /* delay */
  w_waitfor (STATUS_BSY, 0);
  w_status = in_byte (w_wn->base + REG_STATUS);
#endif

  /* Check status. */
  lock();
  if ((w_status & (STATUS_BSY | STATUS_RDY | STATUS_WF | STATUS_ERR))
							== STATUS_RDY) {
	r = OK;
	w_status |= STATUS_BSY;	/* assume still busy with I/O */
  } else
  if ((w_status & STATUS_ERR) && (in_byte(w_wn->base + REG_ERROR) & ERROR_BB)) {
  	r = ERR_BAD_SECTOR;	/* sector marked bad, retries won't help */
  } else {
  	r = ERR;		/* any other error */
  }
  unlock();

  return r;
}

/*==========================================================================*
 *				w_waitfor				    *
 *==========================================================================*/
PRIVATE int w_waitfor(mask, value)
int mask;			/* status mask */
int value;			/* required status */
{
/* Wait until controller is in the required state.  Return zero on timeout. */

  struct milli_state ms;

  milli_start(&ms);
  do {
       if ((in_byte(w_wn->base + REG_STATUS) & mask) == value) return 1;
  } while (milli_elapsed(&ms) < TIMEOUT);

  w_need_reset();	/* Controller gone deaf. */
  return 0;
}


/*==========================================================================*
 *				w_handler				    *
 *==========================================================================*/
PRIVATE int w_handler(irq)
int irq;
{
/* Disk interrupt, send message to winchester task and reenable interrupts. */

  w_status = in_byte(w_wn->base + REG_STATUS);	/* acknowledge interrupt */
  interrupt(WINCHESTER);
  return 1;
}


/*============================================================================*
 *				w_geometry				      *
 *============================================================================*/
PRIVATE void w_geometry(entry)
struct partition *entry;
{
  entry->cylinders = w_wn->pcylinders;
  entry->heads = w_wn->pheads;
  entry->sectors = w_wn->psectors;
}
#endif /* ENABLE_AT_WINI */
