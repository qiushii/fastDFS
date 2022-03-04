#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "fastcommon/common_define.h"
#include "fastcommon/logger.h"
#include "test_types.h"
#include "common_func.h"
#include "dfs_func.h"

#define PROCESS_COUNT	10

typedef struct {
	int bytes;  						//file size 测试文件大小
	char *filename;						//测试文件名
	int count;   						//total file count	
	int upload_count;					//上传文件次数			
	int success_count;  				//success upload count成功上传次数
	int fd;   							//file description
	int64_t time_used;  				//unit: ms		耗时
	char *file_buff; 					//file content 文件内容
} TestFileInfo;

#ifdef DEBUG  //for debug   小批量测试看看情况

static TestFileInfo files[FILE_TYPE_COUNT] = {
	{5 * 1024, 			"5K",  	50000 / PROCESS_COUNT, 0, 0, -1, 0, NULL},	
	{50 * 1024, 		"50K", 	10000 / PROCESS_COUNT, 0, 0, -1, 0, NULL}, 
	{200 * 1024, 		"200K", 5000 / PROCESS_COUNT, 0, 0, -1, 0, NULL},
	{1 * 1024 * 1024, 	"1M",   500 / PROCESS_COUNT, 0, 0, -1, 0, NULL},
	{10 * 1024 * 1024, 	"10M",  50 / PROCESS_COUNT, 0, 0, -1, 0, NULL},
	{100 * 1024 * 1024, "100M",	10 / PROCESS_COUNT, 0, 0, -1, 0, NULL}
};

#else                        // 大批量数据测试

static TestFileInfo files[FILE_TYPE_COUNT] = {
	{5 * 1024, 			"5K",         1000000 / PROCESS_COUNT, 0, 0, -1, 0, NULL},
	{50 * 1024, 		"50K",       2000000 / PROCESS_COUNT, 0, 0, -1, 0, NULL}, 
	{200 * 1024, 		"200K",     1000000 / PROCESS_COUNT, 0, 0, -1, 0, NULL},
	{1 * 1024 * 1024, 	"1M",   200000 / PROCESS_COUNT, 0, 0, -1, 0, NULL},
	{10 * 1024 * 1024, 	"10M",  20000 / PROCESS_COUNT, 0, 0, -1, 0, NULL},
	{100 * 1024 * 1024, "100M", 1000 / PROCESS_COUNT, 0, 0, -1, 0, NULL}
};

#endif

static StorageStat storages[MAX_STORAGE_COUNT];		// 支持的storage数量统计
static int storage_count = 0;			// 有多少个storage数量
static time_t start_time;	// 起始时间
static int total_count = 0;	// 总计数
static int success_count = 0;// 成功计数
static FILE *fpSuccess = NULL; // 成功
static FILE *fpFail = NULL;    // 失败

static int proccess_index;
static int load_file_contents();
static int test_init();
static int save_stats_by_overall();
static int save_stats_by_file_type();
static int save_stats_by_storage_ip();
static int add_to_storage_stat(const char *storage_ip, const int result, const int time_used);

