// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * raid10.c : Multiple Devices driver for Linux
 *
 * Copyright (C) 2000-2004 Neil Brown
 *
 * RAID-10 support for md.
 *
 * Base on code in raid1.c.  See raid1.c for further copyright information.
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/ratelimit.h>
#include <linux/kthread.h>
#include <linux/raid/md_p.h>
#include <trace/events/block.h>
#include "md.h"

#define RAID_1_10_NAME "raid10"
#include "raid10.h"
#include "raid0.h"
#include "md-bitmap.h"
#include "md-cluster.h"

/*
 * RAID10 provides a combination of RAID0 and RAID1 functionality.
 * The layout of data is defined by
 *    chunk_size
 *    raid_disks
 *    near_copies (stored in low byte of layout)
 *    far_copies (stored in second byte of layout)
 *    far_offset (stored in bit 16 of layout )
 *    use_far_sets (stored in bit 17 of layout )
 *    use_far_sets_bugfixed (stored in bit 18 of layout )
 *
 * The data to be stored is divided into chunks using chunksize.  Each device
 * is divided into far_copies sections.   In each section, chunks are laid out
 * in a style similar to raid0, but near_copies copies of each chunk is stored
 * (each on a different drive).  The starting device for each section is offset
 * near_copies from the starting device of the previous section.  Thus there
 * are (near_copies * far_copies) of each chunk, and each is on a different
 * drive.  near_copies and far_copies must be at least one, and their product
 * is at most raid_disks.
 *
 * If far_offset is true, then the far_copies are handled a bit differently.
 * The copies are still in different stripes, but instead of being very far
 * apart on disk, there are adjacent stripes.
 *
 * The far and offset algorithms are handled slightly differently if
 * 'use_far_sets' is true.  In this case, the array's devices are grouped into
 * sets that are (near_copies * far_copies) in size.  The far copied stripes
 * are still shifted by 'near_copies' devices, but this shifting stays confined
 * to the set rather than the entire array.  This is done to improve the number
 * of device combinations that can fail without causing the array to fail.
 * Example 'far' algorithm w/o 'use_far_sets' (each letter represents a chunk
 * on a device):
 *    A B C D    A B C D E
 *      ...         ...
 *    D A B C    E A B C D
 * Example 'far' algorithm w/ 'use_far_sets' enabled (sets illustrated w/ []'s):
 *    [A B] [C D]    [A B] [C D E]
 *    |...| |...|    |...| | ... |
 *    [B A] [D C]    [B A] [E C D]
 */

static void allow_barrier(struct r10conf *conf);
static void lower_barrier(struct r10conf *conf);
static int _enough(struct r10conf *conf, int previous, int ignore);
static int enough(struct r10conf *conf, int ignore);
static sector_t reshape_request(struct mddev *mddev, sector_t sector_nr,
				int *skipped);
static void reshape_request_write(struct mddev *mddev, struct r10bio *r10_bio);
static void end_reshape_write(struct bio *bio);
static void end_reshape(struct r10conf *conf);

#include "raid1-10.c"

#define NULL_CMD
#define cmd_before(conf, cmd) \
	do { \
		write_sequnlock_irq(&(conf)->resync_lock); \
		cmd; \
	} while (0)
#define cmd_after(conf) write_seqlock_irq(&(conf)->resync_lock)

#define wait_event_barrier_cmd(conf, cond, cmd) \
	wait_event_cmd((conf)->wait_barrier, cond, cmd_before(conf, cmd), \
		       cmd_after(conf))

#define wait_event_barrier(conf, cond) \
	wait_event_barrier_cmd(conf, cond, NULL_CMD)

/*
 * for resync bio, r10bio pointer can be retrieved from the per-bio
 * 'struct resync_pages'.
 */
static inline struct r10bio *get_resync_r10bio(struct bio *bio)
{
	return get_resync_pages(bio)->raid_bio;
}

static void * r10bio_pool_alloc(gfp_t gfp_flags, void *data)
{
	struct r10conf *conf = data;
	int size = offsetof(struct r10bio, devs[conf->geo.raid_disks]);

	/* allocate a r10bio with room for raid_disks entries in the
	 * bios array */
	return kzalloc(size, gfp_flags);
}

#define RESYNC_SECTORS (RESYNC_BLOCK_SIZE >> 9)
/* amount of memory to reserve for resync requests */
#define RESYNC_WINDOW (1024*1024)
/* maximum number of concurrent requests, memory permitting */
#define RESYNC_DEPTH (32*1024*1024/RESYNC_BLOCK_SIZE)
#define CLUSTER_RESYNC_WINDOW (32 * RESYNC_WINDOW)
#define CLUSTER_RESYNC_WINDOW_SECTORS (CLUSTER_RESYNC_WINDOW >> 9)

/*
 * When performing a resync, we need to read and compare, so
 * we need as many pages are there are copies.
 * When performing a recovery, we need 2 bios, one for read,
 * one for write (we recover only one drive per r10buf)
 *
 */
static void * r10buf_pool_alloc(gfp_t gfp_flags, void *data)
{
	struct r10conf *conf = data;
	struct r10bio *r10_bio;
	struct bio *bio;
	int j;
	int nalloc, nalloc_rp;
	struct resync_pages *rps;

	r10_bio = r10bio_pool_alloc(gfp_flags, conf);
	if (!r10_bio)
		return NULL;

	if (test_bit(MD_RECOVERY_SYNC, &conf->mddev->recovery) ||
	    test_bit(MD_RECOVERY_RESHAPE, &conf->mddev->recovery))
		nalloc = conf->copies; /* resync */
	else
		nalloc = 2; /* recovery */

	/* allocate once for all bios */
	if (!conf->have_replacement)
		nalloc_rp = nalloc;
	else
		nalloc_rp = nalloc * 2;
	rps = kmalloc_array(nalloc_rp, sizeof(struct resync_pages), gfp_flags);
	if (!rps)
		goto out_free_r10bio;

	/*
	 * Allocate bios.
	 */
	for (j = nalloc ; j-- ; ) {
		bio = bio_kmalloc(RESYNC_PAGES, gfp_flags);
		if (!bio)
			goto out_free_bio;
		bio_init(bio, NULL, bio->bi_inline_vecs, RESYNC_PAGES, 0);
		r10_bio->devs[j].bio = bio;
		if (!conf->have_replacement)
			continue;
		bio = bio_kmalloc(RESYNC_PAGES, gfp_flags);
		if (!bio)
			goto out_free_bio;
		bio_init(bio, NULL, bio->bi_inline_vecs, RESYNC_PAGES, 0);
		r10_bio->devs[j].repl_bio = bio;
	}
	/*
	 * Allocate RESYNC_PAGES data pages and attach them
	 * where needed.
	 */
	for (j = 0; j < nalloc; j++) {
		struct bio *rbio = r10_bio->devs[j].repl_bio;
		struct resync_pages *rp, *rp_repl;

		rp = &rps[j];
		if (rbio)
			rp_repl = &rps[nalloc + j];

		bio = r10_bio->devs[j].bio;

		if (!j || test_bit(MD_RECOVERY_SYNC,
				   &conf->mddev->recovery)) {
			if (resync_alloc_pages(rp, gfp_flags))
				goto out_free_pages;
		} else {
			memcpy(rp, &rps[0], sizeof(*rp));
			resync_get_all_pages(rp);
		}

		rp->raid_bio = r10_bio;
		bio->bi_private = rp;
		if (rbio) {
			memcpy(rp_repl, rp, sizeof(*rp));
			rbio->bi_private = rp_repl;
		}
	}

	return r10_bio;

out_free_pages:
	while (--j >= 0)
		resync_free_pages(&rps[j]);

	j = 0;
out_free_bio:
	for ( ; j < nalloc; j++) {
		if (r10_bio->devs[j].bio)
			bio_uninit(r10_bio->devs[j].bio);
		kfree(r10_bio->devs[j].bio);
		if (r10_bio->devs[j].repl_bio)
			bio_uninit(r10_bio->devs[j].repl_bio);
		kfree(r10_bio->devs[j].repl_bio);
	}
	kfree(rps);
out_free_r10bio:
	rbio_pool_free(r10_bio, conf);
	return NULL;
}

static void r10buf_pool_free(void *__r10_bio, void *data)
{
	struct r10conf *conf = data;
	struct r10bio *r10bio = __r10_bio;
	int j;
	struct resync_pages *rp = NULL;

	for (j = conf->copies; j--; ) {
		struct bio *bio = r10bio->devs[j].bio;

		if (bio) {
			rp = get_resync_pages(bio);
			resync_free_pages(rp);
			bio_uninit(bio);
			kfree(bio);
		}

		bio = r10bio->devs[j].repl_bio;
		if (bio) {
			bio_uninit(bio);
			kfree(bio);
		}
	}

	/* resync pages array stored in the 1st bio's .bi_private */
	kfree(rp);

	rbio_pool_free(r10bio, conf);
}

static void put_all_bios(struct r10conf *conf, struct r10bio *r10_bio)
{
	int i;

	for (i = 0; i < conf->geo.raid_disks; i++) {
		struct bio **bio = & r10_bio->devs[i].bio;
		if (!BIO_SPECIAL(*bio))
			bio_put(*bio);
		*bio = NULL;
		bio = &r10_bio->devs[i].repl_bio;
		if (r10_bio->read_slot < 0 && !BIO_SPECIAL(*bio))
			bio_put(*bio);
		*bio = NULL;
	}
}

static void free_r10bio(struct r10bio *r10_bio)
{
	struct r10conf *conf = r10_bio->mddev->private;

	put_all_bios(conf, r10_bio);
	mempool_free(r10_bio, &conf->r10bio_pool);
}

static void put_buf(struct r10bio *r10_bio)
{
	struct r10conf *conf = r10_bio->mddev->private;

	mempool_free(r10_bio, &conf->r10buf_pool);

	lower_barrier(conf);
}

static void wake_up_barrier(struct r10conf *conf)
{
	if (wq_has_sleeper(&conf->wait_barrier))
		wake_up(&conf->wait_barrier);
}

static void reschedule_retry(struct r10bio *r10_bio)
{
	unsigned long flags;
	struct mddev *mddev = r10_bio->mddev;
	struct r10conf *conf = mddev->private;

	spin_lock_irqsave(&conf->device_lock, flags);
	list_add(&r10_bio->retry_list, &conf->retry_list);
	conf->nr_queued ++;
	spin_unlock_irqrestore(&conf->device_lock, flags);

	/* wake up frozen array... */
	wake_up(&conf->wait_barrier);

	md_wakeup_thread(mddev->thread);
}

/*
 * raid_end_bio_io() is called when we have finished servicing a mirrored
 * operation and are ready to return a success/failure code to the buffer
 * cache layer.
 */
static void raid_end_bio_io(struct r10bio *r10_bio)
{
	struct bio *bio = r10_bio->master_bio;
	struct r10conf *conf = r10_bio->mddev->private;

	if (!test_bit(R10BIO_Uptodate, &r10_bio->state))
		bio->bi_status = BLK_STS_IOERR;

	bio_endio(bio);
	/*
	 * Wake up any possible resync thread that waits for the device
	 * to go idle.
	 */
	allow_barrier(conf);

	free_r10bio(r10_bio);
}

/*
 * Update disk head position estimator based on IRQ completion info.
 */
static inline void update_head_pos(int slot, struct r10bio *r10_bio)
{
	struct r10conf *conf = r10_bio->mddev->private;

	conf->mirrors[r10_bio->devs[slot].devnum].head_position =
		r10_bio->devs[slot].addr + (r10_bio->sectors);
}

/*
 * Find the disk number which triggered given bio
 */
static int find_bio_disk(struct r10conf *conf, struct r10bio *r10_bio,
			 struct bio *bio, int *slotp, int *replp)
{
	int slot;
	int repl = 0;

	for (slot = 0; slot < conf->geo.raid_disks; slot++) {
		if (r10_bio->devs[slot].bio == bio)
			break;
		if (r10_bio->devs[slot].repl_bio == bio) {
			repl = 1;
			break;
		}
	}

	update_head_pos(slot, r10_bio);

	if (slotp)
		*slotp = slot;
	if (replp)
		*replp = repl;
	return r10_bio->devs[slot].devnum;
}

static void raid10_end_read_request(struct bio *bio)
{
	int uptodate = !bio->bi_status;
	struct r10bio *r10_bio = bio->bi_private;
	int slot;
	struct md_rdev *rdev;
	struct r10conf *conf = r10_bio->mddev->private;

	slot = r10_bio->read_slot;
	rdev = r10_bio->devs[slot].rdev;
	/*
	 * this branch is our 'one mirror IO has finished' event handler:
	 */
	update_head_pos(slot, r10_bio);

	if (uptodate) {
		/*
		 * Set R10BIO_Uptodate in our master bio, so that
		 * we will return a good error code to the higher
		 * levels even if IO on some other mirrored buffer fails.
		 *
		 * The 'master' represents the composite IO operation to
		 * user-side. So if something waits for IO, then it will
		 * wait for the 'master' bio.
		 */
		set_bit(R10BIO_Uptodate, &r10_bio->state);
	} else if (!raid1_should_handle_error(bio)) {
		uptodate = 1;
	} else {
		/* If all other devices that store this block have
		 * failed, we want to return the error upwards rather
		 * than fail the last device.  Here we redefine
		 * "uptodate" to mean "Don't want to retry"
		 */
		if (!_enough(conf, test_bit(R10BIO_Previous, &r10_bio->state),
			     rdev->raid_disk))
			uptodate = 1;
	}
	if (uptodate) {
		raid_end_bio_io(r10_bio);
		rdev_dec_pending(rdev, conf->mddev);
	} else {
		/*
		 * oops, read error - keep the refcount on the rdev
		 */
		pr_err_ratelimited("md/raid10:%s: %pg: rescheduling sector %llu\n",
				   mdname(conf->mddev),
				   rdev->bdev,
				   (unsigned long long)r10_bio->sector);
		set_bit(R10BIO_ReadError, &r10_bio->state);
		reschedule_retry(r10_bio);
	}
}

static void close_write(struct r10bio *r10_bio)
{
	struct mddev *mddev = r10_bio->mddev;

	md_write_end(mddev);
}

static void one_write_done(struct r10bio *r10_bio)
{
	if (atomic_dec_and_test(&r10_bio->remaining)) {
		if (test_bit(R10BIO_WriteError, &r10_bio->state))
			reschedule_retry(r10_bio);
		else {
			close_write(r10_bio);
			if (test_bit(R10BIO_MadeGood, &r10_bio->state))
				reschedule_retry(r10_bio);
			else
				raid_end_bio_io(r10_bio);
		}
	}
}

static void raid10_end_write_request(struct bio *bio)
{
	struct r10bio *r10_bio = bio->bi_private;
	int dev;
	int dec_rdev = 1;
	struct r10conf *conf = r10_bio->mddev->private;
	int slot, repl;
	struct md_rdev *rdev = NULL;
	struct bio *to_put = NULL;
	bool ignore_error = !raid1_should_handle_error(bio) ||
		(bio->bi_status && bio_op(bio) == REQ_OP_DISCARD);

	dev = find_bio_disk(conf, r10_bio, bio, &slot, &repl);

	if (repl)
		rdev = conf->mirrors[dev].replacement;
	if (!rdev) {
		smp_rmb();
		repl = 0;
		rdev = conf->mirrors[dev].rdev;
	}
	/*
	 * this branch is our 'one mirror IO has finished' event handler:
	 */
	if (bio->bi_status && !ignore_error) {
		if (repl)
			/* Never record new bad blocks to replacement,
			 * just fail it.
			 */
			md_error(rdev->mddev, rdev);
		else {
			set_bit(WriteErrorSeen,	&rdev->flags);
			if (!test_and_set_bit(WantReplacement, &rdev->flags))
				set_bit(MD_RECOVERY_NEEDED,
					&rdev->mddev->recovery);

			dec_rdev = 0;
			if (test_bit(FailFast, &rdev->flags) &&
			    (bio->bi_opf & MD_FAILFAST)) {
				md_error(rdev->mddev, rdev);
			}

			/*
			 * When the device is faulty, it is not necessary to
			 * handle write error.
			 */
			if (!test_bit(Faulty, &rdev->flags))
				set_bit(R10BIO_WriteError, &r10_bio->state);
			else {
				/* Fail the request */
				r10_bio->devs[slot].bio = NULL;
				to_put = bio;
				dec_rdev = 1;
			}
		}
	} else {
		/*
		 * Set R10BIO_Uptodate in our master bio, so that
		 * we will return a good error code for to the higher
		 * levels even if IO on some other mirrored buffer fails.
		 *
		 * The 'master' represents the composite IO operation to
		 * user-side. So if something waits for IO, then it will
		 * wait for the 'master' bio.
		 *
		 * Do not set R10BIO_Uptodate if the current device is
		 * rebuilding or Faulty. This is because we cannot use
		 * such device for properly reading the data back (we could
		 * potentially use it, if the current write would have felt
		 * before rdev->recovery_offset, but for simplicity we don't
		 * check this here.
		 */
		if (test_bit(In_sync, &rdev->flags) &&
		    !test_bit(Faulty, &rdev->flags))
			set_bit(R10BIO_Uptodate, &r10_bio->state);

		/* Maybe we can clear some bad blocks. */
		if (rdev_has_badblock(rdev, r10_bio->devs[slot].addr,
				      r10_bio->sectors) &&
		    !ignore_error) {
			bio_put(bio);
			if (repl)
				r10_bio->devs[slot].repl_bio = IO_MADE_GOOD;
			else
				r10_bio->devs[slot].bio = IO_MADE_GOOD;
			dec_rdev = 0;
			set_bit(R10BIO_MadeGood, &r10_bio->state);
		}
	}

	/*
	 *
	 * Let's see if all mirrored write operations have finished
	 * already.
	 */
	one_write_done(r10_bio);
	if (dec_rdev)
		rdev_dec_pending(rdev, conf->mddev);
	if (to_put)
		bio_put(to_put);
}

/*
 * RAID10 layout manager
 * As well as the chunksize and raid_disks count, there are two
 * parameters: near_copies and far_copies.
 * near_copies * far_copies must be <= raid_disks.
 * Normally one of these will be 1.
 * If both are 1, we get raid0.
 * If near_copies == raid_disks, we get raid1.
 *
 * Chunks are laid out in raid0 style with near_copies copies of the
 * first chunk, followed by near_copies copies of the next chunk and
 * so on.
 * If far_copies > 1, then after 1/far_copies of the array has been assigned
 * as described above, we start again with a device offset of near_copies.
 * So we effectively have another copy of the whole array further down all
 * the drives, but with blocks on different drives.
 * With this layout, and block is never stored twice on the one device.
 *
 * raid10_find_phys finds the sector offset of a given virtual sector
 * on each device that it is on.
 *
 * raid10_find_virt does the reverse mapping, from a device and a
 * sector offset to a virtual address
 */

static void __raid10_find_phys(struct geom *geo, struct r10bio *r10bio)
{
	int n,f;
	sector_t sector;
	sector_t chunk;
	sector_t stripe;
	int dev;
	int slot = 0;
	int last_far_set_start, last_far_set_size;

	last_far_set_start = (geo->raid_disks / geo->far_set_size) - 1;
	last_far_set_start *= geo->far_set_size;

	last_far_set_size = geo->far_set_size;
	last_far_set_size += (geo->raid_disks % geo->far_set_size);

	/* now calculate first sector/dev */
	chunk = r10bio->sector >> geo->chunk_shift;
	sector = r10bio->sector & geo->chunk_mask;

	chunk *= geo->near_copies;
	stripe = chunk;
	dev = sector_div(stripe, geo->raid_disks);
	if (geo->far_offset)
		stripe *= geo->far_copies;

	sector += stripe << geo->chunk_shift;

	/* and calculate all the others */
	for (n = 0; n < geo->near_copies; n++) {
		int d = dev;
		int set;
		sector_t s = sector;
		r10bio->devs[slot].devnum = d;
		r10bio->devs[slot].addr = s;
		slot++;

		for (f = 1; f < geo->far_copies; f++) {
			set = d / geo->far_set_size;
			d += geo->near_copies;

			if ((geo->raid_disks % geo->far_set_size) &&
			    (d > last_far_set_start)) {
				d -= last_far_set_start;
				d %= last_far_set_size;
				d += last_far_set_start;
			} else {
				d %= geo->far_set_size;
				d += geo->far_set_size * set;
			}
			s += geo->stride;
			r10bio->devs[slot].devnum = d;
			r10bio->devs[slot].addr = s;
			slot++;
		}
		dev++;
		if (dev >= geo->raid_disks) {
			dev = 0;
			sector += (geo->chunk_mask + 1);
		}
	}
}

static void raid10_find_phys(struct r10conf *conf, struct r10bio *r10bio)
{
	struct geom *geo = &conf->geo;

	if (conf->reshape_progress != MaxSector &&
	    ((r10bio->sector >= conf->reshape_progress) !=
	     conf->mddev->reshape_backwards)) {
		set_bit(R10BIO_Previous, &r10bio->state);
		geo = &conf->prev;
	} else
		clear_bit(R10BIO_Previous, &r10bio->state);

	__raid10_find_phys(geo, r10bio);
}

