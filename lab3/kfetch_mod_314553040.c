#include <asm/errno.h>
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>      /* printk helpers, etc. */
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/sched.h>       /* for_each_process */
#include <linux/sched/signal.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/uaccess.h>     /* copy_from_user / copy_to_user */
#include <linux/utsname.h>     /* init_uts_ns */
#include <linux/version.h>
#include <linux/sysinfo.h>     /* struct sysinfo */
#include <linux/string.h>      /* strcpy, strcat, memset, etc. */
#include <linux/mutex.h>       /* mutex_lock / mutex_unlock */

#define SUCCESS         0
#define DEVICE_NAME     "kfetch"       /* Device name as it appears in /dev/ */
#define KFETCH_BUF_SIZE 1024           /* Max length of the message from the device */

/* Information bits */
#define KFETCH_NUM_INFO 6
#define KFETCH_RELEASE   (1 << 0)
#define KFETCH_NUM_CPUS  (1 << 1)
#define KFETCH_CPU_MODEL (1 << 2)
#define KFETCH_MEM       (1 << 3)
#define KFETCH_UPTIME    (1 << 4)
#define KFETCH_NUM_PROCS (1 << 5)

/*
 * Global state
 * ------------
 * - kfetch_buf: temporary buffer to hold the final output (logo + info)
 * - kfetch_mask: current information mask (what to print)
 * - major: device major number
 * - cls: device class for /dev/kfetch
 * - kfetch_lock: mutex protecting shared state (mask, buffer, formatting)
 *
 * All accesses that read or modify kfetch_mask or kfetch_buf must hold
 * kfetch_lock to avoid race conditions with concurrent read/write.
 */

static char kfetch_buf[KFETCH_BUF_SIZE];
static int  kfetch_mask = 0;
static int  major;         /* major number assigned to our device driver */
static struct class *cls;

/* Mutex to provide thread-safe access to shared state */
static DEFINE_MUTEX(kfetch_lock);

/* Prototypes */
static int     kfetch_init(void);
static void    kfetch_exit(void);
static ssize_t kfetch_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t kfetch_write(struct file *, const char __user *, size_t, loff_t *);
static int     kfetch_open(struct inode *, struct file *);
static int     kfetch_release(struct inode *, struct file *);

static const struct file_operations kfetch_ops = {
    .owner   = THIS_MODULE,
    .read    = kfetch_read,
    .write   = kfetch_write,
    .open    = kfetch_open,
    .release = kfetch_release,
};

/*
 * kfetch_init - called when the module is loaded
 *  - allocate a character device (major)
 *  - create /dev/kfetch
 *  - initialize default information mask
 */
static int kfetch_init(void)
{
    /* Register a character device with dynamic major number */
    major = register_chrdev(0, DEVICE_NAME, &kfetch_ops);
    if (major < 0) {
        pr_alert("kfetch: registering char device failed with %d\n", major);
        return major;
    }

    pr_info("kfetch: assigned major number %d\n", major);

    /*
     * On this devkit, class_create takes a single const char * argument.
     * This creates a class under /sys/class/DEVICE_NAME.
     */
    cls = class_create(DEVICE_NAME);
    if (IS_ERR(cls)) {
        unregister_chrdev(major, DEVICE_NAME);
        pr_alert("kfetch: class_create failed\n");
        return PTR_ERR(cls);
    }

    /*
     * Create a device node /dev/kfetch with (major, 0).
     * user-space can open it via open("/dev/kfetch", ...).
     */
    if (IS_ERR(device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME))) {
        class_destroy(cls);
        unregister_chrdev(major, DEVICE_NAME);
        pr_alert("kfetch: device_create failed\n");
        return -ENOMEM;
    }

    pr_info("kfetch: device created at /dev/%s\n", DEVICE_NAME);

    /* When the module is first loaded, show all information by default */
    kfetch_mask = ((1 << KFETCH_NUM_INFO) - 1);

    return SUCCESS;
}

/*
 * kfetch_exit - called when the module is unloaded
 *  - destroy /dev/kfetch
 *  - unregister the character device
 */
static void kfetch_exit(void)
{
    device_destroy(cls, MKDEV(major, 0));
    class_destroy(cls);

    unregister_chrdev(major, DEVICE_NAME);

    pr_info("kfetch: unloaded\n");
}