int main(int argc, char **argv)
{
	int result;
	int upload_count;
	int rand_num;
	int file_index;
	char *conf_filename;				// 配置文件地址
	char file_id[128];
	char storage_ip[IP_ADDRESS_SIZE];		// ip地址
	int count_sums[FILE_TYPE_COUNT];
	int i;
	struct timeval tv_start;
	struct timeval tv_end;
	int time_used;

	if (argc < 2)
	{
		printf("Usage: %s <process_index> [config_filename]\n", argv[0]);
		return EINVAL;
	}

	log_init();
	proccess_index = atoi(argv[1]);  // 默认最大为10
	if (proccess_index < 0 || proccess_index >= PROCESS_COUNT)
	{
		printf("Invalid proccess index: %d\n", proccess_index);
		return EINVAL;
	}

	if (argc >= 3)
	{
		conf_filename = argv[2];  // 配置文件路径
	}
	else
	{
		conf_filename = "/etc/fdfs/client.conf";
	}

	if ((result = load_file_contents()) != 0)	// 加载测试文件，主要打开文件和做内存映射
	{
		return result;
	}

	if ((result=test_init()) != 0) // 创建upload目录，以及创建成功和失败对应的文件
	{
		return result;
	}

	if ((result=dfs_init(proccess_index, conf_filename)) != 0) // fdfs client初始化
	{
		return result;
	}

    if ((result=my_daemon_init()) != 0)	// 后台运行初始化
	{
		return result;
	}

	memset(&storages, 0, sizeof(storages));
	upload_count = 0;
	for (i=0; i<FILE_TYPE_COUNT; i++)
	{
		upload_count += files[i].count;
		count_sums[i] = upload_count;
	}

	if (upload_count == 0)		// upload_count 总的上传测试次数， count_sums每种文件自己的上传测试次数
	{
		return EINVAL;
	}

	memset(file_id, 0, sizeof(file_id));
	memset(storage_ip, 0, sizeof(storage_ip));

	start_time = time(NULL);			// 起始时间
	srand(SRAND_SEED);
	result = 0;
	total_count = 0;					// 当前测试的总次数
	success_count = 0;					// 当前测试的成功总次数
	while (total_count < upload_count)
	{
		rand_num = (int)(upload_count * ((double)rand() / RAND_MAX));
		for (file_index=0; file_index<FILE_TYPE_COUNT; file_index++)
		{
			if (rand_num < count_sums[file_index])		// 正常
			{
				break;  
			}
		}
		// 随机测试哪一种文件
		if (files[file_index].upload_count >= files[file_index].count)
		{
			continue;
		}

		files[file_index].upload_count++;		// 单项文件上传计数+1
		total_count++;	// 总文件上传计数+1

		gettimeofday(&tv_start, NULL);		// 起始时间
		*storage_ip = '\0';
		// 上传文件
		result = upload_file(files[file_index].file_buff, files[file_index].bytes, file_id, storage_ip);
		gettimeofday(&tv_end, NULL);		// 结束时间
		time_used = TIME_SUB_MS(tv_end, tv_start); 	// 转化为时间间隔 ms下载的时候误差比较大
		files[file_index].time_used += time_used;	// 总共耗时		

		add_to_storage_stat(storage_ip, result, time_used); //stat_by_storage_ip.0
		if (result == 0) //success
		{
			success_count++;
			files[file_index].success_count++;			// 成功

			fprintf(fpSuccess, "%d %d %s %s %d\n", 
				(int)tv_end.tv_sec, files[file_index].bytes, 
				file_id, storage_ip, time_used);
		}
		else //fail
		{
			fprintf(fpFail, "%d %d %d %d\n", (int)tv_end.tv_sec, 
				files[file_index].bytes, result, time_used);		// 失败
			fflush(fpFail);
		}

		if (total_count % 100 == 0)							// 每100条数据刷一次盘
		{
			if ((result=save_stats_by_overall()) != 0)	// stat_by_overall.0
			{
				break;
			}
			if ((result=save_stats_by_file_type()) != 0)	// stat_by_file_type.0
			{
				break;
			}

			if ((result=save_stats_by_storage_ip()) != 0)	// stat_by_file_type.0
			{
				break;
			}
		}

	}

	save_stats_by_overall();
	save_stats_by_file_type();
	save_stats_by_storage_ip();

	fclose(fpSuccess);
	fclose(fpFail);

	dfs_destroy();

	printf("proccess %d, time used: %ds\n", proccess_index, (int)(time(NULL) - start_time));
	return result;
}

static int save_stats_by_file_type()
{
	int k;
	char filename[64];
	FILE *fp;

	sprintf(filename, "%s.%d", STAT_FILENAME_BY_FILE_TYPE, proccess_index);
	if ((fp=fopen(filename, "wb")) == NULL)
	{
		printf("open file %s fail, errno: %d, error info: %s\n", 
			filename, errno, STRERROR(errno));
		return errno != 0 ? errno : EPERM;
	}

	fprintf(fp, "#file_type total_count success_count time_used(ms)\n");
	for (k=0; k<FILE_TYPE_COUNT; k++)
	{
		fprintf(fp, "%s %d %d %"PRId64"\n", \
			files[k].filename, files[k].upload_count, \
			files[k].success_count, files[k].time_used);
	}

	fclose(fp);
	return 0;
}

static int save_stats_by_storage_ip()
{
	int k;
	char filename[64];
	FILE *fp;

	sprintf(filename, "%s.%d", STAT_FILENAME_BY_STORAGE_IP, proccess_index);
	if ((fp=fopen(filename, "wb")) == NULL)
	{
		printf("open file %s fail, errno: %d, error info: %s\n", 
			filename, errno, STRERROR(errno));
		return errno != 0 ? errno : EPERM;
	}

	fprintf(fp, "#ip_addr total_count success_count time_used(ms)\n");
	for (k=0; k<storage_count; k++)
	{
		fprintf(fp, "%s %d %d %"PRId64"\n", \
			storages[k].ip_addr, storages[k].total_count, \
			storages[k].success_count, storages[k].time_used);
	}

	fclose(fp);
	return 0;
}