static sector_t raid10_find_virt(struct r10conf *conf, sector_t sector, int dev)
{
	sector_t offset, chunk, vchunk;
	/* Never use conf->prev as this is only called during resync
	 * or recovery, so reshape isn't happening
	 */
	struct geom *geo = &conf->geo;
	int far_set_start = (dev / geo->far_set_size) * geo->far_set_size;
	int far_set_size = geo->far_set_size;
	int last_far_set_start;

	if (geo->raid_disks % geo->far_set_size) {
		last_far_set_start = (geo->raid_disks / geo->far_set_size) - 1;
		last_far_set_start *= geo->far_set_size;

		if (dev >= last_far_set_start) {
			far_set_size = geo->far_set_size;
			far_set_size += (geo->raid_disks % geo->far_set_size);
			far_set_start = last_far_set_start;
		}
	}

	offset = sector & geo->chunk_mask;
	if (geo->far_offset) {
		int fc;
		chunk = sector >> geo->chunk_shift;
		fc = sector_div(chunk, geo->far_copies);
		dev -= fc * geo->near_copies;
		if (dev < far_set_start)
			dev += far_set_size;
	} else {
		while (sector >= geo->stride) {
			sector -= geo->stride;
			if (dev < (geo->near_copies + far_set_start))
				dev += far_set_size - geo->near_copies;
			else
				dev -= geo->near_copies;
		}
		chunk = sector >> geo->chunk_shift;
	}
	vchunk = chunk * geo->raid_disks + dev;
	sector_div(vchunk, geo->near_copies);
	return (vchunk << geo->chunk_shift) + offset;
}

/*
 * This routine returns the disk from which the requested read should
 * be done. There is a per-array 'next expected sequential IO' sector
 * number - if this matches on the next IO then we use the last disk.
 * There is also a per-disk 'last know head position' sector that is
 * maintained from IRQ contexts, both the normal and the resync IO
 * completion handlers update this position correctly. If there is no
 * perfect sequential match then we pick the disk whose head is closest.
 *
 * If there are 2 mirrors in the same 2 devices, performance degrades
 * because position is mirror, not device based.
 *
 * The rdev for the device selected will have nr_pending incremented.
 */

/*
 * FIXME: possibly should rethink readbalancing and do it differently
 * depending on near_copies / far_copies geometry.
 */
static struct md_rdev *read_balance(struct r10conf *conf,
				    struct r10bio *r10_bio,
				    int *max_sectors)
{
	const sector_t this_sector = r10_bio->sector;
	int disk, slot;
	int sectors = r10_bio->sectors;
	int best_good_sectors;
	sector_t new_distance, best_dist;
	struct md_rdev *best_dist_rdev, *best_pending_rdev, *rdev = NULL;
	int do_balance;
	int best_dist_slot, best_pending_slot;
	bool has_nonrot_disk = false;
	unsigned int min_pending;
	struct geom *geo = &conf->geo;

	raid10_find_phys(conf, r10_bio);
	best_dist_slot = -1;
	min_pending = UINT_MAX;
	best_dist_rdev = NULL;
	best_pending_rdev = NULL;
	best_dist = MaxSector;
	best_good_sectors = 0;
	do_balance = 1;
	clear_bit(R10BIO_FailFast, &r10_bio->state);

	if (raid1_should_read_first(conf->mddev, this_sector, sectors))
		do_balance = 0;

	for (slot = 0; slot < conf->copies ; slot++) {
		sector_t first_bad;
		sector_t bad_sectors;
		sector_t dev_sector;
		unsigned int pending;
		bool nonrot;

		if (r10_bio->devs[slot].bio == IO_BLOCKED)
			continue;
		disk = r10_bio->devs[slot].devnum;
		rdev = conf->mirrors[disk].replacement;
		if (rdev == NULL || test_bit(Faulty, &rdev->flags) ||
		    r10_bio->devs[slot].addr + sectors >
		    rdev->recovery_offset)
			rdev = conf->mirrors[disk].rdev;
		if (rdev == NULL ||
		    test_bit(Faulty, &rdev->flags))
			continue;
		if (!test_bit(In_sync, &rdev->flags) &&
		    r10_bio->devs[slot].addr + sectors > rdev->recovery_offset)
			continue;

		dev_sector = r10_bio->devs[slot].addr;
		if (is_badblock(rdev, dev_sector, sectors,
				&first_bad, &bad_sectors)) {
			if (best_dist < MaxSector)
				/* Already have a better slot */
				continue;
			if (first_bad <= dev_sector) {
				/* Cannot read here.  If this is the
				 * 'primary' device, then we must not read
				 * beyond 'bad_sectors' from another device.
				 */
				bad_sectors -= (dev_sector - first_bad);
				if (!do_balance && sectors > bad_sectors)
					sectors = bad_sectors;
				if (best_good_sectors > sectors)
					best_good_sectors = sectors;
			} else {
				sector_t good_sectors =
					first_bad - dev_sector;
				if (good_sectors > best_good_sectors) {
					best_good_sectors = good_sectors;
					best_dist_slot = slot;
					best_dist_rdev = rdev;
				}
				if (!do_balance)
					/* Must read from here */
					break;
			}
			continue;
		} else
			best_good_sectors = sectors;

		if (!do_balance)
			break;

		nonrot = bdev_nonrot(rdev->bdev);
		has_nonrot_disk |= nonrot;
		pending = atomic_read(&rdev->nr_pending);
		if (min_pending > pending && nonrot) {
			min_pending = pending;
			best_pending_slot = slot;
			best_pending_rdev = rdev;
		}

		if (best_dist_slot >= 0)
			/* At least 2 disks to choose from so failfast is OK */
			set_bit(R10BIO_FailFast, &r10_bio->state);
		/* This optimisation is debatable, and completely destroys
		 * sequential read speed for 'far copies' arrays.  So only
		 * keep it for 'near' arrays, and review those later.
		 */
		if (geo->near_copies > 1 && !pending)
			new_distance = 0;

		/* for far > 1 always use the lowest address */
		else if (geo->far_copies > 1)
			new_distance = r10_bio->devs[slot].addr;
		else
			new_distance = abs(r10_bio->devs[slot].addr -
					   conf->mirrors[disk].head_position);

		if (new_distance < best_dist) {
			best_dist = new_distance;
			best_dist_slot = slot;
			best_dist_rdev = rdev;
		}
	}
	if (slot >= conf->copies) {
		if (has_nonrot_disk) {
			slot = best_pending_slot;
			rdev = best_pending_rdev;
		} else {
			slot = best_dist_slot;
			rdev = best_dist_rdev;
		}
	}

	if (slot >= 0) {
		atomic_inc(&rdev->nr_pending);
		r10_bio->read_slot = slot;
	} else
		rdev = NULL;
	*max_sectors = best_good_sectors;

	return rdev;
}

static void flush_pending_writes(struct r10conf *conf)
{
	/* Any writes that have been queued but are awaiting
	 * bitmap updates get flushed here.
	 */
	spin_lock_irq(&conf->device_lock);

	if (conf->pending_bio_list.head) {
		struct blk_plug plug;
		struct bio *bio;

		bio = bio_list_get(&conf->pending_bio_list);
		spin_unlock_irq(&conf->device_lock);

		/*
		 * As this is called in a wait_event() loop (see freeze_array),
		 * current->state might be TASK_UNINTERRUPTIBLE which will
		 * cause a warning when we prepare to wait again.  As it is
		 * rare that this path is taken, it is perfectly safe to force
		 * us to go around the wait_event() loop again, so the warning
		 * is a false-positive. Silence the warning by resetting
		 * thread state
		 */
		__set_current_state(TASK_RUNNING);

		blk_start_plug(&plug);
		raid1_prepare_flush_writes(conf->mddev);
		wake_up(&conf->wait_barrier);

		while (bio) { /* submit pending writes */
			struct bio *next = bio->bi_next;

			raid1_submit_write(bio);
			bio = next;
			cond_resched();
		}
		blk_finish_plug(&plug);
	} else
		spin_unlock_irq(&conf->device_lock);
}

/* Barriers....
 * Sometimes we need to suspend IO while we do something else,
 * either some resync/recovery, or reconfigure the array.
 * To do this we raise a 'barrier'.
 * The 'barrier' is a counter that can be raised multiple times
 * to count how many activities are happening which preclude
 * normal IO.
 * We can only raise the barrier if there is no pending IO.
 * i.e. if nr_pending == 0.
 * We choose only to raise the barrier if no-one is waiting for the
 * barrier to go down.  This means that as soon as an IO request
 * is ready, no other operations which require a barrier will start
 * until the IO request has had a chance.
 *
 * So: regular IO calls 'wait_barrier'.  When that returns there
 *    is no backgroup IO happening,  It must arrange to call
 *    allow_barrier when it has finished its IO.
 * backgroup IO calls must call raise_barrier.  Once that returns
 *    there is no normal IO happeing.  It must arrange to call
 *    lower_barrier when the particular background IO completes.
 */

static void raise_barrier(struct r10conf *conf, int force)
{
	write_seqlock_irq(&conf->resync_lock);

	if (WARN_ON_ONCE(force && !conf->barrier))
		force = false;

	/* Wait until no block IO is waiting (unless 'force') */
	wait_event_barrier(conf, force || !conf->nr_waiting);

	/* block any new IO from starting */
	WRITE_ONCE(conf->barrier, conf->barrier + 1);

	/* Now wait for all pending IO to complete */
	wait_event_barrier(conf, !atomic_read(&conf->nr_pending) &&
				 conf->barrier < RESYNC_DEPTH);

	write_sequnlock_irq(&conf->resync_lock);
}

static void lower_barrier(struct r10conf *conf)
{
	unsigned long flags;

	write_seqlock_irqsave(&conf->resync_lock, flags);
	WRITE_ONCE(conf->barrier, conf->barrier - 1);
	write_sequnlock_irqrestore(&conf->resync_lock, flags);
	wake_up(&conf->wait_barrier);
}

static bool stop_waiting_barrier(struct r10conf *conf)
{
	struct bio_list *bio_list = current->bio_list;
	struct md_thread *thread;

	/* barrier is dropped */
	if (!conf->barrier)
		return true;

	/*
	 * If there are already pending requests (preventing the barrier from
	 * rising completely), and the pre-process bio queue isn't empty, then
	 * don't wait, as we need to empty that queue to get the nr_pending
	 * count down.
	 */
	if (atomic_read(&conf->nr_pending) && bio_list &&
	    (!bio_list_empty(&bio_list[0]) || !bio_list_empty(&bio_list[1])))
		return true;

	/* daemon thread must exist while handling io */
	thread = rcu_dereference_protected(conf->mddev->thread, true);
	/*
	 * move on if io is issued from raid10d(), nr_pending is not released
	 * from original io(see handle_read_error()). All raise barrier is
	 * blocked until this io is done.
	 */
	if (thread->tsk == current) {
		WARN_ON_ONCE(atomic_read(&conf->nr_pending) == 0);
		return true;
	}

	return false;
}

static bool wait_barrier_nolock(struct r10conf *conf)
{
	unsigned int seq = read_seqbegin(&conf->resync_lock);

	if (READ_ONCE(conf->barrier))
		return false;

	atomic_inc(&conf->nr_pending);
	if (!read_seqretry(&conf->resync_lock, seq))
		return true;

	if (atomic_dec_and_test(&conf->nr_pending))
		wake_up_barrier(conf);

	return false;
}

static bool wait_barrier(struct r10conf *conf, bool nowait)
{
	bool ret = true;

	if (wait_barrier_nolock(conf))
		return true;

	write_seqlock_irq(&conf->resync_lock);
	if (conf->barrier) {
		/* Return false when nowait flag is set */
		if (nowait) {
			ret = false;
		} else {
			conf->nr_waiting++;
			mddev_add_trace_msg(conf->mddev, "raid10 wait barrier");
			wait_event_barrier(conf, stop_waiting_barrier(conf));
			conf->nr_waiting--;
		}
		if (!conf->nr_waiting)
			wake_up(&conf->wait_barrier);
	}
	/* Only increment nr_pending when we wait */
	if (ret)
		atomic_inc(&conf->nr_pending);
	write_sequnlock_irq(&conf->resync_lock);
	return ret;
}

static void allow_barrier(struct r10conf *conf)
{
	if ((atomic_dec_and_test(&conf->nr_pending)) ||
			(conf->array_freeze_pending))
		wake_up_barrier(conf);
}

static void freeze_array(struct r10conf *conf, int extra)
{
	/* stop syncio and normal IO and wait for everything to
	 * go quiet.
	 * We increment barrier and nr_waiting, and then
	 * wait until nr_pending match nr_queued+extra
	 * This is called in the context of one normal IO request
	 * that has failed. Thus any sync request that might be pending
	 * will be blocked by nr_pending, and we need to wait for
	 * pending IO requests to complete or be queued for re-try.
	 * Thus the number queued (nr_queued) plus this request (extra)
	 * must match the number of pending IOs (nr_pending) before
	 * we continue.
	 */
	write_seqlock_irq(&conf->resync_lock);
	conf->array_freeze_pending++;
	WRITE_ONCE(conf->barrier, conf->barrier + 1);
	conf->nr_waiting++;
	wait_event_barrier_cmd(conf, atomic_read(&conf->nr_pending) ==
			conf->nr_queued + extra, flush_pending_writes(conf));
	conf->array_freeze_pending--;
	write_sequnlock_irq(&conf->resync_lock);
}

static void unfreeze_array(struct r10conf *conf)
{
	/* reverse the effect of the freeze */
	write_seqlock_irq(&conf->resync_lock);
	WRITE_ONCE(conf->barrier, conf->barrier - 1);
	conf->nr_waiting--;
	wake_up(&conf->wait_barrier);
	write_sequnlock_irq(&conf->resync_lock);
}

static sector_t choose_data_offset(struct r10bio *r10_bio,
				   struct md_rdev *rdev)
{
	if (!test_bit(MD_RECOVERY_RESHAPE, &rdev->mddev->recovery) ||
	    test_bit(R10BIO_Previous, &r10_bio->state))
		return rdev->data_offset;
	else
		return rdev->new_data_offset;
}

static void raid10_unplug(struct blk_plug_cb *cb, bool from_schedule)
{
	struct raid1_plug_cb *plug = container_of(cb, struct raid1_plug_cb, cb);
	struct mddev *mddev = plug->cb.data;
	struct r10conf *conf = mddev->private;
	struct bio *bio;

	if (from_schedule) {
		spin_lock_irq(&conf->device_lock);
		bio_list_merge(&conf->pending_bio_list, &plug->pending);
		spin_unlock_irq(&conf->device_lock);
		wake_up_barrier(conf);
		md_wakeup_thread(mddev->thread);
		kfree(plug);
		return;
	}

	/* we aren't scheduling, so we can do the write-out directly. */
	bio = bio_list_get(&plug->pending);
	raid1_prepare_flush_writes(mddev);
	wake_up_barrier(conf);

	while (bio) { /* submit pending writes */
		struct bio *next = bio->bi_next;

		raid1_submit_write(bio);
		bio = next;
		cond_resched();
	}
	kfree(plug);
}

/*
 * 1. Register the new request and wait if the reconstruction thread has put
 * up a bar for new requests. Continue immediately if no resync is active
 * currently.
 * 2. If IO spans the reshape position.  Need to wait for reshape to pass.
 */
static bool regular_request_wait(struct mddev *mddev, struct r10conf *conf,
				 struct bio *bio, sector_t sectors)
{
	/* Bail out if REQ_NOWAIT is set for the bio */
	if (!wait_barrier(conf, bio->bi_opf & REQ_NOWAIT)) {
		bio_wouldblock_error(bio);
		return false;
	}
	while (test_bit(MD_RECOVERY_RESHAPE, &mddev->recovery) &&
	    bio->bi_iter.bi_sector < conf->reshape_progress &&
	    bio->bi_iter.bi_sector + sectors > conf->reshape_progress) {
		allow_barrier(conf);
		if (bio->bi_opf & REQ_NOWAIT) {
			bio_wouldblock_error(bio);
			return false;
		}
		mddev_add_trace_msg(conf->mddev, "raid10 wait reshape");
		wait_event(conf->wait_barrier,
			   conf->reshape_progress <= bio->bi_iter.bi_sector ||
			   conf->reshape_progress >= bio->bi_iter.bi_sector +
			   sectors);
		wait_barrier(conf, false);
	}
	return true;
}

static void raid10_read_request(struct mddev *mddev, struct bio *bio,
				struct r10bio *r10_bio, bool io_accounting)
{
	struct r10conf *conf = mddev->private;
	struct bio *read_bio;
	int max_sectors;
	struct md_rdev *rdev;
	char b[BDEVNAME_SIZE];
	int slot = r10_bio->read_slot;
	struct md_rdev *err_rdev = NULL;
	gfp_t gfp = GFP_NOIO;
	int error;

	if (slot >= 0 && r10_bio->devs[slot].rdev) {
		/*
		 * This is an error retry, but we cannot
		 * safely dereference the rdev in the r10_bio,
		 * we must use the one in conf.
		 * If it has already been disconnected (unlikely)
		 * we lose the device name in error messages.
		 */
		int disk;
		/*
		 * As we are blocking raid10, it is a little safer to
		 * use __GFP_HIGH.
		 */
		gfp = GFP_NOIO | __GFP_HIGH;

		disk = r10_bio->devs[slot].devnum;
		err_rdev = conf->mirrors[disk].rdev;
		if (err_rdev)
			snprintf(b, sizeof(b), "%pg", err_rdev->bdev);
		else {
			strcpy(b, "???");
			/* This never gets dereferenced */
			err_rdev = r10_bio->devs[slot].rdev;
		}
	}

	if (!regular_request_wait(mddev, conf, bio, r10_bio->sectors)) {
		raid_end_bio_io(r10_bio);
		return;
	}

	rdev = read_balance(conf, r10_bio, &max_sectors);
	if (!rdev) {
		if (err_rdev) {
			pr_crit_ratelimited("md/raid10:%s: %s: unrecoverable I/O read error for block %llu\n",
					    mdname(mddev), b,
					    (unsigned long long)r10_bio->sector);
		}
		raid_end_bio_io(r10_bio);
		return;
	}
	if (err_rdev)
		pr_err_ratelimited("md/raid10:%s: %pg: redirecting sector %llu to another mirror\n",
				   mdname(mddev),
				   rdev->bdev,
				   (unsigned long long)r10_bio->sector);
	if (max_sectors < bio_sectors(bio)) {
		struct bio *split = bio_split(bio, max_sectors,
					      gfp, &conf->bio_split);
		if (IS_ERR(split)) {
			error = PTR_ERR(split);
			goto err_handle;
		}
		bio_chain(split, bio);
		allow_barrier(conf);
		submit_bio_noacct(bio);
		wait_barrier(conf, false);
		bio = split;
		r10_bio->master_bio = bio;
		r10_bio->sectors = max_sectors;
	}
	slot = r10_bio->read_slot;

	if (io_accounting) {
		md_account_bio(mddev, &bio);
		r10_bio->master_bio = bio;
	}
	read_bio = bio_alloc_clone(rdev->bdev, bio, gfp, &mddev->bio_set);
	read_bio->bi_opf &= ~REQ_NOWAIT;

	r10_bio->devs[slot].bio = read_bio;
	r10_bio->devs[slot].rdev = rdev;

	read_bio->bi_iter.bi_sector = r10_bio->devs[slot].addr +
		choose_data_offset(r10_bio, rdev);
	read_bio->bi_end_io = raid10_end_read_request;
	if (test_bit(FailFast, &rdev->flags) &&
	    test_bit(R10BIO_FailFast, &r10_bio->state))
	        read_bio->bi_opf |= MD_FAILFAST;
	read_bio->bi_private = r10_bio;
	mddev_trace_remap(mddev, read_bio, r10_bio->sector);
	submit_bio_noacct(read_bio);
	return;
err_handle:
	atomic_dec(&rdev->nr_pending);
	bio->bi_status = errno_to_blk_status(error);
	set_bit(R10BIO_Uptodate, &r10_bio->state);
	raid_end_bio_io(r10_bio);
}

static void raid10_write_one_disk(struct mddev *mddev, struct r10bio *r10_bio,
				  struct bio *bio, bool replacement,
				  int n_copy)
{
	unsigned long flags;
	struct r10conf *conf = mddev->private;
	struct md_rdev *rdev;
	int devnum = r10_bio->devs[n_copy].devnum;
	struct bio *mbio;

	rdev = replacement ? conf->mirrors[devnum].replacement :
			     conf->mirrors[devnum].rdev;

	mbio = bio_alloc_clone(rdev->bdev, bio, GFP_NOIO, &mddev->bio_set);
	mbio->bi_opf &= ~REQ_NOWAIT;
	if (replacement)
		r10_bio->devs[n_copy].repl_bio = mbio;
	else
		r10_bio->devs[n_copy].bio = mbio;

	mbio->bi_iter.bi_sector	= (r10_bio->devs[n_copy].addr +
				   choose_data_offset(r10_bio, rdev));
	mbio->bi_end_io	= raid10_end_write_request;
	if (!replacement && test_bit(FailFast,
				     &conf->mirrors[devnum].rdev->flags)
			 && enough(conf, devnum))
		mbio->bi_opf |= MD_FAILFAST;
	mbio->bi_private = r10_bio;
	mddev_trace_remap(mddev, mbio, r10_bio->sector);
	/* flush_pending_writes() needs access to the rdev so...*/
	mbio->bi_bdev = (void *)rdev;

	atomic_inc(&r10_bio->remaining);

	if (!raid1_add_bio_to_plug(mddev, mbio, raid10_unplug, conf->copies)) {
		spin_lock_irqsave(&conf->device_lock, flags);
		bio_list_add(&conf->pending_bio_list, mbio);
		spin_unlock_irqrestore(&conf->device_lock, flags);
		md_wakeup_thread(mddev->thread);
	}
}

