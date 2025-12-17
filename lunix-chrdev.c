/*
 * lunix-chrdev.c
 *
 * Implementation of character devices
 * for Lunix:TNG
 *
 * < Aristotles Georgios Dritsas ,Georgios Christophoros Tsimiakakis >
 *
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mmzone.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>

#include "lunix.h"
#include "lunix-chrdev.h"
#include "lunix-lookup.h"

/*
 * Global data
 */
struct cdev lunix_chrdev_cdev;

/*
 * Just a quick [unlocked] check to see if the cached
 * chrdev state needs to be updated from sensor measurements.
 */
/*
 * Declare a prototype so we can define the "unused" attribute and keep
 * the compiler happy. This function is not yet used, because this helpcode
 * is a stub.
 */
static int __attribute__((unused)) lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *);
static int lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *state)
{
        struct lunix_sensor_struct *sensor;

        WARN_ON ( !(sensor = state->sensor));

    struct lunix_msr_data_struct *msr_data;
    unsigned long flags;
    uint32_t last_update;
    int ret;

    sensor = state->sensor;

    spin_lock_irqsave(&sensor->lock, flags);

    msr_data = sensor->msr_data[state->type];

    last_update = msr_data->last_update;

    spin_unlock_irqrestore(&sensor->lock, flags);

    if (state->buf_timestamp < last_update)
        ret = 1;
    else
        ret = 0;

    return ret;
}

/*
 * Updates the cached state of a character device
 * based on sensor data. Must be called with the
 * character device state lock held.
 */
static int lunix_chrdev_state_update(struct lunix_chrdev_state_struct *state)
{
        struct lunix_sensor_struct *sensor;
        struct lunix_msr_data_struct *msr_data;
        unsigned long flags;
        uint32_t last_update;
        uint32_t raw;
        long value;
        long integer, frac;

        sensor = state->sensor;

        debug("starting update\n");

        /*
         * Grab the raw data quickly, hold the
         * spinlock for as little as possible.
         * Why use spinlocks? See LDD3, p. 119
         */
        spin_lock_irqsave(&sensor->lock, flags);

        msr_data = sensor->msr_data[state->type];

        /*
         * Any new data available?
         */
        last_update = msr_data->last_update;

        if (state->buf_timestamp != 0 &&
            last_update == state->buf_timestamp) {
                spin_unlock_irqrestore(&sensor->lock, flags);
                return -EAGAIN;
        }

        /* Grab the raw 16-bit sample */
        raw = msr_data->values[0];

        spin_unlock_irqrestore(&sensor->lock, flags);

        /*
         * Now we can take our time to format them,
         * holding only the private state semaphore
         */
        switch (state->type) {
        case TEMP:
                value = lookup_temperature[raw];
                break;
        case BATT:
                value = lookup_voltage[raw];
                break;
        case LIGHT:
                value = lookup_light[raw];
                break;
        default:
                return -EINVAL;
        }

        /* Convert value in milli-units → X.YYY */
        integer = value / 1000;
        frac    = value % 1000;
        if (frac < 0)
                frac = -frac;

        state->buf_lim = snprintf(state->buf_data, LUNIX_CHRDEV_BUFSZ,
                                  "%ld.%03ld\n", integer, frac);

        state->buf_timestamp = last_update;

        debug("leaving, value=%ld.%03ld ts=%u\n", integer, frac, last_update);

        return 0;
}

/*************************************
 * Implementation of file operations
 * for the Lunix character device
 *************************************/

static int lunix_chrdev_open(struct inode *inode, struct file *filp)
{
    /* Declarations */
    /* ? */
    struct lunix_chrdev_state_struct *state;
    struct lunix_sensor_struct *sensor;
    int minor_number, sensor_number, type;
    int ret;

    debug("entering\n");
    ret = -ENODEV;
    if ((ret = nonseekable_open(inode, filp)) < 0)
        goto out;

    /*
     * Associate this open file with the relevant sensor based on
     * the minor number of the device node [/dev/sensor<NO>-<TYPE>]
     */
    minor_number = iminor(inode);
    sensor_number = minor_number >> 3;
    type = minor_number & 0x7;

    sensor = &lunix_sensors[sensor_number];

    /* Allocate a new Lunix character device private state structure */
    /* ? */
    state = kmalloc(sizeof(*state), GFP_KERNEL);

    if (state == NULL) {
        ret = -ENOMEM;       // if we run out of memory, basically if malloc fails
        goto out;
    }

    state->type          = type;
    state->sensor        = sensor;
    state->buf_lim       = 0;
    state->buf_timestamp = 0;
    sema_init(&state->lock, 1);

    filp->private_data = state;

    ret = 0;

out:
    debug("leaving, with ret = %d\n", ret);
    return ret;
}