static int save_stats_by_overall()
{
	char filename[64];
	FILE *fp;

	sprintf(filename, "%s.%d", STAT_FILENAME_BY_OVERALL, proccess_index);
	if ((fp=fopen(filename, "wb")) == NULL)
	{
		printf("open file %s fail, errno: %d, error info: %s\n", 
			filename, errno, STRERROR(errno));
		return errno != 0 ? errno : EPERM;
	}

	fprintf(fp, "#total_count success_count  time_used(s)\n");
	fprintf(fp, "%d %d %d\n", total_count, success_count, (int)(time(NULL) - start_time));

	fclose(fp);
	return 0;
}

static int add_to_storage_stat(const char *storage_ip, const int result, const int time_used)
{
	StorageStat *pStorage;
	StorageStat *pEnd;

	pEnd = storages + storage_count;
	for (pStorage=storages; pStorage<pEnd; pStorage++)
	{
		if (strcmp(storage_ip, pStorage->ip_addr) == 0)
		{
			break;		// 统计给对应的storage
		}
	}

	if (pStorage == pEnd) //not found
	{
		if (storage_count >= MAX_STORAGE_COUNT)
		{
			printf("storage_count %d >= %d\n", storage_count, MAX_STORAGE_COUNT);
			return ENOSPC;
		}

		strcpy(pStorage->ip_addr, storage_ip);
		storage_count++;
	}

	pStorage->time_used += time_used;		
	pStorage->total_count++;			// 总的++
	if (result == 0)
	{
		pStorage->success_count++;		// 成功++
	}

	return 0;
}

static int load_file_contents()
{
	int i;
	//int result;
	int64_t file_size;

	for (i=0; i<FILE_TYPE_COUNT; i++)
	{
		files[i].fd = open(files[i].filename, O_RDONLY);
		if (files[i].fd < 0)
		{
			fprintf(stderr, "file: "__FILE__", line: %d, " \
				"open file %s fail, " \
				"errno: %d, error info: %s\n", __LINE__, \
				files[i].filename, errno, STRERROR(errno));
			return errno != 0 ? errno : ENOENT;
		}

		if ((file_size=lseek(files[i].fd, 0, SEEK_END)) < 0)
		{
			fprintf(stderr, "file: "__FILE__", line: %d, " \
				"lseek file %s fail, " \
				"errno: %d, error info: %s\n", __LINE__, \
				files[i].filename, errno, STRERROR(errno));
			return errno != 0 ? errno : EIO;
		}

		if (file_size != files[i].bytes)
		{
			fprintf(stderr, "file: "__FILE__", line: %d, " 
				"%s file size: %d != %d\n", __LINE__, 
				files[i].filename, (int)file_size, \
				files[i].bytes);

			return EINVAL;
		}

		files[i].file_buff = mmap(NULL, file_size, PROT_READ, \
					MAP_SHARED, files[i].fd, 0);
		if (files[i].file_buff == NULL)
		{
			fprintf(stderr, "file: "__FILE__", line: %d, " \
				"mmap file %s fail, " \
				"errno: %d, error info: %s\n", \
				__LINE__, files[i].filename, \
				errno, STRERROR(errno));
			return errno != 0 ? errno : ENOENT;
		}
	}

	return 0;
}

static int test_init()
{
	char filename[64];

	if (access("upload", 0) != 0 && mkdir("upload", 0755) != 0)
	{
	}

	if (chdir("upload") != 0)
	{
		printf("chdir fail, errno: %d, error info: %s\n", errno, STRERROR(errno));
		return errno != 0 ? errno : EPERM;
	}

	sprintf(filename, "%s.%d", FILENAME_FILE_ID, proccess_index);
	if ((fpSuccess=fopen(filename, "wb")) == NULL)  // 成功的文件  file_id.0
	{
		printf("open file %s fail, errno: %d, error info: %s\n", 
			filename, errno, STRERROR(errno));
		return errno != 0 ? errno : EPERM;
	}

	sprintf(filename, "%s.%d", FILENAME_FAIL, proccess_index);
	if ((fpFail=fopen(filename, "wb")) == NULL)		// 失败的文件 fail.0
	{
		printf("open file %s fail, errno: %d, error info: %s\n", 
			filename, errno, STRERROR(errno));
		return errno != 0 ? errno : EPERM;
	}

	return 0;
}