static void wait_blocked_dev(struct mddev *mddev, struct r10bio *r10_bio)
{
	struct r10conf *conf = mddev->private;
	struct md_rdev *blocked_rdev;
	int i;

retry_wait:
	blocked_rdev = NULL;
	for (i = 0; i < conf->copies; i++) {
		struct md_rdev *rdev, *rrdev;

		rdev = conf->mirrors[i].rdev;
		if (rdev) {
			sector_t dev_sector = r10_bio->devs[i].addr;

			/*
			 * Discard request doesn't care the write result
			 * so it doesn't need to wait blocked disk here.
			 */
			if (test_bit(WriteErrorSeen, &rdev->flags) &&
			    r10_bio->sectors &&
			    rdev_has_badblock(rdev, dev_sector,
					      r10_bio->sectors) < 0)
				/*
				 * Mustn't write here until the bad
				 * block is acknowledged
				 */
				set_bit(BlockedBadBlocks, &rdev->flags);

			if (rdev_blocked(rdev)) {
				blocked_rdev = rdev;
				atomic_inc(&rdev->nr_pending);
				break;
			}
		}

		rrdev = conf->mirrors[i].replacement;
		if (rrdev && rdev_blocked(rrdev)) {
			atomic_inc(&rrdev->nr_pending);
			blocked_rdev = rrdev;
			break;
		}
	}

	if (unlikely(blocked_rdev)) {
		/* Have to wait for this device to get unblocked, then retry */
		allow_barrier(conf);
		mddev_add_trace_msg(conf->mddev,
			"raid10 %s wait rdev %d blocked",
			__func__, blocked_rdev->raid_disk);
		md_wait_for_blocked_rdev(blocked_rdev, mddev);
		wait_barrier(conf, false);
		goto retry_wait;
	}
}

static void raid10_write_request(struct mddev *mddev, struct bio *bio,
				 struct r10bio *r10_bio)
{
	struct r10conf *conf = mddev->private;
	int i, k;
	sector_t sectors;
	int max_sectors;
	int error;

	if ((mddev_is_clustered(mddev) &&
	     mddev->cluster_ops->area_resyncing(mddev, WRITE,
						bio->bi_iter.bi_sector,
						bio_end_sector(bio)))) {
		DEFINE_WAIT(w);
		/* Bail out if REQ_NOWAIT is set for the bio */
		if (bio->bi_opf & REQ_NOWAIT) {
			bio_wouldblock_error(bio);
			return;
		}
		for (;;) {
			prepare_to_wait(&conf->wait_barrier,
					&w, TASK_IDLE);
			if (!mddev->cluster_ops->area_resyncing(mddev, WRITE,
				 bio->bi_iter.bi_sector, bio_end_sector(bio)))
				break;
			schedule();
		}
		finish_wait(&conf->wait_barrier, &w);
	}

	sectors = r10_bio->sectors;
	if (!regular_request_wait(mddev, conf, bio, sectors)) {
		raid_end_bio_io(r10_bio);
		return;
	}

	if (test_bit(MD_RECOVERY_RESHAPE, &mddev->recovery) &&
	    (mddev->reshape_backwards
	     ? (bio->bi_iter.bi_sector < conf->reshape_safe &&
		bio->bi_iter.bi_sector + sectors > conf->reshape_progress)
	     : (bio->bi_iter.bi_sector + sectors > conf->reshape_safe &&
		bio->bi_iter.bi_sector < conf->reshape_progress))) {
		/* Need to update reshape_position in metadata */
		mddev->reshape_position = conf->reshape_progress;
		set_mask_bits(&mddev->sb_flags, 0,
			      BIT(MD_SB_CHANGE_DEVS) | BIT(MD_SB_CHANGE_PENDING));
		md_wakeup_thread(mddev->thread);
		if (bio->bi_opf & REQ_NOWAIT) {
			allow_barrier(conf);
			bio_wouldblock_error(bio);
			return;
		}
		mddev_add_trace_msg(conf->mddev,
			"raid10 wait reshape metadata");
		wait_event(mddev->sb_wait,
			   !test_bit(MD_SB_CHANGE_PENDING, &mddev->sb_flags));

		conf->reshape_safe = mddev->reshape_position;
	}

	/* first select target devices under rcu_lock and
	 * inc refcount on their rdev.  Record them by setting
	 * bios[x] to bio
	 * If there are known/acknowledged bad blocks on any device
	 * on which we have seen a write error, we want to avoid
	 * writing to those blocks.  This potentially requires several
	 * writes to write around the bad blocks.  Each set of writes
	 * gets its own r10_bio with a set of bios attached.
	 */

	r10_bio->read_slot = -1; /* make sure repl_bio gets freed */
	raid10_find_phys(conf, r10_bio);

	wait_blocked_dev(mddev, r10_bio);

	max_sectors = r10_bio->sectors;

	for (i = 0;  i < conf->copies; i++) {
		int d = r10_bio->devs[i].devnum;
		struct md_rdev *rdev, *rrdev;

		rdev = conf->mirrors[d].rdev;
		rrdev = conf->mirrors[d].replacement;
		if (rdev && (test_bit(Faulty, &rdev->flags)))
			rdev = NULL;
		if (rrdev && (test_bit(Faulty, &rrdev->flags)))
			rrdev = NULL;

		r10_bio->devs[i].bio = NULL;
		r10_bio->devs[i].repl_bio = NULL;

		if (!rdev && !rrdev)
			continue;
		if (rdev && test_bit(WriteErrorSeen, &rdev->flags)) {
			sector_t first_bad;
			sector_t dev_sector = r10_bio->devs[i].addr;
			sector_t bad_sectors;
			int is_bad;

			is_bad = is_badblock(rdev, dev_sector, max_sectors,
					     &first_bad, &bad_sectors);
			if (is_bad && first_bad <= dev_sector) {
				/* Cannot write here at all */
				bad_sectors -= (dev_sector - first_bad);
				if (bad_sectors < max_sectors)
					/* Mustn't write more than bad_sectors
					 * to other devices yet
					 */
					max_sectors = bad_sectors;
				continue;
			}
			if (is_bad) {
				int good_sectors;

				/*
				 * We cannot atomically write this, so just
				 * error in that case. It could be possible to
				 * atomically write other mirrors, but the
				 * complexity of supporting that is not worth
				 * the benefit.
				 */
				if (bio->bi_opf & REQ_ATOMIC) {
					error = -EIO;
					goto err_handle;
				}

				good_sectors = first_bad - dev_sector;
				if (good_sectors < max_sectors)
					max_sectors = good_sectors;
			}
		}
		if (rdev) {
			r10_bio->devs[i].bio = bio;
			atomic_inc(&rdev->nr_pending);
		}
		if (rrdev) {
			r10_bio->devs[i].repl_bio = bio;
			atomic_inc(&rrdev->nr_pending);
		}
	}

	if (max_sectors < r10_bio->sectors)
		r10_bio->sectors = max_sectors;

	if (r10_bio->sectors < bio_sectors(bio)) {
		struct bio *split = bio_split(bio, r10_bio->sectors,
					      GFP_NOIO, &conf->bio_split);
		if (IS_ERR(split)) {
			error = PTR_ERR(split);
			goto err_handle;
		}
		bio_chain(split, bio);
		allow_barrier(conf);
		submit_bio_noacct(bio);
		wait_barrier(conf, false);
		bio = split;
		r10_bio->master_bio = bio;
	}

	md_account_bio(mddev, &bio);
	r10_bio->master_bio = bio;
	atomic_set(&r10_bio->remaining, 1);

	for (i = 0; i < conf->copies; i++) {
		if (r10_bio->devs[i].bio)
			raid10_write_one_disk(mddev, r10_bio, bio, false, i);
		if (r10_bio->devs[i].repl_bio)
			raid10_write_one_disk(mddev, r10_bio, bio, true, i);
	}
	one_write_done(r10_bio);
	return;
err_handle:
	for (k = 0;  k < i; k++) {
		int d = r10_bio->devs[k].devnum;
		struct md_rdev *rdev = conf->mirrors[d].rdev;
		struct md_rdev *rrdev = conf->mirrors[d].replacement;

		if (r10_bio->devs[k].bio) {
			rdev_dec_pending(rdev, mddev);
			r10_bio->devs[k].bio = NULL;
		}
		if (r10_bio->devs[k].repl_bio) {
			rdev_dec_pending(rrdev, mddev);
			r10_bio->devs[k].repl_bio = NULL;
		}
	}

	bio->bi_status = errno_to_blk_status(error);
	set_bit(R10BIO_Uptodate, &r10_bio->state);
	raid_end_bio_io(r10_bio);
}

static void __make_request(struct mddev *mddev, struct bio *bio, int sectors)
{
	struct r10conf *conf = mddev->private;
	struct r10bio *r10_bio;

	r10_bio = mempool_alloc(&conf->r10bio_pool, GFP_NOIO);

	r10_bio->master_bio = bio;
	r10_bio->sectors = sectors;

	r10_bio->mddev = mddev;
	r10_bio->sector = bio->bi_iter.bi_sector;
	r10_bio->state = 0;
	r10_bio->read_slot = -1;
	memset(r10_bio->devs, 0, sizeof(r10_bio->devs[0]) *
			conf->geo.raid_disks);

	if (bio_data_dir(bio) == READ)
		raid10_read_request(mddev, bio, r10_bio, true);
	else
		raid10_write_request(mddev, bio, r10_bio);
}

static void raid_end_discard_bio(struct r10bio *r10bio)
{
	struct r10conf *conf = r10bio->mddev->private;
	struct r10bio *first_r10bio;

	while (atomic_dec_and_test(&r10bio->remaining)) {

		allow_barrier(conf);

		if (!test_bit(R10BIO_Discard, &r10bio->state)) {
			first_r10bio = (struct r10bio *)r10bio->master_bio;
			free_r10bio(r10bio);
			r10bio = first_r10bio;
		} else {
			md_write_end(r10bio->mddev);
			bio_endio(r10bio->master_bio);
			free_r10bio(r10bio);
			break;
		}
	}
}

static void raid10_end_discard_request(struct bio *bio)
{
	struct r10bio *r10_bio = bio->bi_private;
	struct r10conf *conf = r10_bio->mddev->private;
	struct md_rdev *rdev = NULL;
	int dev;
	int slot, repl;

	/*
	 * We don't care the return value of discard bio
	 */
	if (!test_bit(R10BIO_Uptodate, &r10_bio->state))
		set_bit(R10BIO_Uptodate, &r10_bio->state);

	dev = find_bio_disk(conf, r10_bio, bio, &slot, &repl);
	rdev = repl ? conf->mirrors[dev].replacement :
		      conf->mirrors[dev].rdev;

	raid_end_discard_bio(r10_bio);
	rdev_dec_pending(rdev, conf->mddev);
}

/*
 * There are some limitations to handle discard bio
 * 1st, the discard size is bigger than stripe_size*2.
 * 2st, if the discard bio spans reshape progress, we use the old way to
 * handle discard bio
 */
static int raid10_handle_discard(struct mddev *mddev, struct bio *bio)
{
	struct r10conf *conf = mddev->private;
	struct geom *geo = &conf->geo;
	int far_copies = geo->far_copies;
	bool first_copy = true;
	struct r10bio *r10_bio, *first_r10bio;
	struct bio *split;
	int disk;
	sector_t chunk;
	unsigned int stripe_size;
	unsigned int stripe_data_disks;
	sector_t split_size;
	sector_t bio_start, bio_end;
	sector_t first_stripe_index, last_stripe_index;
	sector_t start_disk_offset;
	unsigned int start_disk_index;
	sector_t end_disk_offset;
	unsigned int end_disk_index;
	unsigned int remainder;

	if (test_bit(MD_RECOVERY_RESHAPE, &mddev->recovery))
		return -EAGAIN;

	if (!wait_barrier(conf, bio->bi_opf & REQ_NOWAIT)) {
		bio_wouldblock_error(bio);
		return 0;
	}

	/*
	 * Check reshape again to avoid reshape happens after checking
	 * MD_RECOVERY_RESHAPE and before wait_barrier
	 */
	if (test_bit(MD_RECOVERY_RESHAPE, &mddev->recovery))
		goto out;

	if (geo->near_copies)
		stripe_data_disks = geo->raid_disks / geo->near_copies +
					geo->raid_disks % geo->near_copies;
	else
		stripe_data_disks = geo->raid_disks;

	stripe_size = stripe_data_disks << geo->chunk_shift;

	bio_start = bio->bi_iter.bi_sector;
	bio_end = bio_end_sector(bio);

	/*
	 * Maybe one discard bio is smaller than strip size or across one
	 * stripe and discard region is larger than one stripe size. For far
	 * offset layout, if the discard region is not aligned with stripe
	 * size, there is hole when we submit discard bio to member disk.
	 * For simplicity, we only handle discard bio which discard region
	 * is bigger than stripe_size * 2
	 */
	if (bio_sectors(bio) < stripe_size*2)
		goto out;

	/*
	 * Keep bio aligned with strip size.
	 */
	div_u64_rem(bio_start, stripe_size, &remainder);
	if (remainder) {
		split_size = stripe_size - remainder;
		split = bio_split(bio, split_size, GFP_NOIO, &conf->bio_split);
		if (IS_ERR(split)) {
			bio->bi_status = errno_to_blk_status(PTR_ERR(split));
			bio_endio(bio);
			return 0;
		}
		bio_chain(split, bio);
		allow_barrier(conf);
		/* Resend the fist split part */
		submit_bio_noacct(split);
		wait_barrier(conf, false);
	}
	div_u64_rem(bio_end, stripe_size, &remainder);
	if (remainder) {
		split_size = bio_sectors(bio) - remainder;
		split = bio_split(bio, split_size, GFP_NOIO, &conf->bio_split);
		if (IS_ERR(split)) {
			bio->bi_status = errno_to_blk_status(PTR_ERR(split));
			bio_endio(bio);
			return 0;
		}
		bio_chain(split, bio);
		allow_barrier(conf);
		/* Resend the second split part */
		submit_bio_noacct(bio);
		bio = split;
		wait_barrier(conf, false);
	}

	bio_start = bio->bi_iter.bi_sector;
	bio_end = bio_end_sector(bio);

	/*
	 * Raid10 uses chunk as the unit to store data. It's similar like raid0.
	 * One stripe contains the chunks from all member disk (one chunk from
	 * one disk at the same HBA address). For layout detail, see 'man md 4'
	 */
	chunk = bio_start >> geo->chunk_shift;
	chunk *= geo->near_copies;
	first_stripe_index = chunk;
	start_disk_index = sector_div(first_stripe_index, geo->raid_disks);
	if (geo->far_offset)
		first_stripe_index *= geo->far_copies;
	start_disk_offset = (bio_start & geo->chunk_mask) +
				(first_stripe_index << geo->chunk_shift);

	chunk = bio_end >> geo->chunk_shift;
	chunk *= geo->near_copies;
	last_stripe_index = chunk;
	end_disk_index = sector_div(last_stripe_index, geo->raid_disks);
	if (geo->far_offset)
		last_stripe_index *= geo->far_copies;
	end_disk_offset = (bio_end & geo->chunk_mask) +
				(last_stripe_index << geo->chunk_shift);

retry_discard:
	r10_bio = mempool_alloc(&conf->r10bio_pool, GFP_NOIO);
	r10_bio->mddev = mddev;
	r10_bio->state = 0;
	r10_bio->sectors = 0;
	memset(r10_bio->devs, 0, sizeof(r10_bio->devs[0]) * geo->raid_disks);
	wait_blocked_dev(mddev, r10_bio);

	/*
	 * For far layout it needs more than one r10bio to cover all regions.
	 * Inspired by raid10_sync_request, we can use the first r10bio->master_bio
	 * to record the discard bio. Other r10bio->master_bio record the first
	 * r10bio. The first r10bio only release after all other r10bios finish.
	 * The discard bio returns only first r10bio finishes
	 */
	if (first_copy) {
		md_account_bio(mddev, &bio);
		r10_bio->master_bio = bio;
		set_bit(R10BIO_Discard, &r10_bio->state);
		first_copy = false;
		first_r10bio = r10_bio;
	} else
		r10_bio->master_bio = (struct bio *)first_r10bio;

	/*
	 * first select target devices under rcu_lock and
	 * inc refcount on their rdev.  Record them by setting
	 * bios[x] to bio
	 */
	for (disk = 0; disk < geo->raid_disks; disk++) {
		struct md_rdev *rdev, *rrdev;

		rdev = conf->mirrors[disk].rdev;
		rrdev = conf->mirrors[disk].replacement;
		r10_bio->devs[disk].bio = NULL;
		r10_bio->devs[disk].repl_bio = NULL;

		if (rdev && (test_bit(Faulty, &rdev->flags)))
			rdev = NULL;
		if (rrdev && (test_bit(Faulty, &rrdev->flags)))
			rrdev = NULL;
		if (!rdev && !rrdev)
			continue;

		if (rdev) {
			r10_bio->devs[disk].bio = bio;
			atomic_inc(&rdev->nr_pending);
		}
		if (rrdev) {
			r10_bio->devs[disk].repl_bio = bio;
			atomic_inc(&rrdev->nr_pending);
		}
	}

	atomic_set(&r10_bio->remaining, 1);
	for (disk = 0; disk < geo->raid_disks; disk++) {
		sector_t dev_start, dev_end;
		struct bio *mbio, *rbio = NULL;

		/*
		 * Now start to calculate the start and end address for each disk.
		 * The space between dev_start and dev_end is the discard region.
		 *
		 * For dev_start, it needs to consider three conditions:
		 * 1st, the disk is before start_disk, you can imagine the disk in
		 * the next stripe. So the dev_start is the start address of next
		 * stripe.
		 * 2st, the disk is after start_disk, it means the disk is at the
		 * same stripe of first disk
		 * 3st, the first disk itself, we can use start_disk_offset directly
		 */
		if (disk < start_disk_index)
			dev_start = (first_stripe_index + 1) * mddev->chunk_sectors;
		else if (disk > start_disk_index)
			dev_start = first_stripe_index * mddev->chunk_sectors;
		else
			dev_start = start_disk_offset;

		if (disk < end_disk_index)
			dev_end = (last_stripe_index + 1) * mddev->chunk_sectors;
		else if (disk > end_disk_index)
			dev_end = last_stripe_index * mddev->chunk_sectors;
		else
			dev_end = end_disk_offset;

		/*
		 * It only handles discard bio which size is >= stripe size, so
		 * dev_end > dev_start all the time.
		 * It doesn't need to use rcu lock to get rdev here. We already
		 * add rdev->nr_pending in the first loop.
		 */
		if (r10_bio->devs[disk].bio) {
			struct md_rdev *rdev = conf->mirrors[disk].rdev;
			mbio = bio_alloc_clone(bio->bi_bdev, bio, GFP_NOIO,
					       &mddev->bio_set);
			mbio->bi_end_io = raid10_end_discard_request;
			mbio->bi_private = r10_bio;
			r10_bio->devs[disk].bio = mbio;
			r10_bio->devs[disk].devnum = disk;
			atomic_inc(&r10_bio->remaining);
			md_submit_discard_bio(mddev, rdev, mbio,
					dev_start + choose_data_offset(r10_bio, rdev),
					dev_end - dev_start);
			bio_endio(mbio);
		}
		if (r10_bio->devs[disk].repl_bio) {
			struct md_rdev *rrdev = conf->mirrors[disk].replacement;
			rbio = bio_alloc_clone(bio->bi_bdev, bio, GFP_NOIO,
					       &mddev->bio_set);
			rbio->bi_end_io = raid10_end_discard_request;
			rbio->bi_private = r10_bio;
			r10_bio->devs[disk].repl_bio = rbio;
			r10_bio->devs[disk].devnum = disk;
			atomic_inc(&r10_bio->remaining);
			md_submit_discard_bio(mddev, rrdev, rbio,
					dev_start + choose_data_offset(r10_bio, rrdev),
					dev_end - dev_start);
			bio_endio(rbio);
		}
	}

	if (!geo->far_offset && --far_copies) {
		first_stripe_index += geo->stride >> geo->chunk_shift;
		start_disk_offset += geo->stride;
		last_stripe_index += geo->stride >> geo->chunk_shift;
		end_disk_offset += geo->stride;
		atomic_inc(&first_r10bio->remaining);
		raid_end_discard_bio(r10_bio);
		wait_barrier(conf, false);
		goto retry_discard;
	}

	raid_end_discard_bio(r10_bio);

	return 0;
out:
	allow_barrier(conf);
	return -EAGAIN;
}

static bool raid10_make_request(struct mddev *mddev, struct bio *bio)
{
	struct r10conf *conf = mddev->private;
	sector_t chunk_mask = (conf->geo.chunk_mask & conf->prev.chunk_mask);
	int chunk_sects = chunk_mask + 1;
	int sectors = bio_sectors(bio);

	if (unlikely(bio->bi_opf & REQ_PREFLUSH)
	    && md_flush_request(mddev, bio))
		return true;

	md_write_start(mddev, bio);

	if (unlikely(bio_op(bio) == REQ_OP_DISCARD))
		if (!raid10_handle_discard(mddev, bio))
			return true;

	/*
	 * If this request crosses a chunk boundary, we need to split
	 * it.
	 */
	if (unlikely((bio->bi_iter.bi_sector & chunk_mask) +
		     sectors > chunk_sects
		     && (conf->geo.near_copies < conf->geo.raid_disks
			 || conf->prev.near_copies <
			 conf->prev.raid_disks)))
		sectors = chunk_sects -
			(bio->bi_iter.bi_sector &
			 (chunk_sects - 1));
	__make_request(mddev, bio, sectors);

	/* In case raid10d snuck in to freeze_array */
	wake_up_barrier(conf);
	return true;
}