/*
 * kfetch_open - called when a process opens /dev/kfetch
 *
 * We no longer restrict the device to a single open.
 * Concurrent opens are allowed; synchronization is handled via kfetch_lock
 * in read()/write(), which protects shared state (mask and buffer).
 */
static int kfetch_open(struct inode *inode, struct file *file)
{
    /* Optional: bump module use count (keeps module from being unloaded) */
    try_module_get(THIS_MODULE);
    return SUCCESS;
}

/*
 * kfetch_release - called when a process closes /dev/kfetch
 */
static int kfetch_release(struct inode *inode, struct file *file)
{
    /* Drop the module use count */
    module_put(THIS_MODULE);
    return SUCCESS;
}

/*
 * Colored logo to be shown on the left side.
 * Each entry corresponds to one line of the logo.
 */
static char *logo[8] = {
    "                      ",
    "         .-.          ",
    "        (.. |         ",
    "       \033[1;33m <> \033[1;0m |         ",
    "       / --- \\        ",
    "      ( |   | |       ",
    "    \033[1;33m|\\\033[1;0m\\_)___/\\)\033[1;33m/\\ \033[1;0m    ",
    "   \033[1;33m<__)\033[1;0m------\033[1;33m(__/\033[1;0m     ",
};

/*
 * kfetch_read - called when user-space reads from /dev/kfetch
 *
 * Responsible for:
 *  - fetching system information (hostname, release, CPUs, memory, etc.)
 *  - formatting text into kfetch_buf as "logo + info" lines
 *  - copying the final buffer to user-space
 *
 * To ensure atomic output and avoid race conditions:
 *  - we lock kfetch_lock for the entire "fetch + format + copy" sequence
 *  - thus multiple readers/writers cannot interleave and corrupt kfetch_buf
 *
 * Note:
 *  - We ignore *offset and always return the full block once per read.
 *    This behavior is acceptable for this assignment: each read() returns
 *    the full formatted output block.
 */
