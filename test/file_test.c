#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

// 文件写
int main(int argc, char **argv)
{
	int count = 1000;
	int64_t sum = 0;
	char file_name[128];
	struct timeval tv_start;
	struct timeval tv_end;
	int64_t time_used;
	int len = 5*1024;
	if(argc >= 2) {
		len = atoi(argv[1]);
	}
  	printf("file len:%d\n", len);
	gettimeofday(&tv_start, NULL);
	int temp_remain = 0;
	for(int i = 0; i < count ; i++)
	{
		sprintf(file_name, "%s.%d.txt", "write", i);
		FILE *file = fopen(file_name, "wb");
		temp_remain = len;
		while(temp_remain > 0) {
			int ret = fwrite(file_name, 1,temp_remain, file);
			if(ret <= 0)
			{
				printf("ret:%d, reamin:%d\n", ret, temp_remain);
				break;
			}
			temp_remain -= ret;
		}
		sum += len;
		fclose(file);
	}
	gettimeofday(&tv_end, NULL);
 	time_used = TIME_SUB_MS(tv_end, tv_start);	
	float m_per_s = sum * 1.0 /1024/1024/(time_used/1000.0);
	printf("write %ld bytes need time:%ldms,  %0.2fM/s\n", sum, time_used, m_per_s);
	return 0;
}