static int lunix_chrdev_release(struct inode *inode, struct file *filp)
{
    struct lunix_chrdev_state_struct *state;
    state = filp->private_data;

    if (state != NULL) {
        kfree(state);
    }

    return 0;
}

static long lunix_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
        return -EINVAL;
}

static ssize_t lunix_chrdev_read(struct file *filp, char __user *usrbuf, size_t cnt, loff_t *f_pos)
{
        ssize_t ret;

        struct lunix_sensor_struct *sensor;
        struct lunix_chrdev_state_struct *state;

        state = filp->private_data;
        WARN_ON(!state);

        sensor = state->sensor;
        WARN_ON(!sensor);

        if (down_interruptible(&state->lock))
         return -ERESTARTSYS;
        /*
         * If the cached character device state needs to be
         * updated by actual sensor data (i.e. we need to report
         * on a "fresh" measurement, do so
         */
        if (*f_pos == 0) {
                while (lunix_chrdev_state_update(state) == -EAGAIN) {
                        /* ? */
                        /* The process needs to sleep */
                        /* See LDD3, page 153 for a hint */
                          /* Άφησε το lock πριν κοιμηθείς */
                        up(&state->lock);

                        /* Περίμενε μέχρι να υπάρξουν νέα δεδομένα */
                        ret = wait_event_interruptible(sensor->wq,
                                        lunix_chrdev_state_needs_refresh(state));
                        if (ret)   /* signal, κλπ */
                                return ret;

                        /* Ξαναπάρε το lock */
                        if (down_interruptible(&state->lock))
                               return -ERESTARTSYS;
                }
        }

        /* End of file */
         if (*f_pos >= state->buf_lim) {

                *f_pos = 0;
                ret = 0;
                goto out;
        }

        /* Determine the number of cached bytes to copy to userspace */
             if (cnt > state->buf_lim - *f_pos )
                cnt = state->buf_lim - *f_pos;

        if (copy_to_user(usrbuf, state->buf_data + *f_pos, cnt)) {
                ret = -EFAULT;
                goto out;
        }

        *f_pos += cnt;
        ret = cnt;

        /* Auto-rewind on EOF mode? */


        if (*f_pos >= state->buf_lim)
                *f_pos = 0;
        // σημαντικό για να έχω συνεχόμενες μετρήσεις
out:
        /* Unlock */
        up(&state->lock);
        return ret;
}

static int lunix_chrdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
        return -EINVAL;
}

static struct file_operations lunix_chrdev_fops =
{
        .owner          = THIS_MODULE,
        .open           = lunix_chrdev_open,
        .release        = lunix_chrdev_release,
        .read           = lunix_chrdev_read,
        .unlocked_ioctl = lunix_chrdev_ioctl,
        .mmap           = lunix_chrdev_mmap
};

int lunix_chrdev_init(void)
{
    /* Register the character device with the kernel, asking for
     * a range of minor numbers (number of sensors * 8 measurements / sensor)
     * beginning with LINUX_CHRDEV_MAJOR:0
     */
    int ret;
    dev_t dev_no;
    unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;

    debug("initializing character device\n");
    cdev_init(&lunix_chrdev_cdev, &lunix_chrdev_fops);
    lunix_chrdev_cdev.owner = THIS_MODULE;                // i added this
//     lunix_chrdev_cdev.ops = &lunix_chrdev_fops;

    dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
    /* register_chrdev_region? */
    ret = register_chrdev_region(dev_no, lunix_minor_cnt, "character_devices");
    /* and then this
     * Since this code is a stub, exit early */
    /* return 0; */

    if (ret < 0) {
        debug("failed to register region, ret = %d\n", ret);
        goto out;
    }

    /* cdev_add? */
    ret = cdev_add(&lunix_chrdev_cdev, dev_no, lunix_minor_cnt);  //this at last
    if (ret < 0) {
        debug("failed to add character device\n");
        goto out_with_chrdev_region;
    }

    debug("Completed successfully\n");
    return 0;

out_with_chrdev_region:
    unregister_chrdev_region(dev_no, lunix_minor_cnt);
out:
    return ret;
}
void lunix_chrdev_destroy(void)
{
        dev_t dev_no;
        unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;

        debug("entering\n");
        dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
        cdev_del(&lunix_chrdev_cdev);
        unregister_chrdev_region(dev_no, lunix_minor_cnt);
        debug("leaving\n");
}
