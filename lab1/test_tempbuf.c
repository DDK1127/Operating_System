#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#define __NR_tempbuf 452

enum mode {
    PRINT,
    ADD,
    REMOVE
};

int main() {
    char buf[512];
    int ret;

    ret = syscall(__NR_tempbuf, ADD, "Hello", strlen("Hello"));
    assert(ret == 0);

    ret = syscall(__NR_tempbuf, ADD, "Operating Systems", strlen("Operating Systems"));
    assert(ret == 0);

    memset(buf, 0, sizeof(buf));
    ret = syscall(__NR_tempbuf, PRINT, buf, sizeof(buf));
    assert(ret >= 0);
    printf("%s\n", buf);

    ret = syscall(__NR_tempbuf, REMOVE, "NotExist", strlen("NotExist"));
    assert(ret == -1 && errno == ENOENT);

    ret = syscall(__NR_tempbuf, REMOVE, "Hello", strlen("Hello"));
    assert(ret == 0);

    memset(buf, 0, sizeof(buf));
    ret = syscall(__NR_tempbuf, PRINT, buf, sizeof(buf));
    assert(ret >= 0);
    printf("%s\n", buf);

    return 0;
}