static void raid10_status(struct seq_file *seq, struct mddev *mddev)
{
	struct r10conf *conf = mddev->private;
	int i;

	lockdep_assert_held(&mddev->lock);

	if (conf->geo.near_copies < conf->geo.raid_disks)
		seq_printf(seq, " %dK chunks", mddev->chunk_sectors / 2);
	if (conf->geo.near_copies > 1)
		seq_printf(seq, " %d near-copies", conf->geo.near_copies);
	if (conf->geo.far_copies > 1) {
		if (conf->geo.far_offset)
			seq_printf(seq, " %d offset-copies", conf->geo.far_copies);
		else
			seq_printf(seq, " %d far-copies", conf->geo.far_copies);
		if (conf->geo.far_set_size != conf->geo.raid_disks)
			seq_printf(seq, " %d devices per set", conf->geo.far_set_size);
	}
	seq_printf(seq, " [%d/%d] [", conf->geo.raid_disks,
					conf->geo.raid_disks - mddev->degraded);
	for (i = 0; i < conf->geo.raid_disks; i++) {
		struct md_rdev *rdev = READ_ONCE(conf->mirrors[i].rdev);

		seq_printf(seq, "%s", rdev && test_bit(In_sync, &rdev->flags) ? "U" : "_");
	}
	seq_printf(seq, "]");
}

/* check if there are enough drives for
 * every block to appear on atleast one.
 * Don't consider the device numbered 'ignore'
 * as we might be about to remove it.
 */
static int _enough(struct r10conf *conf, int previous, int ignore)
{
	int first = 0;
	int has_enough = 0;
	int disks, ncopies;
	if (previous) {
		disks = conf->prev.raid_disks;
		ncopies = conf->prev.near_copies;
	} else {
		disks = conf->geo.raid_disks;
		ncopies = conf->geo.near_copies;
	}

	do {
		int n = conf->copies;
		int cnt = 0;
		int this = first;
		while (n--) {
			struct md_rdev *rdev;
			if (this != ignore &&
			    (rdev = conf->mirrors[this].rdev) &&
			    test_bit(In_sync, &rdev->flags))
				cnt++;
			this = (this+1) % disks;
		}
		if (cnt == 0)
			goto out;
		first = (first + ncopies) % disks;
	} while (first != 0);
	has_enough = 1;
out:
	return has_enough;
}

static int enough(struct r10conf *conf, int ignore)
{
	/* when calling 'enough', both 'prev' and 'geo' must
	 * be stable.
	 * This is ensured if ->reconfig_mutex or ->device_lock
	 * is held.
	 */
	return _enough(conf, 0, ignore) &&
		_enough(conf, 1, ignore);
}

/**
 * raid10_error() - RAID10 error handler.
 * @mddev: affected md device.
 * @rdev: member device to fail.
 *
 * The routine acknowledges &rdev failure and determines new @mddev state.
 * If it failed, then:
 *	- &MD_BROKEN flag is set in &mddev->flags.
 * Otherwise, it must be degraded:
 *	- recovery is interrupted.
 *	- &mddev->degraded is bumped.
 *
 * @rdev is marked as &Faulty excluding case when array is failed and
 * &mddev->fail_last_dev is off.
 */
static void raid10_error(struct mddev *mddev, struct md_rdev *rdev)
{
	struct r10conf *conf = mddev->private;
	unsigned long flags;

	spin_lock_irqsave(&conf->device_lock, flags);

	if (test_bit(In_sync, &rdev->flags) && !enough(conf, rdev->raid_disk)) {
		set_bit(MD_BROKEN, &mddev->flags);

		if (!mddev->fail_last_dev) {
			spin_unlock_irqrestore(&conf->device_lock, flags);
			return;
		}
	}
	if (test_and_clear_bit(In_sync, &rdev->flags))
		mddev->degraded++;

	set_bit(MD_RECOVERY_INTR, &mddev->recovery);
	set_bit(Blocked, &rdev->flags);
	set_bit(Faulty, &rdev->flags);
	set_mask_bits(&mddev->sb_flags, 0,
		      BIT(MD_SB_CHANGE_DEVS) | BIT(MD_SB_CHANGE_PENDING));
	spin_unlock_irqrestore(&conf->device_lock, flags);
	pr_crit("md/raid10:%s: Disk failure on %pg, disabling device.\n"
		"md/raid10:%s: Operation continuing on %d devices.\n",
		mdname(mddev), rdev->bdev,
		mdname(mddev), conf->geo.raid_disks - mddev->degraded);
}

static void print_conf(struct r10conf *conf)
{
	int i;
	struct md_rdev *rdev;

	pr_debug("RAID10 conf printout:\n");
	if (!conf) {
		pr_debug("(!conf)\n");
		return;
	}
	pr_debug(" --- wd:%d rd:%d\n", conf->geo.raid_disks - conf->mddev->degraded,
		 conf->geo.raid_disks);

	lockdep_assert_held(&conf->mddev->reconfig_mutex);
	for (i = 0; i < conf->geo.raid_disks; i++) {
		rdev = conf->mirrors[i].rdev;
		if (rdev)
			pr_debug(" disk %d, wo:%d, o:%d, dev:%pg\n",
				 i, !test_bit(In_sync, &rdev->flags),
				 !test_bit(Faulty, &rdev->flags),
				 rdev->bdev);
	}
}

static void close_sync(struct r10conf *conf)
{
	wait_barrier(conf, false);
	allow_barrier(conf);

	mempool_exit(&conf->r10buf_pool);
}

static int raid10_spare_active(struct mddev *mddev)
{
	int i;
	struct r10conf *conf = mddev->private;
	struct raid10_info *tmp;
	int count = 0;
	unsigned long flags;

	/*
	 * Find all non-in_sync disks within the RAID10 configuration
	 * and mark them in_sync
	 */
	for (i = 0; i < conf->geo.raid_disks; i++) {
		tmp = conf->mirrors + i;
		if (tmp->replacement
		    && tmp->replacement->recovery_offset == MaxSector
		    && !test_bit(Faulty, &tmp->replacement->flags)
		    && !test_and_set_bit(In_sync, &tmp->replacement->flags)) {
			/* Replacement has just become active */
			if (!tmp->rdev
			    || !test_and_clear_bit(In_sync, &tmp->rdev->flags))
				count++;
			if (tmp->rdev) {
				/* Replaced device not technically faulty,
				 * but we need to be sure it gets removed
				 * and never re-added.
				 */
				set_bit(Faulty, &tmp->rdev->flags);
				sysfs_notify_dirent_safe(
					tmp->rdev->sysfs_state);
			}
			sysfs_notify_dirent_safe(tmp->replacement->sysfs_state);
		} else if (tmp->rdev
			   && tmp->rdev->recovery_offset == MaxSector
			   && !test_bit(Faulty, &tmp->rdev->flags)
			   && !test_and_set_bit(In_sync, &tmp->rdev->flags)) {
			count++;
			sysfs_notify_dirent_safe(tmp->rdev->sysfs_state);
		}
	}
	spin_lock_irqsave(&conf->device_lock, flags);
	mddev->degraded -= count;
	spin_unlock_irqrestore(&conf->device_lock, flags);

	print_conf(conf);
	return count;
}

static int raid10_add_disk(struct mddev *mddev, struct md_rdev *rdev)
{
	struct r10conf *conf = mddev->private;
	int err = -EEXIST;
	int mirror, repl_slot = -1;
	int first = 0;
	int last = conf->geo.raid_disks - 1;
	struct raid10_info *p;

	if (mddev->recovery_cp < MaxSector)
		/* only hot-add to in-sync arrays, as recovery is
		 * very different from resync
		 */
		return -EBUSY;
	if (rdev->saved_raid_disk < 0 && !_enough(conf, 1, -1))
		return -EINVAL;

	if (rdev->raid_disk >= 0)
		first = last = rdev->raid_disk;

	if (rdev->saved_raid_disk >= first &&
	    rdev->saved_raid_disk < conf->geo.raid_disks &&
	    conf->mirrors[rdev->saved_raid_disk].rdev == NULL)
		mirror = rdev->saved_raid_disk;
	else
		mirror = first;
	for ( ; mirror <= last ; mirror++) {
		p = &conf->mirrors[mirror];
		if (p->recovery_disabled == mddev->recovery_disabled)
			continue;
		if (p->rdev) {
			if (test_bit(WantReplacement, &p->rdev->flags) &&
			    p->replacement == NULL && repl_slot < 0)
				repl_slot = mirror;
			continue;
		}

		err = mddev_stack_new_rdev(mddev, rdev);
		if (err)
			return err;
		p->head_position = 0;
		p->recovery_disabled = mddev->recovery_disabled - 1;
		rdev->raid_disk = mirror;
		err = 0;
		if (rdev->saved_raid_disk != mirror)
			conf->fullsync = 1;
		WRITE_ONCE(p->rdev, rdev);
		break;
	}

	if (err && repl_slot >= 0) {
		p = &conf->mirrors[repl_slot];
		clear_bit(In_sync, &rdev->flags);
		set_bit(Replacement, &rdev->flags);
		rdev->raid_disk = repl_slot;
		err = mddev_stack_new_rdev(mddev, rdev);
		if (err)
			return err;
		conf->fullsync = 1;
		WRITE_ONCE(p->replacement, rdev);
	}

	print_conf(conf);
	return err;
}

static int raid10_remove_disk(struct mddev *mddev, struct md_rdev *rdev)
{
	struct r10conf *conf = mddev->private;
	int err = 0;
	int number = rdev->raid_disk;
	struct md_rdev **rdevp;
	struct raid10_info *p;

	print_conf(conf);
	if (unlikely(number >= mddev->raid_disks))
		return 0;
	p = conf->mirrors + number;
	if (rdev == p->rdev)
		rdevp = &p->rdev;
	else if (rdev == p->replacement)
		rdevp = &p->replacement;
	else
		return 0;

	if (test_bit(In_sync, &rdev->flags) ||
	    atomic_read(&rdev->nr_pending)) {
		err = -EBUSY;
		goto abort;
	}
	/* Only remove non-faulty devices if recovery
	 * is not possible.
	 */
	if (!test_bit(Faulty, &rdev->flags) &&
	    mddev->recovery_disabled != p->recovery_disabled &&
	    (!p->replacement || p->replacement == rdev) &&
	    number < conf->geo.raid_disks &&
	    enough(conf, -1)) {
		err = -EBUSY;
		goto abort;
	}
	WRITE_ONCE(*rdevp, NULL);
	if (p->replacement) {
		/* We must have just cleared 'rdev' */
		WRITE_ONCE(p->rdev, p->replacement);
		clear_bit(Replacement, &p->replacement->flags);
		WRITE_ONCE(p->replacement, NULL);
	}

	clear_bit(WantReplacement, &rdev->flags);
	err = md_integrity_register(mddev);

abort:

	print_conf(conf);
	return err;
}

static void __end_sync_read(struct r10bio *r10_bio, struct bio *bio, int d)
{
	struct r10conf *conf = r10_bio->mddev->private;

	if (!bio->bi_status)
		set_bit(R10BIO_Uptodate, &r10_bio->state);
	else
		/* The write handler will notice the lack of
		 * R10BIO_Uptodate and record any errors etc
		 */
		atomic_add(r10_bio->sectors,
			   &conf->mirrors[d].rdev->corrected_errors);

	/* for reconstruct, we always reschedule after a read.
	 * for resync, only after all reads
	 */
	rdev_dec_pending(conf->mirrors[d].rdev, conf->mddev);
	if (test_bit(R10BIO_IsRecover, &r10_bio->state) ||
	    atomic_dec_and_test(&r10_bio->remaining)) {
		/* we have read all the blocks,
		 * do the comparison in process context in raid10d
		 */
		reschedule_retry(r10_bio);
	}
}

static void end_sync_read(struct bio *bio)
{
	struct r10bio *r10_bio = get_resync_r10bio(bio);
	struct r10conf *conf = r10_bio->mddev->private;
	int d = find_bio_disk(conf, r10_bio, bio, NULL, NULL);

	__end_sync_read(r10_bio, bio, d);
}

static void end_reshape_read(struct bio *bio)
{
	/* reshape read bio isn't allocated from r10buf_pool */
	struct r10bio *r10_bio = bio->bi_private;

	__end_sync_read(r10_bio, bio, r10_bio->read_slot);
}

static void end_sync_request(struct r10bio *r10_bio)
{
	struct mddev *mddev = r10_bio->mddev;

	while (atomic_dec_and_test(&r10_bio->remaining)) {
		if (r10_bio->master_bio == NULL) {
			/* the primary of several recovery bios */
			sector_t s = r10_bio->sectors;
			if (test_bit(R10BIO_MadeGood, &r10_bio->state) ||
			    test_bit(R10BIO_WriteError, &r10_bio->state))
				reschedule_retry(r10_bio);
			else
				put_buf(r10_bio);
			md_done_sync(mddev, s, 1);
			break;
		} else {
			struct r10bio *r10_bio2 = (struct r10bio *)r10_bio->master_bio;
			if (test_bit(R10BIO_MadeGood, &r10_bio->state) ||
			    test_bit(R10BIO_WriteError, &r10_bio->state))
				reschedule_retry(r10_bio);
			else
				put_buf(r10_bio);
			r10_bio = r10_bio2;
		}
	}
}

static void end_sync_write(struct bio *bio)
{
	struct r10bio *r10_bio = get_resync_r10bio(bio);
	struct mddev *mddev = r10_bio->mddev;
	struct r10conf *conf = mddev->private;
	int d;
	int slot;
	int repl;
	struct md_rdev *rdev = NULL;

	d = find_bio_disk(conf, r10_bio, bio, &slot, &repl);
	if (repl)
		rdev = conf->mirrors[d].replacement;
	else
		rdev = conf->mirrors[d].rdev;

	if (bio->bi_status) {
		if (repl)
			md_error(mddev, rdev);
		else {
			set_bit(WriteErrorSeen, &rdev->flags);
			if (!test_and_set_bit(WantReplacement, &rdev->flags))
				set_bit(MD_RECOVERY_NEEDED,
					&rdev->mddev->recovery);
			set_bit(R10BIO_WriteError, &r10_bio->state);
		}
	} else if (rdev_has_badblock(rdev, r10_bio->devs[slot].addr,
				     r10_bio->sectors)) {
		set_bit(R10BIO_MadeGood, &r10_bio->state);
	}

	rdev_dec_pending(rdev, mddev);

	end_sync_request(r10_bio);
}

/*
 * Note: sync and recover and handled very differently for raid10
 * This code is for resync.
 * For resync, we read through virtual addresses and read all blocks.
 * If there is any error, we schedule a write.  The lowest numbered
 * drive is authoritative.
 * However requests come for physical address, so we need to map.
 * For every physical address there are raid_disks/copies virtual addresses,
 * which is always are least one, but is not necessarly an integer.
 * This means that a physical address can span multiple chunks, so we may
 * have to submit multiple io requests for a single sync request.
 */
/*
 * We check if all blocks are in-sync and only write to blocks that
 * aren't in sync
 */
static void sync_request_write(struct mddev *mddev, struct r10bio *r10_bio)
{
	struct r10conf *conf = mddev->private;
	int i, first;
	struct bio *tbio, *fbio;
	int vcnt;
	struct page **tpages, **fpages;

	atomic_set(&r10_bio->remaining, 1);

	/* find the first device with a block */
	for (i=0; i<conf->copies; i++)
		if (!r10_bio->devs[i].bio->bi_status)
			break;

	if (i == conf->copies)
		goto done;

	first = i;
	fbio = r10_bio->devs[i].bio;
	fbio->bi_iter.bi_size = r10_bio->sectors << 9;
	fbio->bi_iter.bi_idx = 0;
	fpages = get_resync_pages(fbio)->pages;

	vcnt = (r10_bio->sectors + (PAGE_SIZE >> 9) - 1) >> (PAGE_SHIFT - 9);
	/* now find blocks with errors */
	for (i=0 ; i < conf->copies ; i++) {
		int  j, d;
		struct md_rdev *rdev;
		struct resync_pages *rp;

		tbio = r10_bio->devs[i].bio;

		if (tbio->bi_end_io != end_sync_read)
			continue;
		if (i == first)
			continue;

		tpages = get_resync_pages(tbio)->pages;
		d = r10_bio->devs[i].devnum;
		rdev = conf->mirrors[d].rdev;
		if (!r10_bio->devs[i].bio->bi_status) {
			/* We know that the bi_io_vec layout is the same for
			 * both 'first' and 'i', so we just compare them.
			 * All vec entries are PAGE_SIZE;
			 */
			int sectors = r10_bio->sectors;
			for (j = 0; j < vcnt; j++) {
				int len = PAGE_SIZE;
				if (sectors < (len / 512))
					len = sectors * 512;
				if (memcmp(page_address(fpages[j]),
					   page_address(tpages[j]),
					   len))
					break;
				sectors -= len/512;
			}
			if (j == vcnt)
				continue;
			atomic64_add(r10_bio->sectors, &mddev->resync_mismatches);
			if (test_bit(MD_RECOVERY_CHECK, &mddev->recovery))
				/* Don't fix anything. */
				continue;
		} else if (test_bit(FailFast, &rdev->flags)) {
			/* Just give up on this device */
			md_error(rdev->mddev, rdev);
			continue;
		}
		/* Ok, we need to write this bio, either to correct an
		 * inconsistency or to correct an unreadable block.
		 * First we need to fixup bv_offset, bv_len and
		 * bi_vecs, as the read request might have corrupted these
		 */
		rp = get_resync_pages(tbio);
		bio_reset(tbio, conf->mirrors[d].rdev->bdev, REQ_OP_WRITE);

		md_bio_reset_resync_pages(tbio, rp, fbio->bi_iter.bi_size);

		rp->raid_bio = r10_bio;
		tbio->bi_private = rp;
		tbio->bi_iter.bi_sector = r10_bio->devs[i].addr;
		tbio->bi_end_io = end_sync_write;

		bio_copy_data(tbio, fbio);

		atomic_inc(&conf->mirrors[d].rdev->nr_pending);
		atomic_inc(&r10_bio->remaining);

		if (test_bit(FailFast, &conf->mirrors[d].rdev->flags))
			tbio->bi_opf |= MD_FAILFAST;
		tbio->bi_iter.bi_sector += conf->mirrors[d].rdev->data_offset;
		submit_bio_noacct(tbio);
	}

	/* Now write out to any replacement devices
	 * that are active
	 */
	for (i = 0; i < conf->copies; i++) {
		tbio = r10_bio->devs[i].repl_bio;
		if (!tbio || !tbio->bi_end_io)
			continue;
		if (r10_bio->devs[i].bio->bi_end_io != end_sync_write
		    && r10_bio->devs[i].bio != fbio)
			bio_copy_data(tbio, fbio);
		atomic_inc(&r10_bio->remaining);
		submit_bio_noacct(tbio);
	}

done:
	if (atomic_dec_and_test(&r10_bio->remaining)) {
		md_done_sync(mddev, r10_bio->sectors, 1);
		put_buf(r10_bio);
	}
}

/*
 * Now for the recovery code.
 * Recovery happens across physical sectors.
 * We recover all non-is_sync drives by finding the virtual address of
 * each, and then choose a working drive that also has that virt address.
 * There is a separate r10_bio for each non-in_sync drive.
 * Only the first two slots are in use. The first for reading,
 * The second for writing.
 *
 */
static void fix_recovery_read_error(struct r10bio *r10_bio)
{
	/* We got a read error during recovery.
	 * We repeat the read in smaller page-sized sections.
	 * If a read succeeds, write it to the new device or record
	 * a bad block if we cannot.
	 * If a read fails, record a bad block on both old and
	 * new devices.
	 */
	struct mddev *mddev = r10_bio->mddev;
	struct r10conf *conf = mddev->private;
	struct bio *bio = r10_bio->devs[0].bio;
	sector_t sect = 0;
	int sectors = r10_bio->sectors;
	int idx = 0;
	int dr = r10_bio->devs[0].devnum;
	int dw = r10_bio->devs[1].devnum;
	struct page **pages = get_resync_pages(bio)->pages;

	while (sectors) {
		int s = sectors;
		struct md_rdev *rdev;
		sector_t addr;
		int ok;

		if (s > (PAGE_SIZE>>9))
			s = PAGE_SIZE >> 9;

		rdev = conf->mirrors[dr].rdev;
		addr = r10_bio->devs[0].addr + sect;
		ok = sync_page_io(rdev,
				  addr,
				  s << 9,
				  pages[idx],
				  REQ_OP_READ, false);
		if (ok) {
			rdev = conf->mirrors[dw].rdev;
			addr = r10_bio->devs[1].addr + sect;
			ok = sync_page_io(rdev,
					  addr,
					  s << 9,
					  pages[idx],
					  REQ_OP_WRITE, false);
			if (!ok) {
				set_bit(WriteErrorSeen, &rdev->flags);
				if (!test_and_set_bit(WantReplacement,
						      &rdev->flags))
					set_bit(MD_RECOVERY_NEEDED,
						&rdev->mddev->recovery);
			}
		}
		if (!ok) {
			/* We don't worry if we cannot set a bad block -
			 * it really is bad so there is no loss in not
			 * recording it yet
			 */
			rdev_set_badblocks(rdev, addr, s, 0);

			if (rdev != conf->mirrors[dw].rdev) {
				/* need bad block on destination too */
				struct md_rdev *rdev2 = conf->mirrors[dw].rdev;
				addr = r10_bio->devs[1].addr + sect;
				ok = rdev_set_badblocks(rdev2, addr, s, 0);
				if (!ok) {
					/* just abort the recovery */
					pr_notice("md/raid10:%s: recovery aborted due to read error\n",
						  mdname(mddev));

					conf->mirrors[dw].recovery_disabled
						= mddev->recovery_disabled;
					set_bit(MD_RECOVERY_INTR,
						&mddev->recovery);
					break;
				}
			}
		}

		sectors -= s;
		sect += s;
		idx++;
	}
}