static ssize_t kfetch_read(struct file *filp,
                           char __user *buffer,
                           size_t length,
                           loff_t *offset)
{
    const char *machine_hostname;
    const char *kernel_release;
    char split_line[64];

    const char *cpu_model_name;
    int online_cpus;
    int total_cpus;

    unsigned long free_memory;
    unsigned long total_memory;

    int  num_procs = 0;
    long uptime;

    char info_list[8][64];
    bool contain_info[8] = { true, true, false, false, false, false, false, false };
    char tmp[64] = {0};

    size_t out_len;
    int i, j, sl;

    /*
     * We ignore *offset and always regenerate the entire block
     * for each read() call, which simplifies the logic and is
     * enough for this homework.
     */

    /* Acquire the lock to protect kfetch_mask and kfetch_buf and
     * to keep the output block atomic for concurrent readers. */
    mutex_lock(&kfetch_lock);

    /* -------------------- Fetching information -------------------- */

    /* Hostname and kernel release from init_uts_ns */
    machine_hostname = init_uts_ns.name.nodename;
    kernel_release   = init_uts_ns.name.release;

    /* Separator line: same length as hostname */
    sl = 0;
    for (; machine_hostname[sl] != '\0' && sl < (int)(sizeof(split_line) - 1); sl++) {
        split_line[sl] = '-';
    }
    split_line[sl] = '\0';

    /*
     * CPU model / number of CPUs
     * For RISC-V we don't have x86-style model names in the same way,
     * so we use a simple placeholder string. The spec allows us to
     * decide a reasonable format.
     */
    cpu_model_name = "RISC-V Processor";
    online_cpus    = num_online_cpus();
    total_cpus     = num_possible_cpus();

    /* Memory info: use struct sysinfo and convert to MB */
    {
        struct sysinfo si;

        si_meminfo(&si);
        total_memory = (si.totalram * si.mem_unit) >> 20; /* bytes -> MB */
        free_memory  = (si.freeram  * si.mem_unit) >> 20;
    }

    /* Number of processes: simply count tasks via for_each_process() */
    {
        struct task_struct *task;

        for_each_process(task)
            num_procs++;
    }

    /* Uptime: boot time in seconds, then convert to minutes */
    {
        u64 uptime_sec = ktime_get_boottime_seconds();
        uptime = (long)(uptime_sec / 60);
    }

    /* -------------------- Building text lines -------------------- */

    /* Line 0: hostname (always present) */
    strcpy(info_list[0], machine_hostname);
    /* Line 1: separator line */
    strcpy(info_list[1], split_line);

    if (kfetch_mask & KFETCH_RELEASE) {
        contain_info[2] = true;
        snprintf(tmp, sizeof(tmp), "\033[1;33mKernel:\033[1;0m %s", kernel_release);
        strcpy(info_list[2], tmp);
    }
    if (kfetch_mask & KFETCH_CPU_MODEL) {
        contain_info[3] = true;
        snprintf(tmp, sizeof(tmp), "\033[1;33mCPU:\033[1;0m    %s", cpu_model_name);
        strcpy(info_list[3], tmp);
    }
    if (kfetch_mask & KFETCH_NUM_CPUS) {
        contain_info[4] = true;
        snprintf(tmp, sizeof(tmp), "\033[1;33mCPUs:\033[1;0m   %d / %d",
                 online_cpus, total_cpus);
        strcpy(info_list[4], tmp);
    }
    if (kfetch_mask & KFETCH_MEM) {
        contain_info[5] = true;
        snprintf(tmp, sizeof(tmp), "\033[1;33mMem:\033[1;0m    %lu / %lu MB",
                 free_memory, total_memory);
        strcpy(info_list[5], tmp);
    }
    if (kfetch_mask & KFETCH_NUM_PROCS) {
        contain_info[6] = true;
        snprintf(tmp, sizeof(tmp), "\033[1;33mProcs:\033[1;0m  %d", num_procs);
        strcpy(info_list[6], tmp);
    }
    if (kfetch_mask & KFETCH_UPTIME) {
        contain_info[7] = true;
        snprintf(tmp, sizeof(tmp), "\033[1;33mUptime:\033[1;0m %ld mins", uptime);
        strcpy(info_list[7], tmp);
    }

    /* -------------------- Combine logo + info into kfetch_buf -------------------- */

    memset(kfetch_buf, 0, sizeof(kfetch_buf));

    j = 0;
    for (i = 0; i < 8; i++) {
        /* Append logo line */
        strcat(kfetch_buf, logo[i]);

        /* Append the corresponding info line if any (logo and info aligned by row) */
        while (j < 8) {
            if (contain_info[j]) {
                strcat(kfetch_buf, info_list[j]);
                j++;
                break;
            }
            j++;
        }

        strcat(kfetch_buf, "\n");
    }

    /* Compute actual output length (do not always send full KFETCH_BUF_SIZE) */
    out_len = strnlen(kfetch_buf, sizeof(kfetch_buf));
    if (length > out_len)
        length = out_len;

    /* Copy the final formatted block to user-space */
    if (copy_to_user(buffer, kfetch_buf, length)) {
        pr_alert("kfetch: failed to copy data to user\n");
        mutex_unlock(&kfetch_lock);
        return -EFAULT;
    }

    mutex_unlock(&kfetch_lock);

    /*
     * We always return 'length' bytes on each read call; since we ignore *offset,
     * a second read will return the same block again. For the assignment, this
     * behavior is acceptable as long as the block itself is not corrupted.
     */
    return length;
}

/*
 * kfetch_write - called when user-space writes to /dev/kfetch
 *
 * User-space passes a single integer (information mask) to the driver.
 * Subsequent read() calls use this mask to decide which lines to print.
 *
 * The access to kfetch_mask is protected by kfetch_lock to avoid races with
 * concurrent readers (kfetch_read) and writers (kfetch_write).
 */
static ssize_t kfetch_write(struct file *filp,
                            const char __user *buffer,
                            size_t length,
                            loff_t *offset)
{
    int mask_info;

    if (length < sizeof(int))
        return -EINVAL;

    /* Copy mask from user-space */
    if (copy_from_user(&mask_info, buffer, sizeof(mask_info))) {
        pr_alert("kfetch: failed to copy data from user\n");
        return -EFAULT;
    }

    /* Update the global information mask atomically under the lock */
    mutex_lock(&kfetch_lock);
    kfetch_mask = mask_info;
    mutex_unlock(&kfetch_lock);

    /* Return the number of bytes consumed */
    return sizeof(mask_info);
}

module_init(kfetch_init);
module_exit(kfetch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("314553040");
MODULE_DESCRIPTION("Fetch system information via /dev/kfetch (RISC-V)");

