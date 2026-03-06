#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

SYSCALL_DEFINE2(revstr, char __user *, str, size_t, n)
{
    char *kbuf, *revbuf;
    size_t i;

    kbuf = kmalloc(n + 1, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    revbuf = kmalloc(n + 1, GFP_KERNEL);
    if (!revbuf)
    {
        kfree(kbuf);
        return -ENOMEM;
    }

    if (copy_from_user(kbuf, str, n))
    {
        kfree(kbuf);
        kfree(revbuf);
        return -EFAULT;
    }

    kbuf[n] = '\0';

    pr_info("The origin string = %s\n", kbuf);

    for (i = 0; i < n; i++)
        revbuf[i] = kbuf[n - i - 1];
    revbuf[n] = '\0';

    pr_info("The reversed string = %s\n", revbuf);

    if (copy_to_user(str, revbuf, n + 1))
    {
        kfree(kbuf);
        kfree(revbuf);
        return -EFAULT;
    }

    kfree(kbuf);
    kfree(revbuf);
    return 0;
}