static void recovery_request_write(struct mddev *mddev, struct r10bio *r10_bio)
{
	struct r10conf *conf = mddev->private;
	int d;
	struct bio *wbio = r10_bio->devs[1].bio;
	struct bio *wbio2 = r10_bio->devs[1].repl_bio;

	/* Need to test wbio2->bi_end_io before we call
	 * submit_bio_noacct as if the former is NULL,
	 * the latter is free to free wbio2.
	 */
	if (wbio2 && !wbio2->bi_end_io)
		wbio2 = NULL;

	if (!test_bit(R10BIO_Uptodate, &r10_bio->state)) {
		fix_recovery_read_error(r10_bio);
		if (wbio->bi_end_io)
			end_sync_request(r10_bio);
		if (wbio2)
			end_sync_request(r10_bio);
		return;
	}

	/*
	 * share the pages with the first bio
	 * and submit the write request
	 */
	d = r10_bio->devs[1].devnum;
	if (wbio->bi_end_io) {
		atomic_inc(&conf->mirrors[d].rdev->nr_pending);
		submit_bio_noacct(wbio);
	}
	if (wbio2) {
		atomic_inc(&conf->mirrors[d].replacement->nr_pending);
		submit_bio_noacct(wbio2);
	}
}

static int r10_sync_page_io(struct md_rdev *rdev, sector_t sector,
			    int sectors, struct page *page, enum req_op op)
{
	if (rdev_has_badblock(rdev, sector, sectors) &&
	    (op == REQ_OP_READ || test_bit(WriteErrorSeen, &rdev->flags)))
		return -1;
	if (sync_page_io(rdev, sector, sectors << 9, page, op, false))
		/* success */
		return 1;
	if (op == REQ_OP_WRITE) {
		set_bit(WriteErrorSeen, &rdev->flags);
		if (!test_and_set_bit(WantReplacement, &rdev->flags))
			set_bit(MD_RECOVERY_NEEDED,
				&rdev->mddev->recovery);
	}
	/* need to record an error - either for the block or the device */
	if (!rdev_set_badblocks(rdev, sector, sectors, 0))
		md_error(rdev->mddev, rdev);
	return 0;
}

/*
 * This is a kernel thread which:
 *
 *	1.	Retries failed read operations on working mirrors.
 *	2.	Updates the raid superblock when problems encounter.
 *	3.	Performs writes following reads for array synchronising.
 */

static void fix_read_error(struct r10conf *conf, struct mddev *mddev, struct r10bio *r10_bio)
{
	int sect = 0; /* Offset from r10_bio->sector */
	int sectors = r10_bio->sectors, slot = r10_bio->read_slot;
	struct md_rdev *rdev;
	int d = r10_bio->devs[slot].devnum;

	/* still own a reference to this rdev, so it cannot
	 * have been cleared recently.
	 */
	rdev = conf->mirrors[d].rdev;

	if (test_bit(Faulty, &rdev->flags))
		/* drive has already been failed, just ignore any
		   more fix_read_error() attempts */
		return;

	if (exceed_read_errors(mddev, rdev)) {
		r10_bio->devs[slot].bio = IO_BLOCKED;
		return;
	}

	while(sectors) {
		int s = sectors;
		int sl = slot;
		int success = 0;
		int start;

		if (s > (PAGE_SIZE>>9))
			s = PAGE_SIZE >> 9;

		do {
			d = r10_bio->devs[sl].devnum;
			rdev = conf->mirrors[d].rdev;
			if (rdev &&
			    test_bit(In_sync, &rdev->flags) &&
			    !test_bit(Faulty, &rdev->flags) &&
			    rdev_has_badblock(rdev,
					      r10_bio->devs[sl].addr + sect,
					      s) == 0) {
				atomic_inc(&rdev->nr_pending);
				success = sync_page_io(rdev,
						       r10_bio->devs[sl].addr +
						       sect,
						       s<<9,
						       conf->tmppage,
						       REQ_OP_READ, false);
				rdev_dec_pending(rdev, mddev);
				if (success)
					break;
			}
			sl++;
			if (sl == conf->copies)
				sl = 0;
		} while (sl != slot);

		if (!success) {
			/* Cannot read from anywhere, just mark the block
			 * as bad on the first device to discourage future
			 * reads.
			 */
			int dn = r10_bio->devs[slot].devnum;
			rdev = conf->mirrors[dn].rdev;

			if (!rdev_set_badblocks(
				    rdev,
				    r10_bio->devs[slot].addr
				    + sect,
				    s, 0)) {
				md_error(mddev, rdev);
				r10_bio->devs[slot].bio
					= IO_BLOCKED;
			}
			break;
		}

		start = sl;
		/* write it back and re-read */
		while (sl != slot) {
			if (sl==0)
				sl = conf->copies;
			sl--;
			d = r10_bio->devs[sl].devnum;
			rdev = conf->mirrors[d].rdev;
			if (!rdev ||
			    test_bit(Faulty, &rdev->flags) ||
			    !test_bit(In_sync, &rdev->flags))
				continue;

			atomic_inc(&rdev->nr_pending);
			if (r10_sync_page_io(rdev,
					     r10_bio->devs[sl].addr +
					     sect,
					     s, conf->tmppage, REQ_OP_WRITE)
			    == 0) {
				/* Well, this device is dead */
				pr_notice("md/raid10:%s: read correction write failed (%d sectors at %llu on %pg)\n",
					  mdname(mddev), s,
					  (unsigned long long)(
						  sect +
						  choose_data_offset(r10_bio,
								     rdev)),
					  rdev->bdev);
				pr_notice("md/raid10:%s: %pg: failing drive\n",
					  mdname(mddev),
					  rdev->bdev);
			}
			rdev_dec_pending(rdev, mddev);
		}
		sl = start;
		while (sl != slot) {
			if (sl==0)
				sl = conf->copies;
			sl--;
			d = r10_bio->devs[sl].devnum;
			rdev = conf->mirrors[d].rdev;
			if (!rdev ||
			    test_bit(Faulty, &rdev->flags) ||
			    !test_bit(In_sync, &rdev->flags))
				continue;

			atomic_inc(&rdev->nr_pending);
			switch (r10_sync_page_io(rdev,
					     r10_bio->devs[sl].addr +
					     sect,
					     s, conf->tmppage, REQ_OP_READ)) {
			case 0:
				/* Well, this device is dead */
				pr_notice("md/raid10:%s: unable to read back corrected sectors (%d sectors at %llu on %pg)\n",
				       mdname(mddev), s,
				       (unsigned long long)(
					       sect +
					       choose_data_offset(r10_bio, rdev)),
				       rdev->bdev);
				pr_notice("md/raid10:%s: %pg: failing drive\n",
				       mdname(mddev),
				       rdev->bdev);
				break;
			case 1:
				pr_info("md/raid10:%s: read error corrected (%d sectors at %llu on %pg)\n",
				       mdname(mddev), s,
				       (unsigned long long)(
					       sect +
					       choose_data_offset(r10_bio, rdev)),
				       rdev->bdev);
				atomic_add(s, &rdev->corrected_errors);
			}

			rdev_dec_pending(rdev, mddev);
		}

		sectors -= s;
		sect += s;
	}
}

static bool narrow_write_error(struct r10bio *r10_bio, int i)
{
	struct bio *bio = r10_bio->master_bio;
	struct mddev *mddev = r10_bio->mddev;
	struct r10conf *conf = mddev->private;
	struct md_rdev *rdev = conf->mirrors[r10_bio->devs[i].devnum].rdev;
	/* bio has the data to be written to slot 'i' where
	 * we just recently had a write error.
	 * We repeatedly clone the bio and trim down to one block,
	 * then try the write.  Where the write fails we record
	 * a bad block.
	 * It is conceivable that the bio doesn't exactly align with
	 * blocks.  We must handle this.
	 *
	 * We currently own a reference to the rdev.
	 */

	int block_sectors;
	sector_t sector;
	int sectors;
	int sect_to_write = r10_bio->sectors;
	bool ok = true;

	if (rdev->badblocks.shift < 0)
		return false;

	block_sectors = roundup(1 << rdev->badblocks.shift,
				bdev_logical_block_size(rdev->bdev) >> 9);
	sector = r10_bio->sector;
	sectors = ((r10_bio->sector + block_sectors)
		   & ~(sector_t)(block_sectors - 1))
		- sector;

	while (sect_to_write) {
		struct bio *wbio;
		sector_t wsector;
		if (sectors > sect_to_write)
			sectors = sect_to_write;
		/* Write at 'sector' for 'sectors' */
		wbio = bio_alloc_clone(rdev->bdev, bio, GFP_NOIO,
				       &mddev->bio_set);
		bio_trim(wbio, sector - bio->bi_iter.bi_sector, sectors);
		wsector = r10_bio->devs[i].addr + (sector - r10_bio->sector);
		wbio->bi_iter.bi_sector = wsector +
				   choose_data_offset(r10_bio, rdev);
		wbio->bi_opf = REQ_OP_WRITE;

		if (submit_bio_wait(wbio) < 0)
			/* Failure! */
			ok = rdev_set_badblocks(rdev, wsector,
						sectors, 0)
				&& ok;

		bio_put(wbio);
		sect_to_write -= sectors;
		sector += sectors;
		sectors = block_sectors;
	}
	return ok;
}

static void handle_read_error(struct mddev *mddev, struct r10bio *r10_bio)
{
	int slot = r10_bio->read_slot;
	struct bio *bio;
	struct r10conf *conf = mddev->private;
	struct md_rdev *rdev = r10_bio->devs[slot].rdev;

	/* we got a read error. Maybe the drive is bad.  Maybe just
	 * the block and we can fix it.
	 * We freeze all other IO, and try reading the block from
	 * other devices.  When we find one, we re-write
	 * and check it that fixes the read error.
	 * This is all done synchronously while the array is
	 * frozen.
	 */
	bio = r10_bio->devs[slot].bio;
	bio_put(bio);
	r10_bio->devs[slot].bio = NULL;

	if (mddev->ro)
		r10_bio->devs[slot].bio = IO_BLOCKED;
	else if (!test_bit(FailFast, &rdev->flags)) {
		freeze_array(conf, 1);
		fix_read_error(conf, mddev, r10_bio);
		unfreeze_array(conf);
	} else
		md_error(mddev, rdev);

	rdev_dec_pending(rdev, mddev);
	r10_bio->state = 0;
	raid10_read_request(mddev, r10_bio->master_bio, r10_bio, false);
	/*
	 * allow_barrier after re-submit to ensure no sync io
	 * can be issued while regular io pending.
	 */
	allow_barrier(conf);
}

static void handle_write_completed(struct r10conf *conf, struct r10bio *r10_bio)
{
	/* Some sort of write request has finished and it
	 * succeeded in writing where we thought there was a
	 * bad block.  So forget the bad block.
	 * Or possibly if failed and we need to record
	 * a bad block.
	 */
	int m;
	struct md_rdev *rdev;

	if (test_bit(R10BIO_IsSync, &r10_bio->state) ||
	    test_bit(R10BIO_IsRecover, &r10_bio->state)) {
		for (m = 0; m < conf->copies; m++) {
			int dev = r10_bio->devs[m].devnum;
			rdev = conf->mirrors[dev].rdev;
			if (r10_bio->devs[m].bio == NULL ||
				r10_bio->devs[m].bio->bi_end_io == NULL)
				continue;
			if (!r10_bio->devs[m].bio->bi_status) {
				rdev_clear_badblocks(
					rdev,
					r10_bio->devs[m].addr,
					r10_bio->sectors, 0);
			} else {
				if (!rdev_set_badblocks(
					    rdev,
					    r10_bio->devs[m].addr,
					    r10_bio->sectors, 0))
					md_error(conf->mddev, rdev);
			}
			rdev = conf->mirrors[dev].replacement;
			if (r10_bio->devs[m].repl_bio == NULL ||
				r10_bio->devs[m].repl_bio->bi_end_io == NULL)
				continue;

			if (!r10_bio->devs[m].repl_bio->bi_status) {
				rdev_clear_badblocks(
					rdev,
					r10_bio->devs[m].addr,
					r10_bio->sectors, 0);
			} else {
				if (!rdev_set_badblocks(
					    rdev,
					    r10_bio->devs[m].addr,
					    r10_bio->sectors, 0))
					md_error(conf->mddev, rdev);
			}
		}
		put_buf(r10_bio);
	} else {
		bool fail = false;
		for (m = 0; m < conf->copies; m++) {
			int dev = r10_bio->devs[m].devnum;
			struct bio *bio = r10_bio->devs[m].bio;
			rdev = conf->mirrors[dev].rdev;
			if (bio == IO_MADE_GOOD) {
				rdev_clear_badblocks(
					rdev,
					r10_bio->devs[m].addr,
					r10_bio->sectors, 0);
				rdev_dec_pending(rdev, conf->mddev);
			} else if (bio != NULL && bio->bi_status) {
				fail = true;
				if (!narrow_write_error(r10_bio, m))
					md_error(conf->mddev, rdev);
				rdev_dec_pending(rdev, conf->mddev);
			}
			bio = r10_bio->devs[m].repl_bio;
			rdev = conf->mirrors[dev].replacement;
			if (rdev && bio == IO_MADE_GOOD) {
				rdev_clear_badblocks(
					rdev,
					r10_bio->devs[m].addr,
					r10_bio->sectors, 0);
				rdev_dec_pending(rdev, conf->mddev);
			}
		}
		if (fail) {
			spin_lock_irq(&conf->device_lock);
			list_add(&r10_bio->retry_list, &conf->bio_end_io_list);
			conf->nr_queued++;
			spin_unlock_irq(&conf->device_lock);
			/*
			 * In case freeze_array() is waiting for condition
			 * nr_pending == nr_queued + extra to be true.
			 */
			wake_up(&conf->wait_barrier);
			md_wakeup_thread(conf->mddev->thread);
		} else {
			if (test_bit(R10BIO_WriteError,
				     &r10_bio->state))
				close_write(r10_bio);
			raid_end_bio_io(r10_bio);
		}
	}
}

static void raid10d(struct md_thread *thread)
{
	struct mddev *mddev = thread->mddev;
	struct r10bio *r10_bio;
	unsigned long flags;
	struct r10conf *conf = mddev->private;
	struct list_head *head = &conf->retry_list;
	struct blk_plug plug;

	md_check_recovery(mddev);

	if (!list_empty_careful(&conf->bio_end_io_list) &&
	    !test_bit(MD_SB_CHANGE_PENDING, &mddev->sb_flags)) {
		LIST_HEAD(tmp);
		spin_lock_irqsave(&conf->device_lock, flags);
		if (!test_bit(MD_SB_CHANGE_PENDING, &mddev->sb_flags)) {
			while (!list_empty(&conf->bio_end_io_list)) {
				list_move(conf->bio_end_io_list.prev, &tmp);
				conf->nr_queued--;
			}
		}
		spin_unlock_irqrestore(&conf->device_lock, flags);
		while (!list_empty(&tmp)) {
			r10_bio = list_first_entry(&tmp, struct r10bio,
						   retry_list);
			list_del(&r10_bio->retry_list);

			if (test_bit(R10BIO_WriteError,
				     &r10_bio->state))
				close_write(r10_bio);
			raid_end_bio_io(r10_bio);
		}
	}

	blk_start_plug(&plug);
	for (;;) {

		flush_pending_writes(conf);

		spin_lock_irqsave(&conf->device_lock, flags);
		if (list_empty(head)) {
			spin_unlock_irqrestore(&conf->device_lock, flags);
			break;
		}
		r10_bio = list_entry(head->prev, struct r10bio, retry_list);
		list_del(head->prev);
		conf->nr_queued--;
		spin_unlock_irqrestore(&conf->device_lock, flags);

		mddev = r10_bio->mddev;
		conf = mddev->private;
		if (test_bit(R10BIO_MadeGood, &r10_bio->state) ||
		    test_bit(R10BIO_WriteError, &r10_bio->state))
			handle_write_completed(conf, r10_bio);
		else if (test_bit(R10BIO_IsReshape, &r10_bio->state))
			reshape_request_write(mddev, r10_bio);
		else if (test_bit(R10BIO_IsSync, &r10_bio->state))
			sync_request_write(mddev, r10_bio);
		else if (test_bit(R10BIO_IsRecover, &r10_bio->state))
			recovery_request_write(mddev, r10_bio);
		else if (test_bit(R10BIO_ReadError, &r10_bio->state))
			handle_read_error(mddev, r10_bio);
		else
			WARN_ON_ONCE(1);

		cond_resched();
		if (mddev->sb_flags & ~(1<<MD_SB_CHANGE_PENDING))
			md_check_recovery(mddev);
	}
	blk_finish_plug(&plug);
}

static int init_resync(struct r10conf *conf)
{
	int ret, buffs, i;

	buffs = RESYNC_WINDOW / RESYNC_BLOCK_SIZE;
	BUG_ON(mempool_initialized(&conf->r10buf_pool));
	conf->have_replacement = 0;
	for (i = 0; i < conf->geo.raid_disks; i++)
		if (conf->mirrors[i].replacement)
			conf->have_replacement = 1;
	ret = mempool_init(&conf->r10buf_pool, buffs,
			   r10buf_pool_alloc, r10buf_pool_free, conf);
	if (ret)
		return ret;
	conf->next_resync = 0;
	return 0;
}

static struct r10bio *raid10_alloc_init_r10buf(struct r10conf *conf)
{
	struct r10bio *r10bio = mempool_alloc(&conf->r10buf_pool, GFP_NOIO);
	struct rsync_pages *rp;
	struct bio *bio;
	int nalloc;
	int i;

	if (test_bit(MD_RECOVERY_SYNC, &conf->mddev->recovery) ||
	    test_bit(MD_RECOVERY_RESHAPE, &conf->mddev->recovery))
		nalloc = conf->copies; /* resync */
	else
		nalloc = 2; /* recovery */

	for (i = 0; i < nalloc; i++) {
		bio = r10bio->devs[i].bio;
		rp = bio->bi_private;
		bio_reset(bio, NULL, 0);
		bio->bi_private = rp;
		bio = r10bio->devs[i].repl_bio;
		if (bio) {
			rp = bio->bi_private;
			bio_reset(bio, NULL, 0);
			bio->bi_private = rp;
		}
	}
	return r10bio;
}

/*
 * Set cluster_sync_high since we need other nodes to add the
 * range [cluster_sync_low, cluster_sync_high] to suspend list.
 */
static void raid10_set_cluster_sync_high(struct r10conf *conf)
{
	sector_t window_size;
	int extra_chunk, chunks;

	/*
	 * First, here we define "stripe" as a unit which across
	 * all member devices one time, so we get chunks by use
	 * raid_disks / near_copies. Otherwise, if near_copies is
	 * close to raid_disks, then resync window could increases
	 * linearly with the increase of raid_disks, which means
	 * we will suspend a really large IO window while it is not
	 * necessary. If raid_disks is not divisible by near_copies,
	 * an extra chunk is needed to ensure the whole "stripe" is
	 * covered.
	 */

	chunks = conf->geo.raid_disks / conf->geo.near_copies;
	if (conf->geo.raid_disks % conf->geo.near_copies == 0)
		extra_chunk = 0;
	else
		extra_chunk = 1;
	window_size = (chunks + extra_chunk) * conf->mddev->chunk_sectors;

	/*
	 * At least use a 32M window to align with raid1's resync window
	 */
	window_size = (CLUSTER_RESYNC_WINDOW_SECTORS > window_size) ?
			CLUSTER_RESYNC_WINDOW_SECTORS : window_size;

	conf->cluster_sync_high = conf->cluster_sync_low + window_size;
}

/*
 * perform a "sync" on one "block"
 *
 * We need to make sure that no normal I/O request - particularly write
 * requests - conflict with active sync requests.
 *
 * This is achieved by tracking pending requests and a 'barrier' concept
 * that can be installed to exclude normal IO requests.
 *
 * Resync and recovery are handled very differently.
 * We differentiate by looking at MD_RECOVERY_SYNC in mddev->recovery.
 *
 * For resync, we iterate over virtual addresses, read all copies,
 * and update if there are differences.  If only one copy is live,
 * skip it.
 * For recovery, we iterate over physical addresses, read a good
 * value for each non-in_sync drive, and over-write.
 *
 * So, for recovery we may have several outstanding complex requests for a
 * given address, one for each out-of-sync device.  We model this by allocating
 * a number of r10_bio structures, one for each out-of-sync device.
 * As we setup these structures, we collect all bio's together into a list
 * which we then process collectively to add pages, and then process again
 * to pass to submit_bio_noacct.
 *
 * The r10_bio structures are linked using a borrowed master_bio pointer.
 * This link is counted in ->remaining.  When the r10_bio that points to NULL
 * has its remaining count decremented to 0, the whole complex operation
 * is complete.
 *
 */

