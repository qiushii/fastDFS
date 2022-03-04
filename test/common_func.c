#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "common_func.h"

int my_daemon_init()
{
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) == NULL)	// 当前的工作目录绝对路径复制到参数buf 所指的内存空间，参数size 为buf 的空间大小
    {
        logError("file: "__FILE__", line: %d, "
                "getcwd fail, errno: %d, error info: %s",
                __LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EPERM;
    }
#ifndef WIN32
	daemon_init(false);		//  后台运行初始化
#endif

	if (chdir(cwd) != 0)
	{
        logError("file: "__FILE__", line: %d, "
                "chdir to %s fail, errno: %d, error info: %s",
                __LINE__, cwd, errno, STRERROR(errno));
		return errno != 0 ? errno : EPERM;
	}

    return 0;
}
