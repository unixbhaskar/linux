// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2005, 2012 IBM Corporation
 *
 * Authors:
 *	Kent Yoder <key@linux.vnet.ibm.com>
 *	Seiji Munetoh <munetoh@jp.ibm.com>
 *	Stefan Berger <stefanb@us.ibm.com>
 *	Reiner Sailer <sailer@watson.ibm.com>
 *	Kylene Hall <kjhall@us.ibm.com>
 *	Nayna Jain <nayna@linux.vnet.ibm.com>
 *
 * Access to the event log created by a system's firmware / BIOS
 */

#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/security.h>
#include <linux/module.h>
#include <linux/tpm_eventlog.h>

#include "../tpm.h"
#include "common.h"

static int tpm_bios_measurements_open(struct inode *inode,
					    struct file *file)
{
	int err;
	struct seq_file *seq;
	struct tpm_chip_seqops *chip_seqops;
	const struct seq_operations *seqops;
	struct tpm_chip *chip;

	inode_lock(inode);
	if (!inode->i_nlink) {
		inode_unlock(inode);
		return -ENODEV;
	}
	chip_seqops = inode->i_private;
	seqops = chip_seqops->seqops;
	chip = chip_seqops->chip;
	get_device(&chip->dev);
	inode_unlock(inode);

	/* now register seq file */
	err = seq_open(file, seqops);
	if (!err) {
		seq = file->private_data;
		seq->private = chip;
	} else {
		put_device(&chip->dev);
	}

	return err;
}

static int tpm_bios_measurements_release(struct inode *inode,
					 struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct tpm_chip *chip = seq->private;

	put_device(&chip->dev);

	return seq_release(inode, file);
}

static const struct file_operations tpm_bios_measurements_ops = {
	.owner = THIS_MODULE,
	.open = tpm_bios_measurements_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = tpm_bios_measurements_release,
};

static int tpm_read_log(struct tpm_chip *chip)
{
	int rc;

	if (chip->log.bios_event_log != NULL) {
		dev_dbg(&chip->dev,
			"%s: ERROR - event log already initialized\n",
			__func__);
		return -EFAULT;
	}

	rc = tpm_read_log_acpi(chip);
	if (rc != -ENODEV)
		return rc;

	rc = tpm_read_log_efi(chip);
	if (rc != -ENODEV)
		return rc;

	return tpm_read_log_of(chip);
}

/*
 * tpm_bios_log_setup() - Read the event log from the firmware
 * @chip: TPM chip to use.
 *
 * If an event log is found then the securityfs files are setup to
 * export it to userspace, otherwise nothing is done.
 */
void tpm_bios_log_setup(struct tpm_chip *chip)
{
	const char *name = dev_name(&chip->dev);
	struct dentry *dentry;
	int log_version;
	int rc = 0;

	if (chip->flags & TPM_CHIP_FLAG_VIRTUAL)
		return;

	rc = tpm_read_log(chip);
	if (rc < 0)
		return;
	log_version = rc;

	chip->bios_dir = securityfs_create_dir(name, NULL);
	/* NOTE: securityfs_create_dir can return ENODEV if securityfs is
	 * compiled out. The caller should ignore the ENODEV return code.
	 */
	if (IS_ERR(chip->bios_dir))
		return;

	chip->bin_log_seqops.chip = chip;
	if (log_version == EFI_TCG2_EVENT_LOG_FORMAT_TCG_2)
		chip->bin_log_seqops.seqops =
			&tpm2_binary_b_measurements_seqops;
	else
		chip->bin_log_seqops.seqops =
			&tpm1_binary_b_measurements_seqops;


	dentry =
	    securityfs_create_file("binary_bios_measurements",
				   0440, chip->bios_dir,
				   (void *)&chip->bin_log_seqops,
				   &tpm_bios_measurements_ops);
	if (IS_ERR(dentry))
		goto err;

	if (!(chip->flags & TPM_CHIP_FLAG_TPM2)) {

		chip->ascii_log_seqops.chip = chip;
		chip->ascii_log_seqops.seqops =
			&tpm1_ascii_b_measurements_seqops;

		dentry =
			securityfs_create_file("ascii_bios_measurements",
					       0440, chip->bios_dir,
					       (void *)&chip->ascii_log_seqops,
					       &tpm_bios_measurements_ops);
		if (IS_ERR(dentry))
			goto err;
	}

	return;

err:
	tpm_bios_log_teardown(chip);
	return;
}

void tpm_bios_log_teardown(struct tpm_chip *chip)
{
	securityfs_remove(chip->bios_dir);
}