static sector_t raid10_sync_request(struct mddev *mddev, sector_t sector_nr,
				    sector_t max_sector, int *skipped)
{
	struct r10conf *conf = mddev->private;
	struct r10bio *r10_bio;
	struct bio *biolist = NULL, *bio;
	sector_t nr_sectors;
	int i;
	int max_sync;
	sector_t sync_blocks;
	sector_t sectors_skipped = 0;
	int chunks_skipped = 0;
	sector_t chunk_mask = conf->geo.chunk_mask;
	int page_idx = 0;
	int error_disk = -1;

	/*
	 * Allow skipping a full rebuild for incremental assembly
	 * of a clean array, like RAID1 does.
	 */
	if (mddev->bitmap == NULL &&
	    mddev->recovery_cp == MaxSector &&
	    mddev->reshape_position == MaxSector &&
	    !test_bit(MD_RECOVERY_SYNC, &mddev->recovery) &&
	    !test_bit(MD_RECOVERY_REQUESTED, &mddev->recovery) &&
	    !test_bit(MD_RECOVERY_RESHAPE, &mddev->recovery) &&
	    conf->fullsync == 0) {
		*skipped = 1;
		return mddev->dev_sectors - sector_nr;
	}

	if (!mempool_initialized(&conf->r10buf_pool))
		if (init_resync(conf))
			return 0;

 skipped:
	if (sector_nr >= max_sector) {
		conf->cluster_sync_low = 0;
		conf->cluster_sync_high = 0;

		/* If we aborted, we need to abort the
		 * sync on the 'current' bitmap chucks (there can
		 * be several when recovering multiple devices).
		 * as we may have started syncing it but not finished.
		 * We can find the current address in
		 * mddev->curr_resync, but for recovery,
		 * we need to convert that to several
		 * virtual addresses.
		 */
		if (test_bit(MD_RECOVERY_RESHAPE, &mddev->recovery)) {
			end_reshape(conf);
			close_sync(conf);
			return 0;
		}

		if (mddev->curr_resync < max_sector) { /* aborted */
			if (test_bit(MD_RECOVERY_SYNC, &mddev->recovery))
				mddev->bitmap_ops->end_sync(mddev,
							    mddev->curr_resync,
							    &sync_blocks);
			else for (i = 0; i < conf->geo.raid_disks; i++) {
				sector_t sect =
					raid10_find_virt(conf, mddev->curr_resync, i);

				mddev->bitmap_ops->end_sync(mddev, sect,
							    &sync_blocks);
			}
		} else {
			/* completed sync */
			if ((!mddev->bitmap || conf->fullsync)
			    && conf->have_replacement
			    && test_bit(MD_RECOVERY_SYNC, &mddev->recovery)) {
				/* Completed a full sync so the replacements
				 * are now fully recovered.
				 */
				for (i = 0; i < conf->geo.raid_disks; i++) {
					struct md_rdev *rdev =
						conf->mirrors[i].replacement;

					if (rdev)
						rdev->recovery_offset = MaxSector;
				}
			}
			conf->fullsync = 0;
		}
		mddev->bitmap_ops->close_sync(mddev);
		close_sync(conf);
		*skipped = 1;
		return sectors_skipped;
	}

	if (test_bit(MD_RECOVERY_RESHAPE, &mddev->recovery))
		return reshape_request(mddev, sector_nr, skipped);

	if (chunks_skipped >= conf->geo.raid_disks) {
		pr_err("md/raid10:%s: %s fails\n", mdname(mddev),
			test_bit(MD_RECOVERY_SYNC, &mddev->recovery) ?  "resync" : "recovery");
		if (error_disk >= 0 &&
		    !test_bit(MD_RECOVERY_SYNC, &mddev->recovery)) {
			/*
			 * recovery fails, set mirrors.recovery_disabled,
			 * device shouldn't be added to there.
			 */
			conf->mirrors[error_disk].recovery_disabled =
						mddev->recovery_disabled;
			return 0;
		}
		/*
		 * if there has been nothing to do on any drive,
		 * then there is nothing to do at all.
		 */
		*skipped = 1;
		return (max_sector - sector_nr) + sectors_skipped;
	}

	if (max_sector > mddev->resync_max)
		max_sector = mddev->resync_max; /* Don't do IO beyond here */

	/* make sure whole request will fit in a chunk - if chunks
	 * are meaningful
	 */
	if (conf->geo.near_copies < conf->geo.raid_disks &&
	    max_sector > (sector_nr | chunk_mask))
		max_sector = (sector_nr | chunk_mask) + 1;

	/*
	 * If there is non-resync activity waiting for a turn, then let it
	 * though before starting on this new sync request.
	 */
	if (conf->nr_waiting)
		schedule_timeout_uninterruptible(1);

	/* Again, very different code for resync and recovery.
	 * Both must result in an r10bio with a list of bios that
	 * have bi_end_io, bi_sector, bi_bdev set,
	 * and bi_private set to the r10bio.
	 * For recovery, we may actually create several r10bios
	 * with 2 bios in each, that correspond to the bios in the main one.
	 * In this case, the subordinate r10bios link back through a
	 * borrowed master_bio pointer, and the counter in the master
	 * includes a ref from each subordinate.
	 */
	/* First, we decide what to do and set ->bi_end_io
	 * To end_sync_read if we want to read, and
	 * end_sync_write if we will want to write.
	 */

	max_sync = RESYNC_PAGES << (PAGE_SHIFT-9);
	if (!test_bit(MD_RECOVERY_SYNC, &mddev->recovery)) {
		/* recovery... the complicated one */
		int j;
		r10_bio = NULL;

		for (i = 0 ; i < conf->geo.raid_disks; i++) {
			bool still_degraded;
			struct r10bio *rb2;
			sector_t sect;
			bool must_sync;
			int any_working;
			struct raid10_info *mirror = &conf->mirrors[i];
			struct md_rdev *mrdev, *mreplace;

			mrdev = mirror->rdev;
			mreplace = mirror->replacement;

			if (mrdev && (test_bit(Faulty, &mrdev->flags) ||
			    test_bit(In_sync, &mrdev->flags)))
				mrdev = NULL;
			if (mreplace && test_bit(Faulty, &mreplace->flags))
				mreplace = NULL;

			if (!mrdev && !mreplace)
				continue;

			still_degraded = false;
			/* want to reconstruct this device */
			rb2 = r10_bio;
			sect = raid10_find_virt(conf, sector_nr, i);
			if (sect >= mddev->resync_max_sectors)
				/* last stripe is not complete - don't
				 * try to recover this sector.
				 */
				continue;
			/* Unless we are doing a full sync, or a replacement
			 * we only need to recover the block if it is set in
			 * the bitmap
			 */
			must_sync = mddev->bitmap_ops->start_sync(mddev, sect,
								  &sync_blocks,
								  true);
			if (sync_blocks < max_sync)
				max_sync = sync_blocks;
			if (!must_sync &&
			    mreplace == NULL &&
			    !conf->fullsync) {
				/* yep, skip the sync_blocks here, but don't assume
				 * that there will never be anything to do here
				 */
				chunks_skipped = -1;
				continue;
			}
			if (mrdev)
				atomic_inc(&mrdev->nr_pending);
			if (mreplace)
				atomic_inc(&mreplace->nr_pending);

			r10_bio = raid10_alloc_init_r10buf(conf);
			r10_bio->state = 0;
			raise_barrier(conf, rb2 != NULL);
			atomic_set(&r10_bio->remaining, 0);

			r10_bio->master_bio = (struct bio*)rb2;
			if (rb2)
				atomic_inc(&rb2->remaining);
			r10_bio->mddev = mddev;
			set_bit(R10BIO_IsRecover, &r10_bio->state);
			r10_bio->sector = sect;

			raid10_find_phys(conf, r10_bio);

			/* Need to check if the array will still be
			 * degraded
			 */
			for (j = 0; j < conf->geo.raid_disks; j++) {
				struct md_rdev *rdev = conf->mirrors[j].rdev;

				if (rdev == NULL || test_bit(Faulty, &rdev->flags)) {
					still_degraded = false;
					break;
				}
			}

			must_sync = mddev->bitmap_ops->start_sync(mddev, sect,
						&sync_blocks, still_degraded);

			any_working = 0;
			for (j=0; j<conf->copies;j++) {
				int k;
				int d = r10_bio->devs[j].devnum;
				sector_t from_addr, to_addr;
				struct md_rdev *rdev = conf->mirrors[d].rdev;
				sector_t sector, first_bad;
				sector_t bad_sectors;
				if (!rdev ||
				    !test_bit(In_sync, &rdev->flags))
					continue;
				/* This is where we read from */
				any_working = 1;
				sector = r10_bio->devs[j].addr;

				if (is_badblock(rdev, sector, max_sync,
						&first_bad, &bad_sectors)) {
					if (first_bad > sector)
						max_sync = first_bad - sector;
					else {
						bad_sectors -= (sector
								- first_bad);
						if (max_sync > bad_sectors)
							max_sync = bad_sectors;
						continue;
					}
				}
				bio = r10_bio->devs[0].bio;
				bio->bi_next = biolist;
				biolist = bio;
				bio->bi_end_io = end_sync_read;
				bio->bi_opf = REQ_OP_READ;
				if (test_bit(FailFast, &rdev->flags))
					bio->bi_opf |= MD_FAILFAST;
				from_addr = r10_bio->devs[j].addr;
				bio->bi_iter.bi_sector = from_addr +
					rdev->data_offset;
				bio_set_dev(bio, rdev->bdev);
				atomic_inc(&rdev->nr_pending);
				/* and we write to 'i' (if not in_sync) */

				for (k=0; k<conf->copies; k++)
					if (r10_bio->devs[k].devnum == i)
						break;
				BUG_ON(k == conf->copies);
				to_addr = r10_bio->devs[k].addr;
				r10_bio->devs[0].devnum = d;
				r10_bio->devs[0].addr = from_addr;
				r10_bio->devs[1].devnum = i;
				r10_bio->devs[1].addr = to_addr;

				if (mrdev) {
					bio = r10_bio->devs[1].bio;
					bio->bi_next = biolist;
					biolist = bio;
					bio->bi_end_io = end_sync_write;
					bio->bi_opf = REQ_OP_WRITE;
					bio->bi_iter.bi_sector = to_addr
						+ mrdev->data_offset;
					bio_set_dev(bio, mrdev->bdev);
					atomic_inc(&r10_bio->remaining);
				} else
					r10_bio->devs[1].bio->bi_end_io = NULL;

				/* and maybe write to replacement */
				bio = r10_bio->devs[1].repl_bio;
				if (bio)
					bio->bi_end_io = NULL;
				/* Note: if replace is not NULL, then bio
				 * cannot be NULL as r10buf_pool_alloc will
				 * have allocated it.
				 */
				if (!mreplace)
					break;
				bio->bi_next = biolist;
				biolist = bio;
				bio->bi_end_io = end_sync_write;
				bio->bi_opf = REQ_OP_WRITE;
				bio->bi_iter.bi_sector = to_addr +
					mreplace->data_offset;
				bio_set_dev(bio, mreplace->bdev);
				atomic_inc(&r10_bio->remaining);
				break;
			}
			if (j == conf->copies) {
				/* Cannot recover, so abort the recovery or
				 * record a bad block */
				if (any_working) {
					/* problem is that there are bad blocks
					 * on other device(s)
					 */
					int k;
					for (k = 0; k < conf->copies; k++)
						if (r10_bio->devs[k].devnum == i)
							break;
					if (mrdev && !test_bit(In_sync,
						      &mrdev->flags)
					    && !rdev_set_badblocks(
						    mrdev,
						    r10_bio->devs[k].addr,
						    max_sync, 0))
						any_working = 0;
					if (mreplace &&
					    !rdev_set_badblocks(
						    mreplace,
						    r10_bio->devs[k].addr,
						    max_sync, 0))
						any_working = 0;
				}
				if (!any_working)  {
					if (!test_and_set_bit(MD_RECOVERY_INTR,
							      &mddev->recovery))
						pr_warn("md/raid10:%s: insufficient working devices for recovery.\n",
						       mdname(mddev));
					mirror->recovery_disabled
						= mddev->recovery_disabled;
				} else {
					error_disk = i;
				}
				put_buf(r10_bio);
				if (rb2)
					atomic_dec(&rb2->remaining);
				r10_bio = rb2;
				if (mrdev)
					rdev_dec_pending(mrdev, mddev);
				if (mreplace)
					rdev_dec_pending(mreplace, mddev);
				break;
			}
			if (mrdev)
				rdev_dec_pending(mrdev, mddev);
			if (mreplace)
				rdev_dec_pending(mreplace, mddev);
			if (r10_bio->devs[0].bio->bi_opf & MD_FAILFAST) {
				/* Only want this if there is elsewhere to
				 * read from. 'j' is currently the first
				 * readable copy.
				 */
				int targets = 1;
				for (; j < conf->copies; j++) {
					int d = r10_bio->devs[j].devnum;
					if (conf->mirrors[d].rdev &&
					    test_bit(In_sync,
						      &conf->mirrors[d].rdev->flags))
						targets++;
				}
				if (targets == 1)
					r10_bio->devs[0].bio->bi_opf
						&= ~MD_FAILFAST;
			}
		}
		if (biolist == NULL) {
			while (r10_bio) {
				struct r10bio *rb2 = r10_bio;
				r10_bio = (struct r10bio*) rb2->master_bio;
				rb2->master_bio = NULL;
				put_buf(rb2);
			}
			goto giveup;
		}
	} else {
		/* resync. Schedule a read for every block at this virt offset */
		int count = 0;

		/*
		 * Since curr_resync_completed could probably not update in
		 * time, and we will set cluster_sync_low based on it.
		 * Let's check against "sector_nr + 2 * RESYNC_SECTORS" for
		 * safety reason, which ensures curr_resync_completed is
		 * updated in bitmap_cond_end_sync.
		 */
		mddev->bitmap_ops->cond_end_sync(mddev, sector_nr,
					mddev_is_clustered(mddev) &&
					(sector_nr + 2 * RESYNC_SECTORS > conf->cluster_sync_high));

		if (!mddev->bitmap_ops->start_sync(mddev, sector_nr,
						   &sync_blocks,
						   mddev->degraded) &&
		    !conf->fullsync && !test_bit(MD_RECOVERY_REQUESTED,
						 &mddev->recovery)) {
			/* We can skip this block */
			*skipped = 1;
			return sync_blocks + sectors_skipped;
		}
		if (sync_blocks < max_sync)
			max_sync = sync_blocks;
		r10_bio = raid10_alloc_init_r10buf(conf);
		r10_bio->state = 0;

		r10_bio->mddev = mddev;
		atomic_set(&r10_bio->remaining, 0);
		raise_barrier(conf, 0);
		conf->next_resync = sector_nr;

		r10_bio->master_bio = NULL;
		r10_bio->sector = sector_nr;
		set_bit(R10BIO_IsSync, &r10_bio->state);
		raid10_find_phys(conf, r10_bio);
		r10_bio->sectors = (sector_nr | chunk_mask) - sector_nr + 1;

		for (i = 0; i < conf->copies; i++) {
			int d = r10_bio->devs[i].devnum;
			sector_t first_bad, sector;
			sector_t bad_sectors;
			struct md_rdev *rdev;

			if (r10_bio->devs[i].repl_bio)
				r10_bio->devs[i].repl_bio->bi_end_io = NULL;

			bio = r10_bio->devs[i].bio;
			bio->bi_status = BLK_STS_IOERR;
			rdev = conf->mirrors[d].rdev;
			if (rdev == NULL || test_bit(Faulty, &rdev->flags))
				continue;

			sector = r10_bio->devs[i].addr;
			if (is_badblock(rdev, sector, max_sync,
					&first_bad, &bad_sectors)) {
				if (first_bad > sector)
					max_sync = first_bad - sector;
				else {
					bad_sectors -= (sector - first_bad);
					if (max_sync > bad_sectors)
						max_sync = bad_sectors;
					continue;
				}
			}
			atomic_inc(&rdev->nr_pending);
			atomic_inc(&r10_bio->remaining);
			bio->bi_next = biolist;
			biolist = bio;
			bio->bi_end_io = end_sync_read;
			bio->bi_opf = REQ_OP_READ;
			if (test_bit(FailFast, &rdev->flags))
				bio->bi_opf |= MD_FAILFAST;
			bio->bi_iter.bi_sector = sector + rdev->data_offset;
			bio_set_dev(bio, rdev->bdev);
			count++;

			rdev = conf->mirrors[d].replacement;
			if (rdev == NULL || test_bit(Faulty, &rdev->flags))
				continue;

			atomic_inc(&rdev->nr_pending);

			/* Need to set up for writing to the replacement */
			bio = r10_bio->devs[i].repl_bio;
			bio->bi_status = BLK_STS_IOERR;

			sector = r10_bio->devs[i].addr;
			bio->bi_next = biolist;
			biolist = bio;
			bio->bi_end_io = end_sync_write;
			bio->bi_opf = REQ_OP_WRITE;
			if (test_bit(FailFast, &rdev->flags))
				bio->bi_opf |= MD_FAILFAST;
			bio->bi_iter.bi_sector = sector + rdev->data_offset;
			bio_set_dev(bio, rdev->bdev);
			count++;
		}

		if (count < 2) {
			for (i=0; i<conf->copies; i++) {
				int d = r10_bio->devs[i].devnum;
				if (r10_bio->devs[i].bio->bi_end_io)
					rdev_dec_pending(conf->mirrors[d].rdev,
							 mddev);
				if (r10_bio->devs[i].repl_bio &&
				    r10_bio->devs[i].repl_bio->bi_end_io)
					rdev_dec_pending(
						conf->mirrors[d].replacement,
						mddev);
			}
			put_buf(r10_bio);
			biolist = NULL;
			goto giveup;
		}
	}

	nr_sectors = 0;
	if (sector_nr + max_sync < max_sector)
		max_sector = sector_nr + max_sync;
	do {
		struct page *page;
		int len = PAGE_SIZE;
		if (sector_nr + (len>>9) > max_sector)
			len = (max_sector - sector_nr) << 9;
		if (len == 0)
			break;
		for (bio= biolist ; bio ; bio=bio->bi_next) {
			struct resync_pages *rp = get_resync_pages(bio);
			page = resync_fetch_page(rp, page_idx);
			if (WARN_ON(!bio_add_page(bio, page, len, 0))) {
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				goto giveup;
			}
		}
		nr_sectors += len>>9;
		sector_nr += len>>9;
	} while (++page_idx < RESYNC_PAGES);
	r10_bio->sectors = nr_sectors;

	if (mddev_is_clustered(mddev) &&
	    test_bit(MD_RECOVERY_SYNC, &mddev->recovery)) {
		/* It is resync not recovery */
		if (conf->cluster_sync_high < sector_nr + nr_sectors) {
			conf->cluster_sync_low = mddev->curr_resync_completed;
			raid10_set_cluster_sync_high(conf);
			/* Send resync message */
			mddev->cluster_ops->resync_info_update(mddev,
						conf->cluster_sync_low,
						conf->cluster_sync_high);
		}
	} else if (mddev_is_clustered(mddev)) {
		/* This is recovery not resync */
		sector_t sect_va1, sect_va2;
		bool broadcast_msg = false;

		for (i = 0; i < conf->geo.raid_disks; i++) {
			/*
			 * sector_nr is a device address for recovery, so we
			 * need translate it to array address before compare
			 * with cluster_sync_high.
			 */
			sect_va1 = raid10_find_virt(conf, sector_nr, i);

			if (conf->cluster_sync_high < sect_va1 + nr_sectors) {
				broadcast_msg = true;
				/*
				 * curr_resync_completed is similar as
				 * sector_nr, so make the translation too.
				 */
				sect_va2 = raid10_find_virt(conf,
					mddev->curr_resync_completed, i);

				if (conf->cluster_sync_low == 0 ||
				    conf->cluster_sync_low > sect_va2)
					conf->cluster_sync_low = sect_va2;
			}
		}
		if (broadcast_msg) {
			raid10_set_cluster_sync_high(conf);
			mddev->cluster_ops->resync_info_update(mddev,
						conf->cluster_sync_low,
						conf->cluster_sync_high);
		}
	}

	while (biolist) {
		bio = biolist;
		biolist = biolist->bi_next;

		bio->bi_next = NULL;
		r10_bio = get_resync_r10bio(bio);
		r10_bio->sectors = nr_sectors;

		if (bio->bi_end_io == end_sync_read) {
			bio->bi_status = 0;
			submit_bio_noacct(bio);
		}
	}

	if (sectors_skipped)
		/* pretend they weren't skipped, it makes
		 * no important difference in this case
		 */
		md_done_sync(mddev, sectors_skipped, 1);

	return sectors_skipped + nr_sectors;
 giveup:
	/* There is nowhere to write, so all non-sync
	 * drives must be failed or in resync, all drives
	 * have a bad block, so try the next chunk...
	 */
	if (sector_nr + max_sync < max_sector)
		max_sector = sector_nr + max_sync;

	sectors_skipped += (max_sector - sector_nr);
	chunks_skipped ++;
	sector_nr = max_sector;
	goto skipped;
}

static sector_t
raid10_size(struct mddev *mddev, sector_t sectors, int raid_disks)
{
	sector_t size;
	struct r10conf *conf = mddev->private;

	if (!raid_disks)
		raid_disks = min(conf->geo.raid_disks,
				 conf->prev.raid_disks);
	if (!sectors)
		sectors = conf->dev_sectors;

	size = sectors >> conf->geo.chunk_shift;
	sector_div(size, conf->geo.far_copies);
	size = size * raid_disks;
	sector_div(size, conf->geo.near_copies);

	return size << conf->geo.chunk_shift;
}

static void calc_sectors(struct r10conf *conf, sector_t size)
{
	/* Calculate the number of sectors-per-device that will
	 * actually be used, and set conf->dev_sectors and
	 * conf->stride
	 */

	size = size >> conf->geo.chunk_shift;
	sector_div(size, conf->geo.far_copies);
	size = size * conf->geo.raid_disks;
	sector_div(size, conf->geo.near_copies);
	/* 'size' is now the number of chunks in the array */
	/* calculate "used chunks per device" */
	size = size * conf->copies;

	/* We need to round up when dividing by raid_disks to
	 * get the stride size.
	 */
	size = DIV_ROUND_UP_SECTOR_T(size, conf->geo.raid_disks);

	conf->dev_sectors = size << conf->geo.chunk_shift;

	if (conf->geo.far_offset)
		conf->geo.stride = 1 << conf->geo.chunk_shift;
	else {
		sector_div(size, conf->geo.far_copies);
		conf->geo.stride = size << conf->geo.chunk_shift;
	}
}

