#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>

enum mode
{
    PRINT = 0,
    ADD = 1,
    REMOVE = 2
};

struct temp_node
{
    struct list_head link;
    char *s;
    size_t len;
};

static LIST_HEAD(temp_list);

static DEFINE_SPINLOCK(temp_lock);

static int do_add_from_user(const void __user *data, size_t size)
{
    struct temp_node *node;
    char *buf;

    if (!data || size == 0)
        return -EFAULT;

    buf = kmalloc(size + 1, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    if (copy_from_user(buf, data, size))
    {
        kfree(buf);
        return -EFAULT;
    }
    buf[size] = '\0';

    node = kmalloc(sizeof(*node), GFP_KERNEL);
    if (!node)
    {
        kfree(buf);
        return -ENOMEM;
    }
    node->s = buf;
    node->len = size;

    spin_lock(&temp_lock);
    list_add_tail(&node->link, &temp_list);
    spin_unlock(&temp_lock);

    pr_info("[tempbuf] Added: %s\n", buf);
    return 0;
}

static int do_remove_match_user(const void __user *data, size_t size)
{
    struct temp_node *pos, *n;
    char *key;

    if (!data || size == 0)
        return -EFAULT;

    key = kmalloc(size, GFP_KERNEL);
    if (!key)
        return -ENOMEM;
    if (copy_from_user(key, data, size))
    {
        kfree(key);
        return -EFAULT;
    }

    spin_lock(&temp_lock);
    list_for_each_entry_safe(pos, n, &temp_list, link)
    {

        if (pos->len == size && !memcmp(pos->s, key, size))
        {
            list_del(&pos->link);
            spin_unlock(&temp_lock);

            pr_info("[tempbuf] Removed: %.*s\n", (int)size, pos->s);

            kfree(pos->s);
            kfree(pos);
            kfree(key);
            return 0;
        }
    }
    spin_unlock(&temp_lock);

    kfree(key);
    return -ENOENT;
}

static ssize_t do_print_to_user(void __user *ubuf, size_t usize)
{
    ssize_t written = 0;
    size_t need_space = 0;
    struct temp_node *pos;
    char *kbuf, *p;

    if (!ubuf || usize == 0)
        return -EFAULT;

    if (usize > 512)
        usize = 512;

    kbuf = kzalloc(usize, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    p = kbuf;

    spin_lock(&temp_lock);
    list_for_each_entry(pos, &temp_list, link)
    {
        size_t add = pos->len + (need_space ? 1 : 0);
        if (written + add >= usize)
            break;

        if (need_space)
        {
            *p++ = ' ';
            written += 1;
        }

        memcpy(p, pos->s, pos->len);
        p += pos->len;
        written += pos->len;

        need_space = 1;
    }
    spin_unlock(&temp_lock);

    pr_info("[tempbuf] %s\n", kbuf);

    if (copy_to_user(ubuf, kbuf, written))
    {
        kfree(kbuf);
        return -EFAULT;
    }

    kfree(kbuf);
    return written;
}

SYSCALL_DEFINE3(tempbuf, int, mode, void __user *, data, size_t, size)
{
    switch (mode)
    {
    case ADD:
        return do_add_from_user(data, size);
    case REMOVE:
        return do_remove_match_user(data, size);
    case PRINT:
        return do_print_to_user(data, size);
    default:
        return -EINVAL;
    }
}