enum geo_type {geo_new, geo_old, geo_start};
static int setup_geo(struct geom *geo, struct mddev *mddev, enum geo_type new)
{
	int nc, fc, fo;
	int layout, chunk, disks;
	switch (new) {
	case geo_old:
		layout = mddev->layout;
		chunk = mddev->chunk_sectors;
		disks = mddev->raid_disks - mddev->delta_disks;
		break;
	case geo_new:
		layout = mddev->new_layout;
		chunk = mddev->new_chunk_sectors;
		disks = mddev->raid_disks;
		break;
	default: /* avoid 'may be unused' warnings */
	case geo_start: /* new when starting reshape - raid_disks not
			 * updated yet. */
		layout = mddev->new_layout;
		chunk = mddev->new_chunk_sectors;
		disks = mddev->raid_disks + mddev->delta_disks;
		break;
	}
	if (layout >> 19)
		return -1;
	if (chunk < (PAGE_SIZE >> 9) ||
	    !is_power_of_2(chunk))
		return -2;
	nc = layout & 255;
	fc = (layout >> 8) & 255;
	fo = layout & (1<<16);
	geo->raid_disks = disks;
	geo->near_copies = nc;
	geo->far_copies = fc;
	geo->far_offset = fo;
	switch (layout >> 17) {
	case 0:	/* original layout.  simple but not always optimal */
		geo->far_set_size = disks;
		break;
	case 1: /* "improved" layout which was buggy.  Hopefully no-one is
		 * actually using this, but leave code here just in case.*/
		geo->far_set_size = disks/fc;
		WARN(geo->far_set_size < fc,
		     "This RAID10 layout does not provide data safety - please backup and create new array\n");
		break;
	case 2: /* "improved" layout fixed to match documentation */
		geo->far_set_size = fc * nc;
		break;
	default: /* Not a valid layout */
		return -1;
	}
	geo->chunk_mask = chunk - 1;
	geo->chunk_shift = ffz(~chunk);
	return nc*fc;
}

static void raid10_free_conf(struct r10conf *conf)
{
	if (!conf)
		return;

	mempool_exit(&conf->r10bio_pool);
	kfree(conf->mirrors);
	kfree(conf->mirrors_old);
	kfree(conf->mirrors_new);
	safe_put_page(conf->tmppage);
	bioset_exit(&conf->bio_split);
	kfree(conf);
}

static struct r10conf *setup_conf(struct mddev *mddev)
{
	struct r10conf *conf = NULL;
	int err = -EINVAL;
	struct geom geo;
	int copies;

	copies = setup_geo(&geo, mddev, geo_new);

	if (copies == -2) {
		pr_warn("md/raid10:%s: chunk size must be at least PAGE_SIZE(%ld) and be a power of 2.\n",
			mdname(mddev), PAGE_SIZE);
		goto out;
	}

	if (copies < 2 || copies > mddev->raid_disks) {
		pr_warn("md/raid10:%s: unsupported raid10 layout: 0x%8x\n",
			mdname(mddev), mddev->new_layout);
		goto out;
	}

	err = -ENOMEM;
	conf = kzalloc(sizeof(struct r10conf), GFP_KERNEL);
	if (!conf)
		goto out;

	/* FIXME calc properly */
	conf->mirrors = kcalloc(mddev->raid_disks + max(0, -mddev->delta_disks),
				sizeof(struct raid10_info),
				GFP_KERNEL);
	if (!conf->mirrors)
		goto out;

	conf->tmppage = alloc_page(GFP_KERNEL);
	if (!conf->tmppage)
		goto out;

	conf->geo = geo;
	conf->copies = copies;
	err = mempool_init(&conf->r10bio_pool, NR_RAID_BIOS, r10bio_pool_alloc,
			   rbio_pool_free, conf);
	if (err)
		goto out;

	err = bioset_init(&conf->bio_split, BIO_POOL_SIZE, 0, 0);
	if (err)
		goto out;

	calc_sectors(conf, mddev->dev_sectors);
	if (mddev->reshape_position == MaxSector) {
		conf->prev = conf->geo;
		conf->reshape_progress = MaxSector;
	} else {
		if (setup_geo(&conf->prev, mddev, geo_old) != conf->copies) {
			err = -EINVAL;
			goto out;
		}
		conf->reshape_progress = mddev->reshape_position;
		if (conf->prev.far_offset)
			conf->prev.stride = 1 << conf->prev.chunk_shift;
		else
			/* far_copies must be 1 */
			conf->prev.stride = conf->dev_sectors;
	}
	conf->reshape_safe = conf->reshape_progress;
	spin_lock_init(&conf->device_lock);
	INIT_LIST_HEAD(&conf->retry_list);
	INIT_LIST_HEAD(&conf->bio_end_io_list);

	seqlock_init(&conf->resync_lock);
	init_waitqueue_head(&conf->wait_barrier);
	atomic_set(&conf->nr_pending, 0);

	err = -ENOMEM;
	rcu_assign_pointer(conf->thread,
			   md_register_thread(raid10d, mddev, "raid10"));
	if (!conf->thread)
		goto out;

	conf->mddev = mddev;
	return conf;

 out:
	raid10_free_conf(conf);
	return ERR_PTR(err);
}

static unsigned int raid10_nr_stripes(struct r10conf *conf)
{
	unsigned int raid_disks = conf->geo.raid_disks;

	if (conf->geo.raid_disks % conf->geo.near_copies)
		return raid_disks;
	return raid_disks / conf->geo.near_copies;
}

static int raid10_set_queue_limits(struct mddev *mddev)
{
	struct r10conf *conf = mddev->private;
	struct queue_limits lim;
	int err;

	md_init_stacking_limits(&lim);
	lim.max_write_zeroes_sectors = 0;
	lim.io_min = mddev->chunk_sectors << 9;
	lim.chunk_sectors = mddev->chunk_sectors;
	lim.io_opt = lim.io_min * raid10_nr_stripes(conf);
	lim.features |= BLK_FEAT_ATOMIC_WRITES;
	err = mddev_stack_rdev_limits(mddev, &lim, MDDEV_STACK_INTEGRITY);
	if (err)
		return err;
	return queue_limits_set(mddev->gendisk->queue, &lim);
}

static int raid10_run(struct mddev *mddev)
{
	struct r10conf *conf;
	int i, disk_idx;
	struct raid10_info *disk;
	struct md_rdev *rdev;
	sector_t size;
	sector_t min_offset_diff = 0;
	int first = 1;
	int ret = -EIO;

	if (mddev->private == NULL) {
		conf = setup_conf(mddev);
		if (IS_ERR(conf))
			return PTR_ERR(conf);
		mddev->private = conf;
	}
	conf = mddev->private;
	if (!conf)
		goto out;

	rcu_assign_pointer(mddev->thread, conf->thread);
	rcu_assign_pointer(conf->thread, NULL);

	if (mddev_is_clustered(conf->mddev)) {
		int fc, fo;

		fc = (mddev->layout >> 8) & 255;
		fo = mddev->layout & (1<<16);
		if (fc > 1 || fo > 0) {
			pr_err("only near layout is supported by clustered"
				" raid10\n");
			goto out_free_conf;
		}
	}

	rdev_for_each(rdev, mddev) {
		long long diff;

		disk_idx = rdev->raid_disk;
		if (disk_idx < 0)
			continue;
		if (disk_idx >= conf->geo.raid_disks &&
		    disk_idx >= conf->prev.raid_disks)
			continue;
		disk = conf->mirrors + disk_idx;

		if (test_bit(Replacement, &rdev->flags)) {
			if (disk->replacement)
				goto out_free_conf;
			disk->replacement = rdev;
		} else {
			if (disk->rdev)
				goto out_free_conf;
			disk->rdev = rdev;
		}
		diff = (rdev->new_data_offset - rdev->data_offset);
		if (!mddev->reshape_backwards)
			diff = -diff;
		if (diff < 0)
			diff = 0;
		if (first || diff < min_offset_diff)
			min_offset_diff = diff;

		disk->head_position = 0;
		first = 0;
	}

	if (!mddev_is_dm(conf->mddev)) {
		int err = raid10_set_queue_limits(mddev);

		if (err) {
			ret = err;
			goto out_free_conf;
		}
	}

	/* need to check that every block has at least one working mirror */
	if (!enough(conf, -1)) {
		pr_err("md/raid10:%s: not enough operational mirrors.\n",
		       mdname(mddev));
		goto out_free_conf;
	}

	if (conf->reshape_progress != MaxSector) {
		/* must ensure that shape change is supported */
		if (conf->geo.far_copies != 1 &&
		    conf->geo.far_offset == 0)
			goto out_free_conf;
		if (conf->prev.far_copies != 1 &&
		    conf->prev.far_offset == 0)
			goto out_free_conf;
	}

	mddev->degraded = 0;
	for (i = 0;
	     i < conf->geo.raid_disks
		     || i < conf->prev.raid_disks;
	     i++) {

		disk = conf->mirrors + i;

		if (!disk->rdev && disk->replacement) {
			/* The replacement is all we have - use it */
			disk->rdev = disk->replacement;
			disk->replacement = NULL;
			clear_bit(Replacement, &disk->rdev->flags);
		}

		if (!disk->rdev ||
		    !test_bit(In_sync, &disk->rdev->flags)) {
			disk->head_position = 0;
			mddev->degraded++;
			if (disk->rdev &&
			    disk->rdev->saved_raid_disk < 0)
				conf->fullsync = 1;
		}

		if (disk->replacement &&
		    !test_bit(In_sync, &disk->replacement->flags) &&
		    disk->replacement->saved_raid_disk < 0) {
			conf->fullsync = 1;
		}

		disk->recovery_disabled = mddev->recovery_disabled - 1;
	}

	if (mddev->recovery_cp != MaxSector)
		pr_notice("md/raid10:%s: not clean -- starting background reconstruction\n",
			  mdname(mddev));
	pr_info("md/raid10:%s: active with %d out of %d devices\n",
		mdname(mddev), conf->geo.raid_disks - mddev->degraded,
		conf->geo.raid_disks);
	/*
	 * Ok, everything is just fine now
	 */
	mddev->dev_sectors = conf->dev_sectors;
	size = raid10_size(mddev, 0, 0);
	md_set_array_sectors(mddev, size);
	mddev->resync_max_sectors = size;
	set_bit(MD_FAILFAST_SUPPORTED, &mddev->flags);

	if (md_integrity_register(mddev))
		goto out_free_conf;

	if (conf->reshape_progress != MaxSector) {
		unsigned long before_length, after_length;

		before_length = ((1 << conf->prev.chunk_shift) *
				 conf->prev.far_copies);
		after_length = ((1 << conf->geo.chunk_shift) *
				conf->geo.far_copies);

		if (max(before_length, after_length) > min_offset_diff) {
			/* This cannot work */
			pr_warn("md/raid10: offset difference not enough to continue reshape\n");
			goto out_free_conf;
		}
		conf->offset_diff = min_offset_diff;

		clear_bit(MD_RECOVERY_SYNC, &mddev->recovery);
		clear_bit(MD_RECOVERY_CHECK, &mddev->recovery);
		set_bit(MD_RECOVERY_RESHAPE, &mddev->recovery);
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	}

	return 0;

out_free_conf:
	md_unregister_thread(mddev, &mddev->thread);
	raid10_free_conf(conf);
	mddev->private = NULL;
out:
	return ret;
}

static void raid10_free(struct mddev *mddev, void *priv)
{
	raid10_free_conf(priv);
}

static void raid10_quiesce(struct mddev *mddev, int quiesce)
{
	struct r10conf *conf = mddev->private;

	if (quiesce)
		raise_barrier(conf, 0);
	else
		lower_barrier(conf);
}

static int raid10_resize(struct mddev *mddev, sector_t sectors)
{
	/* Resize of 'far' arrays is not supported.
	 * For 'near' and 'offset' arrays we can set the
	 * number of sectors used to be an appropriate multiple
	 * of the chunk size.
	 * For 'offset', this is far_copies*chunksize.
	 * For 'near' the multiplier is the LCM of
	 * near_copies and raid_disks.
	 * So if far_copies > 1 && !far_offset, fail.
	 * Else find LCM(raid_disks, near_copy)*far_copies and
	 * multiply by chunk_size.  Then round to this number.
	 * This is mostly done by raid10_size()
	 */
	struct r10conf *conf = mddev->private;
	sector_t oldsize, size;
	int ret;

	if (mddev->reshape_position != MaxSector)
		return -EBUSY;

	if (conf->geo.far_copies > 1 && !conf->geo.far_offset)
		return -EINVAL;

	oldsize = raid10_size(mddev, 0, 0);
	size = raid10_size(mddev, sectors, 0);
	if (mddev->external_size &&
	    mddev->array_sectors > size)
		return -EINVAL;

	ret = mddev->bitmap_ops->resize(mddev, size, 0, false);
	if (ret)
		return ret;

	md_set_array_sectors(mddev, size);
	if (sectors > mddev->dev_sectors &&
	    mddev->recovery_cp > oldsize) {
		mddev->recovery_cp = oldsize;
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	}
	calc_sectors(conf, sectors);
	mddev->dev_sectors = conf->dev_sectors;
	mddev->resync_max_sectors = size;
	return 0;
}

static void *raid10_takeover_raid0(struct mddev *mddev, sector_t size, int devs)
{
	struct md_rdev *rdev;
	struct r10conf *conf;

	if (mddev->degraded > 0) {
		pr_warn("md/raid10:%s: Error: degraded raid0!\n",
			mdname(mddev));
		return ERR_PTR(-EINVAL);
	}
	sector_div(size, devs);

	/* Set new parameters */
	mddev->new_level = 10;
	/* new layout: far_copies = 1, near_copies = 2 */
	mddev->new_layout = (1<<8) + 2;
	mddev->new_chunk_sectors = mddev->chunk_sectors;
	mddev->delta_disks = mddev->raid_disks;
	mddev->raid_disks *= 2;
	/* make sure it will be not marked as dirty */
	mddev->recovery_cp = MaxSector;
	mddev->dev_sectors = size;

	conf = setup_conf(mddev);
	if (!IS_ERR(conf)) {
		rdev_for_each(rdev, mddev)
			if (rdev->raid_disk >= 0) {
				rdev->new_raid_disk = rdev->raid_disk * 2;
				rdev->sectors = size;
			}
	}

	return conf;
}

static void *raid10_takeover(struct mddev *mddev)
{
	struct r0conf *raid0_conf;

	/* raid10 can take over:
	 *  raid0 - providing it has only two drives
	 */
	if (mddev->level == 0) {
		/* for raid0 takeover only one zone is supported */
		raid0_conf = mddev->private;
		if (raid0_conf->nr_strip_zones > 1) {
			pr_warn("md/raid10:%s: cannot takeover raid 0 with more than one zone.\n",
				mdname(mddev));
			return ERR_PTR(-EINVAL);
		}
		return raid10_takeover_raid0(mddev,
			raid0_conf->strip_zone->zone_end,
			raid0_conf->strip_zone->nb_dev);
	}
	return ERR_PTR(-EINVAL);
}

static int raid10_check_reshape(struct mddev *mddev)
{
	/* Called when there is a request to change
	 * - layout (to ->new_layout)
	 * - chunk size (to ->new_chunk_sectors)
	 * - raid_disks (by delta_disks)
	 * or when trying to restart a reshape that was ongoing.
	 *
	 * We need to validate the request and possibly allocate
	 * space if that might be an issue later.
	 *
	 * Currently we reject any reshape of a 'far' mode array,
	 * allow chunk size to change if new is generally acceptable,
	 * allow raid_disks to increase, and allow
	 * a switch between 'near' mode and 'offset' mode.
	 */
	struct r10conf *conf = mddev->private;
	struct geom geo;

	if (conf->geo.far_copies != 1 && !conf->geo.far_offset)
		return -EINVAL;

	if (setup_geo(&geo, mddev, geo_start) != conf->copies)
		/* mustn't change number of copies */
		return -EINVAL;
	if (geo.far_copies > 1 && !geo.far_offset)
		/* Cannot switch to 'far' mode */
		return -EINVAL;

	if (mddev->array_sectors & geo.chunk_mask)
			/* not factor of array size */
			return -EINVAL;

	if (!enough(conf, -1))
		return -EINVAL;

	kfree(conf->mirrors_new);
	conf->mirrors_new = NULL;
	if (mddev->delta_disks > 0) {
		/* allocate new 'mirrors' list */
		conf->mirrors_new =
			kcalloc(mddev->raid_disks + mddev->delta_disks,
				sizeof(struct raid10_info),
				GFP_KERNEL);
		if (!conf->mirrors_new)
			return -ENOMEM;
	}
	return 0;
}

/*
 * Need to check if array has failed when deciding whether to:
 *  - start an array
 *  - remove non-faulty devices
 *  - add a spare
 *  - allow a reshape
 * This determination is simple when no reshape is happening.
 * However if there is a reshape, we need to carefully check
 * both the before and after sections.
 * This is because some failed devices may only affect one
 * of the two sections, and some non-in_sync devices may
 * be insync in the section most affected by failed devices.
 */
static int calc_degraded(struct r10conf *conf)
{
	int degraded, degraded2;
	int i;

	degraded = 0;
	/* 'prev' section first */
	for (i = 0; i < conf->prev.raid_disks; i++) {
		struct md_rdev *rdev = conf->mirrors[i].rdev;

		if (!rdev || test_bit(Faulty, &rdev->flags))
			degraded++;
		else if (!test_bit(In_sync, &rdev->flags))
			/* When we can reduce the number of devices in
			 * an array, this might not contribute to
			 * 'degraded'.  It does now.
			 */
			degraded++;
	}
	if (conf->geo.raid_disks == conf->prev.raid_disks)
		return degraded;
	degraded2 = 0;
	for (i = 0; i < conf->geo.raid_disks; i++) {
		struct md_rdev *rdev = conf->mirrors[i].rdev;

		if (!rdev || test_bit(Faulty, &rdev->flags))
			degraded2++;
		else if (!test_bit(In_sync, &rdev->flags)) {
			/* If reshape is increasing the number of devices,
			 * this section has already been recovered, so
			 * it doesn't contribute to degraded.
			 * else it does.
			 */
			if (conf->geo.raid_disks <= conf->prev.raid_disks)
				degraded2++;
		}
	}
	if (degraded2 > degraded)
		return degraded2;
	return degraded;
}

static int raid10_start_reshape(struct mddev *mddev)
{
	/* A 'reshape' has been requested. This commits
	 * the various 'new' fields and sets MD_RECOVER_RESHAPE
	 * This also checks if there are enough spares and adds them
	 * to the array.
	 * We currently require enough spares to make the final
	 * array non-degraded.  We also require that the difference
	 * between old and new data_offset - on each device - is
	 * enough that we never risk over-writing.
	 */

	unsigned long before_length, after_length;
	sector_t min_offset_diff = 0;
	int first = 1;
	struct geom new;
	struct r10conf *conf = mddev->private;
	struct md_rdev *rdev;
	int spares = 0;
	int ret;

	if (test_bit(MD_RECOVERY_RUNNING, &mddev->recovery))
		return -EBUSY;

	if (setup_geo(&new, mddev, geo_start) != conf->copies)
		return -EINVAL;

	before_length = ((1 << conf->prev.chunk_shift) *
			 conf->prev.far_copies);
	after_length = ((1 << conf->geo.chunk_shift) *
			conf->geo.far_copies);

	rdev_for_each(rdev, mddev) {
		if (!test_bit(In_sync, &rdev->flags)
		    && !test_bit(Faulty, &rdev->flags))
			spares++;
		if (rdev->raid_disk >= 0) {
			long long diff = (rdev->new_data_offset
					  - rdev->data_offset);
			if (!mddev->reshape_backwards)
				diff = -diff;
			if (diff < 0)
				diff = 0;
			if (first || diff < min_offset_diff)
				min_offset_diff = diff;
			first = 0;
		}
	}

	if (max(before_length, after_length) > min_offset_diff)
		return -EINVAL;

	if (spares < mddev->delta_disks)
		return -EINVAL;

	conf->offset_diff = min_offset_diff;
	spin_lock_irq(&conf->device_lock);
	if (conf->mirrors_new) {
		memcpy(conf->mirrors_new, conf->mirrors,
		       sizeof(struct raid10_info)*conf->prev.raid_disks);
		smp_mb();
		kfree(conf->mirrors_old);
		conf->mirrors_old = conf->mirrors;
		conf->mirrors = conf->mirrors_new;
		conf->mirrors_new = NULL;
	}
	setup_geo(&conf->geo, mddev, geo_start);
	smp_mb();
	if (mddev->reshape_backwards) {
		sector_t size = raid10_size(mddev, 0, 0);
		if (size < mddev->array_sectors) {
			spin_unlock_irq(&conf->device_lock);
			pr_warn("md/raid10:%s: array size must be reduce before number of disks\n",
				mdname(mddev));
			return -EINVAL;
		}
		mddev->resync_max_sectors = size;
		conf->reshape_progress = size;
	} else
		conf->reshape_progress = 0;
	conf->reshape_safe = conf->reshape_progress;
	spin_unlock_irq(&conf->device_lock);

	if (mddev->delta_disks && mddev->bitmap) {
		struct mdp_superblock_1 *sb = NULL;
		sector_t oldsize, newsize;

		oldsize = raid10_size(mddev, 0, 0);
		newsize = raid10_size(mddev, 0, conf->geo.raid_disks);

		if (!mddev_is_clustered(mddev)) {
			ret = mddev->bitmap_ops->resize(mddev, newsize, 0, false);
			if (ret)
				goto abort;
			else
				goto out;
		}

		rdev_for_each(rdev, mddev) {
			if (rdev->raid_disk > -1 &&
			    !test_bit(Faulty, &rdev->flags))
				sb = page_address(rdev->sb_page);
		}

		/*
		 * some node is already performing reshape, and no need to
		 * call bitmap_ops->resize again since it should be called when
		 * receiving BITMAP_RESIZE msg
		 */
		if ((sb && (le32_to_cpu(sb->feature_map) &
			    MD_FEATURE_RESHAPE_ACTIVE)) || (oldsize == newsize))
			goto out;

		ret = mddev->bitmap_ops->resize(mddev, newsize, 0, false);
		if (ret)
			goto abort;

		ret = mddev->cluster_ops->resize_bitmaps(mddev, newsize, oldsize);
		if (ret) {
			mddev->bitmap_ops->resize(mddev, oldsize, 0, false);
			goto abort;
		}
	}
out:
	if (mddev->delta_disks > 0) {
		rdev_for_each(rdev, mddev)
			if (rdev->raid_disk < 0 &&
			    !test_bit(Faulty, &rdev->flags)) {
				if (raid10_add_disk(mddev, rdev) == 0) {
					if (rdev->raid_disk >=
					    conf->prev.raid_disks)
						set_bit(In_sync, &rdev->flags);
					else
						rdev->recovery_offset = 0;

					/* Failure here is OK */
					sysfs_link_rdev(mddev, rdev);
				}
			} else if (rdev->raid_disk >= conf->prev.raid_disks
				   && !test_bit(Faulty, &rdev->flags)) {
				/* This is a spare that was manually added */
				set_bit(In_sync, &rdev->flags);
			}
	}
	/* When a reshape changes the number of devices,
	 * ->degraded is measured against the larger of the
	 * pre and  post numbers.
	 */
	spin_lock_irq(&conf->device_lock);
	mddev->degraded = calc_degraded(conf);
	spin_unlock_irq(&conf->device_lock);
	mddev->raid_disks = conf->geo.raid_disks;
	mddev->reshape_position = conf->reshape_progress;
	set_bit(MD_SB_CHANGE_DEVS, &mddev->sb_flags);

	clear_bit(MD_RECOVERY_SYNC, &mddev->recovery);
	clear_bit(MD_RECOVERY_CHECK, &mddev->recovery);
	clear_bit(MD_RECOVERY_DONE, &mddev->recovery);
	set_bit(MD_RECOVERY_RESHAPE, &mddev->recovery);
	set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	conf->reshape_checkpoint = jiffies;
	md_new_event();
	return 0;

abort:
	mddev->recovery = 0;
	spin_lock_irq(&conf->device_lock);
	conf->geo = conf->prev;
	mddev->raid_disks = conf->geo.raid_disks;
	rdev_for_each(rdev, mddev)
		rdev->new_data_offset = rdev->data_offset;
	smp_wmb();
	conf->reshape_progress = MaxSector;
	conf->reshape_safe = MaxSector;
	mddev->reshape_position = MaxSector;
	spin_unlock_irq(&conf->device_lock);
	return ret;
}

/* Calculate the last device-address that could contain
 * any block from the chunk that includes the array-address 's'
 * and report the next address.
 * i.e. the address returned will be chunk-aligned and after
 * any data that is in the chunk containing 's'.
 */
static sector_t last_dev_address(sector_t s, struct geom *geo)
{
	s = (s | geo->chunk_mask) + 1;
	s >>= geo->chunk_shift;
	s *= geo->near_copies;
	s = DIV_ROUND_UP_SECTOR_T(s, geo->raid_disks);
	s *= geo->far_copies;
	s <<= geo->chunk_shift;
	return s;
}

/* Calculate the first device-address that could contain
 * any block from the chunk that includes the array-address 's'.
 * This too will be the start of a chunk
 */
static sector_t first_dev_address(sector_t s, struct geom *geo)
{
	s >>= geo->chunk_shift;
	s *= geo->near_copies;
	sector_div(s, geo->raid_disks);
	s *= geo->far_copies;
	s <<= geo->chunk_shift;
	return s;
}

static sector_t reshape_request(struct mddev *mddev, sector_t sector_nr,
				int *skipped)
{
	/* We simply copy at most one chunk (smallest of old and new)
	 * at a time, possibly less if that exceeds RESYNC_PAGES,
	 * or we hit a bad block or something.
	 * This might mean we pause for normal IO in the middle of
	 * a chunk, but that is not a problem as mddev->reshape_position
	 * can record any location.
	 *
	 * If we will want to write to a location that isn't
	 * yet recorded as 'safe' (i.e. in metadata on disk) then
	 * we need to flush all reshape requests and update the metadata.
	 *
	 * When reshaping forwards (e.g. to more devices), we interpret
	 * 'safe' as the earliest block which might not have been copied
	 * down yet.  We divide this by previous stripe size and multiply
	 * by previous stripe length to get lowest device offset that we
	 * cannot write to yet.
	 * We interpret 'sector_nr' as an address that we want to write to.
	 * From this we use last_device_address() to find where we might
	 * write to, and first_device_address on the  'safe' position.
	 * If this 'next' write position is after the 'safe' position,
	 * we must update the metadata to increase the 'safe' position.
	 *
	 * When reshaping backwards, we round in the opposite direction
	 * and perform the reverse test:  next write position must not be
	 * less than current safe position.
	 *
	 * In all this the minimum difference in data offsets
	 * (conf->offset_diff - always positive) allows a bit of slack,
	 * so next can be after 'safe', but not by more than offset_diff
	 *
	 * We need to prepare all the bios here before we start any IO
	 * to ensure the size we choose is acceptable to all devices.
	 * The means one for each copy for write-out and an extra one for
	 * read-in.
	 * We store the read-in bio in ->master_bio and the others in
	 * ->devs[x].bio and ->devs[x].repl_bio.
	 */
	struct r10conf *conf = mddev->private;
	struct r10bio *r10_bio;
	sector_t next, safe, last;
	int max_sectors;
	int nr_sectors;
	int s;
	struct md_rdev *rdev;
	int need_flush = 0;
	struct bio *blist;
	struct bio *bio, *read_bio;
	int sectors_done = 0;
	struct page **pages;

	if (sector_nr == 0) {
		/* If restarting in the middle, skip the initial sectors */
		if (mddev->reshape_backwards &&
		    conf->reshape_progress < raid10_size(mddev, 0, 0)) {
			sector_nr = (raid10_size(mddev, 0, 0)
				     - conf->reshape_progress);
		} else if (!mddev->reshape_backwards &&
			   conf->reshape_progress > 0)
			sector_nr = conf->reshape_progress;
		if (sector_nr) {
			mddev->curr_resync_completed = sector_nr;
			sysfs_notify_dirent_safe(mddev->sysfs_completed);
			*skipped = 1;
			return sector_nr;
		}
	}

	/* We don't use sector_nr to track where we are up to
	 * as that doesn't work well for ->reshape_backwards.
	 * So just use ->reshape_progress.
	 */
	if (mddev->reshape_backwards) {
		/* 'next' is the earliest device address that we might
		 * write to for this chunk in the new layout
		 */
		next = first_dev_address(conf->reshape_progress - 1,
					 &conf->geo);

		/* 'safe' is the last device address that we might read from
		 * in the old layout after a restart
		 */
		safe = last_dev_address(conf->reshape_safe - 1,
					&conf->prev);

		if (next + conf->offset_diff < safe)
			need_flush = 1;

		last = conf->reshape_progress - 1;
		sector_nr = last & ~(sector_t)(conf->geo.chunk_mask
					       & conf->prev.chunk_mask);
		if (sector_nr + RESYNC_SECTORS < last)
			sector_nr = last + 1 - RESYNC_SECTORS;
	} else {
		/* 'next' is after the last device address that we
		 * might write to for this chunk in the new layout
		 */
		next = last_dev_address(conf->reshape_progress, &conf->geo);

		/* 'safe' is the earliest device address that we might
		 * read from in the old layout after a restart
		 */
		safe = first_dev_address(conf->reshape_safe, &conf->prev);

		/* Need to update metadata if 'next' might be beyond 'safe'
		 * as that would possibly corrupt data
		 */
		if (next > safe + conf->offset_diff)
			need_flush = 1;

		sector_nr = conf->reshape_progress;
		last  = sector_nr | (conf->geo.chunk_mask
				     & conf->prev.chunk_mask);

		if (sector_nr + RESYNC_SECTORS <= last)
			last = sector_nr + RESYNC_SECTORS - 1;
	}

	if (need_flush ||
	    time_after(jiffies, conf->reshape_checkpoint + 10*HZ)) {
		/* Need to update reshape_position in metadata */
		wait_barrier(conf, false);
		mddev->reshape_position = conf->reshape_progress;
		if (mddev->reshape_backwards)
			mddev->curr_resync_completed = raid10_size(mddev, 0, 0)
				- conf->reshape_progress;
		else
			mddev->curr_resync_completed = conf->reshape_progress;
		conf->reshape_checkpoint = jiffies;
		set_bit(MD_SB_CHANGE_DEVS, &mddev->sb_flags);
		md_wakeup_thread(mddev->thread);
		wait_event(mddev->sb_wait, mddev->sb_flags == 0 ||
			   test_bit(MD_RECOVERY_INTR, &mddev->recovery));
		if (test_bit(MD_RECOVERY_INTR, &mddev->recovery)) {
			allow_barrier(conf);
			return sectors_done;
		}
		conf->reshape_safe = mddev->reshape_position;
		allow_barrier(conf);
	}

	raise_barrier(conf, 0);
read_more:
	/* Now schedule reads for blocks from sector_nr to last */
	r10_bio = raid10_alloc_init_r10buf(conf);
	r10_bio->state = 0;
	raise_barrier(conf, 1);
	atomic_set(&r10_bio->remaining, 0);
	r10_bio->mddev = mddev;
	r10_bio->sector = sector_nr;
	set_bit(R10BIO_IsReshape, &r10_bio->state);
	r10_bio->sectors = last - sector_nr + 1;
	rdev = read_balance(conf, r10_bio, &max_sectors);
	BUG_ON(!test_bit(R10BIO_Previous, &r10_bio->state));

	if (!rdev) {
		/* Cannot read from here, so need to record bad blocks
		 * on all the target devices.
		 */
		// FIXME
		mempool_free(r10_bio, &conf->r10buf_pool);
		set_bit(MD_RECOVERY_INTR, &mddev->recovery);
		return sectors_done;
	}

	read_bio = bio_alloc_bioset(rdev->bdev, RESYNC_PAGES, REQ_OP_READ,
				    GFP_KERNEL, &mddev->bio_set);
	read_bio->bi_iter.bi_sector = (r10_bio->devs[r10_bio->read_slot].addr
			       + rdev->data_offset);
	read_bio->bi_private = r10_bio;
	read_bio->bi_end_io = end_reshape_read;
	r10_bio->master_bio = read_bio;
	r10_bio->read_slot = r10_bio->devs[r10_bio->read_slot].devnum;

	/*
	 * Broadcast RESYNC message to other nodes, so all nodes would not
	 * write to the region to avoid conflict.
	*/
	if (mddev_is_clustered(mddev) && conf->cluster_sync_high <= sector_nr) {
		struct mdp_superblock_1 *sb = NULL;
		int sb_reshape_pos = 0;

		conf->cluster_sync_low = sector_nr;
		conf->cluster_sync_high = sector_nr + CLUSTER_RESYNC_WINDOW_SECTORS;
		sb = page_address(rdev->sb_page);
		if (sb) {
			sb_reshape_pos = le64_to_cpu(sb->reshape_position);
			/*
			 * Set cluster_sync_low again if next address for array
			 * reshape is less than cluster_sync_low. Since we can't
			 * update cluster_sync_low until it has finished reshape.
			 */
			if (sb_reshape_pos < conf->cluster_sync_low)
				conf->cluster_sync_low = sb_reshape_pos;
		}

		mddev->cluster_ops->resync_info_update(mddev, conf->cluster_sync_low,
							  conf->cluster_sync_high);
	}

	/* Now find the locations in the new layout */
	__raid10_find_phys(&conf->geo, r10_bio);

	blist = read_bio;
	read_bio->bi_next = NULL;

	for (s = 0; s < conf->copies*2; s++) {
		struct bio *b;
		int d = r10_bio->devs[s/2].devnum;
		struct md_rdev *rdev2;
		if (s&1) {
			rdev2 = conf->mirrors[d].replacement;
			b = r10_bio->devs[s/2].repl_bio;
		} else {
			rdev2 = conf->mirrors[d].rdev;
			b = r10_bio->devs[s/2].bio;
		}
		if (!rdev2 || test_bit(Faulty, &rdev2->flags))
			continue;

		bio_set_dev(b, rdev2->bdev);
		b->bi_iter.bi_sector = r10_bio->devs[s/2].addr +
			rdev2->new_data_offset;
		b->bi_end_io = end_reshape_write;
		b->bi_opf = REQ_OP_WRITE;
		b->bi_next = blist;
		blist = b;
	}

	/* Now add as many pages as possible to all of these bios. */

	nr_sectors = 0;
	pages = get_resync_pages(r10_bio->devs[0].bio)->pages;
	for (s = 0 ; s < max_sectors; s += PAGE_SIZE >> 9) {
		struct page *page = pages[s / (PAGE_SIZE >> 9)];
		int len = (max_sectors - s) << 9;
		if (len > PAGE_SIZE)
			len = PAGE_SIZE;
		for (bio = blist; bio ; bio = bio->bi_next) {
			if (WARN_ON(!bio_add_page(bio, page, len, 0))) {
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return sectors_done;
			}
		}
		sector_nr += len >> 9;
		nr_sectors += len >> 9;
	}
	r10_bio->sectors = nr_sectors;

	/* Now submit the read */
	atomic_inc(&r10_bio->remaining);
	read_bio->bi_next = NULL;
	submit_bio_noacct(read_bio);
	sectors_done += nr_sectors;
	if (sector_nr <= last)
		goto read_more;

	lower_barrier(conf);

	/* Now that we have done the whole section we can
	 * update reshape_progress
	 */
	if (mddev->reshape_backwards)
		conf->reshape_progress -= sectors_done;
	else
		conf->reshape_progress += sectors_done;

	return sectors_done;
}

static void end_reshape_request(struct r10bio *r10_bio);
static int handle_reshape_read_error(struct mddev *mddev,
				     struct r10bio *r10_bio);
static void reshape_request_write(struct mddev *mddev, struct r10bio *r10_bio)
{
	/* Reshape read completed.  Hopefully we have a block
	 * to write out.
	 * If we got a read error then we do sync 1-page reads from
	 * elsewhere until we find the data - or give up.
	 */
	struct r10conf *conf = mddev->private;
	int s;

	if (!test_bit(R10BIO_Uptodate, &r10_bio->state))
		if (handle_reshape_read_error(mddev, r10_bio) < 0) {
			/* Reshape has been aborted */
			md_done_sync(mddev, r10_bio->sectors, 0);
			return;
		}

	/* We definitely have the data in the pages, schedule the
	 * writes.
	 */
	atomic_set(&r10_bio->remaining, 1);
	for (s = 0; s < conf->copies*2; s++) {
		struct bio *b;
		int d = r10_bio->devs[s/2].devnum;
		struct md_rdev *rdev;
		if (s&1) {
			rdev = conf->mirrors[d].replacement;
			b = r10_bio->devs[s/2].repl_bio;
		} else {
			rdev = conf->mirrors[d].rdev;
			b = r10_bio->devs[s/2].bio;
		}
		if (!rdev || test_bit(Faulty, &rdev->flags))
			continue;

		atomic_inc(&rdev->nr_pending);
		atomic_inc(&r10_bio->remaining);
		b->bi_next = NULL;
		submit_bio_noacct(b);
	}
	end_reshape_request(r10_bio);
}

static void end_reshape(struct r10conf *conf)
{
	if (test_bit(MD_RECOVERY_INTR, &conf->mddev->recovery))
		return;

	spin_lock_irq(&conf->device_lock);
	conf->prev = conf->geo;
	md_finish_reshape(conf->mddev);
	smp_wmb();
	conf->reshape_progress = MaxSector;
	conf->reshape_safe = MaxSector;
	spin_unlock_irq(&conf->device_lock);

	mddev_update_io_opt(conf->mddev, raid10_nr_stripes(conf));
	conf->fullsync = 0;
}

static void raid10_update_reshape_pos(struct mddev *mddev)
{
	struct r10conf *conf = mddev->private;
	sector_t lo, hi;

	mddev->cluster_ops->resync_info_get(mddev, &lo, &hi);
	if (((mddev->reshape_position <= hi) && (mddev->reshape_position >= lo))
	    || mddev->reshape_position == MaxSector)
		conf->reshape_progress = mddev->reshape_position;
	else
		WARN_ON_ONCE(1);
}

static int handle_reshape_read_error(struct mddev *mddev,
				     struct r10bio *r10_bio)
{
	/* Use sync reads to get the blocks from somewhere else */
	int sectors = r10_bio->sectors;
	struct r10conf *conf = mddev->private;
	struct r10bio *r10b;
	int slot = 0;
	int idx = 0;
	struct page **pages;

	r10b = kmalloc(struct_size(r10b, devs, conf->copies), GFP_NOIO);
	if (!r10b) {
		set_bit(MD_RECOVERY_INTR, &mddev->recovery);
		return -ENOMEM;
	}

	/* reshape IOs share pages from .devs[0].bio */
	pages = get_resync_pages(r10_bio->devs[0].bio)->pages;

	r10b->sector = r10_bio->sector;
	__raid10_find_phys(&conf->prev, r10b);

	while (sectors) {
		int s = sectors;
		int success = 0;
		int first_slot = slot;

		if (s > (PAGE_SIZE >> 9))
			s = PAGE_SIZE >> 9;

		while (!success) {
			int d = r10b->devs[slot].devnum;
			struct md_rdev *rdev = conf->mirrors[d].rdev;
			sector_t addr;
			if (rdev == NULL ||
			    test_bit(Faulty, &rdev->flags) ||
			    !test_bit(In_sync, &rdev->flags))
				goto failed;

			addr = r10b->devs[slot].addr + idx * PAGE_SIZE;
			atomic_inc(&rdev->nr_pending);
			success = sync_page_io(rdev,
					       addr,
					       s << 9,
					       pages[idx],
					       REQ_OP_READ, false);
			rdev_dec_pending(rdev, mddev);
			if (success)
				break;
		failed:
			slot++;
			if (slot >= conf->copies)
				slot = 0;
			if (slot == first_slot)
				break;
		}
		if (!success) {
			/* couldn't read this block, must give up */
			set_bit(MD_RECOVERY_INTR,
				&mddev->recovery);
			kfree(r10b);
			return -EIO;
		}
		sectors -= s;
		idx++;
	}
	kfree(r10b);
	return 0;
}

static void end_reshape_write(struct bio *bio)
{
	struct r10bio *r10_bio = get_resync_r10bio(bio);
	struct mddev *mddev = r10_bio->mddev;
	struct r10conf *conf = mddev->private;
	int d;
	int slot;
	int repl;
	struct md_rdev *rdev = NULL;

	d = find_bio_disk(conf, r10_bio, bio, &slot, &repl);
	rdev = repl ? conf->mirrors[d].replacement :
		      conf->mirrors[d].rdev;

	if (bio->bi_status) {
		/* FIXME should record badblock */
		md_error(mddev, rdev);
	}

	rdev_dec_pending(rdev, mddev);
	end_reshape_request(r10_bio);
}

static void end_reshape_request(struct r10bio *r10_bio)
{
	if (!atomic_dec_and_test(&r10_bio->remaining))
		return;
	md_done_sync(r10_bio->mddev, r10_bio->sectors, 1);
	bio_put(r10_bio->master_bio);
	put_buf(r10_bio);
}

static void raid10_finish_reshape(struct mddev *mddev)
{
	struct r10conf *conf = mddev->private;

	if (test_bit(MD_RECOVERY_INTR, &mddev->recovery))
		return;

	if (mddev->delta_disks > 0) {
		if (mddev->recovery_cp > mddev->resync_max_sectors) {
			mddev->recovery_cp = mddev->resync_max_sectors;
			set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
		}
		mddev->resync_max_sectors = mddev->array_sectors;
	} else {
		int d;
		for (d = conf->geo.raid_disks ;
		     d < conf->geo.raid_disks - mddev->delta_disks;
		     d++) {
			struct md_rdev *rdev = conf->mirrors[d].rdev;
			if (rdev)
				clear_bit(In_sync, &rdev->flags);
			rdev = conf->mirrors[d].replacement;
			if (rdev)
				clear_bit(In_sync, &rdev->flags);
		}
	}
	mddev->layout = mddev->new_layout;
	mddev->chunk_sectors = 1 << conf->geo.chunk_shift;
	mddev->reshape_position = MaxSector;
	mddev->delta_disks = 0;
	mddev->reshape_backwards = 0;
}

static struct md_personality raid10_personality =
{
	.head = {
		.type	= MD_PERSONALITY,
		.id	= ID_RAID10,
		.name	= "raid10",
		.owner	= THIS_MODULE,
	},

	.make_request	= raid10_make_request,
	.run		= raid10_run,
	.free		= raid10_free,
	.status		= raid10_status,
	.error_handler	= raid10_error,
	.hot_add_disk	= raid10_add_disk,
	.hot_remove_disk= raid10_remove_disk,
	.spare_active	= raid10_spare_active,
	.sync_request	= raid10_sync_request,
	.quiesce	= raid10_quiesce,
	.size		= raid10_size,
	.resize		= raid10_resize,
	.takeover	= raid10_takeover,
	.check_reshape	= raid10_check_reshape,
	.start_reshape	= raid10_start_reshape,
	.finish_reshape	= raid10_finish_reshape,
	.update_reshape_pos = raid10_update_reshape_pos,
};

static int __init raid10_init(void)
{
	return register_md_submodule(&raid10_personality.head);
}

static void __exit raid10_exit(void)
{
	unregister_md_submodule(&raid10_personality.head);
}

module_init(raid10_init);
module_exit(raid10_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RAID10 (striped mirror) personality for MD");
MODULE_ALIAS("md-personality-9"); /* RAID10 */
MODULE_ALIAS("md-raid10");
MODULE_ALIAS("md-level-10");
