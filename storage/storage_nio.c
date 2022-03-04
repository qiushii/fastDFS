/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.fastken.com/ for more detail.
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/fast_task_queue.h"
#include "tracker_types.h"
#include "tracker_proto.h"
#include "storage_global.h"
#include "storage_service.h"
#include "fastcommon/ioevent_loop.h"
#include "storage_dio.h"
#include "storage_nio.h"

static void client_sock_read(int sock, short event, void *arg);
static void client_sock_write(int sock, short event, void *arg);
static int storage_nio_init(struct fast_task_info *pTask);
//对于超时的处理:删除文件列表,释放任务到队列里面
void task_finish_clean_up(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	if (pClientInfo->clean_func != NULL)
	{
		pClientInfo->clean_func(pTask);
	}

	ioevent_detach(&pTask->thread_data->ev_puller, pTask->event.fd);
	close(pTask->event.fd);
	pTask->event.fd = -1;

	if (pTask->event.timer.expires > 0)
	{
		fast_timer_remove(&pTask->thread_data->timer,
			&pTask->event.timer);
		pTask->event.timer.expires = 0;
	}

    pTask->canceled = false;
	memset(pTask->arg, 0, sizeof(StorageClientInfo));
	free_queue_push(pTask);

    __sync_fetch_and_sub(&g_storage_stat.connection.current_count, 1);
    ++g_stat_change_count;
}

static int set_recv_event(struct fast_task_info *pTask)
{
	int result;

	if (pTask->event.callback == client_sock_read)
	{
		return 0;
	}

	pTask->event.callback = client_sock_read;
	if (ioevent_modify(&pTask->thread_data->ev_puller,
		pTask->event.fd, IOEVENT_READ, pTask) != 0)
	{
		result = errno != 0 ? errno : ENOENT;
		ioevent_add_to_deleted_list(pTask);

		logError("file: "__FILE__", line: %d, "\
			"ioevent_modify fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
		return result;
	}
	return 0;
}

static int set_send_event(struct fast_task_info *pTask)
{
	int result;

	if (pTask->event.callback == client_sock_write)
	{
		return 0;
	}

	pTask->event.callback = client_sock_write;
	if (ioevent_modify(&pTask->thread_data->ev_puller,
		pTask->event.fd, IOEVENT_WRITE, pTask) != 0)
	{
		result = errno != 0 ? errno : ENOENT;
		ioevent_add_to_deleted_list(pTask);

		logError("file: "__FILE__", line: %d, "\
			"ioevent_modify fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
		return result;
	}
	return 0;
}
// 这里的socket实际是pipe
void storage_recv_notify_read(int sock, short event, void *arg)// 数据服务器socket事件回调,比如说在上传文件时,接收了一部分之后,调用storage_nio_notify(pTask)
{
	struct fast_task_info *pTask;
	StorageClientInfo *pClientInfo;//注意这个参数是不同的,一个是跟踪服务器参数,一个是数据服务器参数
	long task_addr;	// 读取task的地址
	int64_t remain_bytes;
	int bytes;
	int result;

	while (1)		 			// 循环读取task任务
	{
		if ((bytes=read(sock, &task_addr, sizeof(task_addr))) < 0) 		// 读取task任务
		{
			if (!(errno == EAGAIN || errno == EWOULDBLOCK))
			{
				logError("file: "__FILE__", line: %d, " \
					"call read failed, " \
					"errno: %d, error info: %s", \
					__LINE__, errno, STRERROR(errno));
			}

			break;				// 没有task可读
		}
		else if (bytes == 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"call read failed, end of file", __LINE__);
			break;
		}

		pTask = (struct fast_task_info *)task_addr;			// 还原任务
		pClientInfo = (StorageClientInfo *)pTask->arg;

		if (pTask->event.fd < 0)  //quit flag, 这个是对应的 socket fd
		{
			return;
		}

		/* //logInfo("=====thread index: %d, pTask->event.fd=%d", \
			pClientInfo->nio_thread_index, pTask->event.fd);
		*/

		if (pClientInfo->stage & FDFS_STORAGE_STAGE_DIO_THREAD)
		{
			pClientInfo->stage &= ~FDFS_STORAGE_STAGE_DIO_THREAD;
		}
		switch (pClientInfo->stage)
		{
			case FDFS_STORAGE_STAGE_NIO_INIT: //数据服务器服务端socket接收过来的任务的pClientInfo->stage=FDFS_STORAGE_STAGE_NIO_INIT
				result = storage_nio_init(pTask);	 //因此在这里在重新绑定读写事件 //每连接一个客户端,在这里都会触发这个动作
				break;
			case FDFS_STORAGE_STAGE_NIO_RECV:
				pTask->offset = 0;				//在次接受包体时pTask->offset偏移量被重置
				remain_bytes = pClientInfo->total_length - \	//任务的长度=包的总长度-包的总偏移量
					       pClientInfo->total_offset;
				if (remain_bytes > pTask->size)	//总是试图将余下的自己一次接收收完
				{
					pTask->length = pTask->size;		// pTask->size 是每次最大的数据接收长度
				}
				else
				{
					pTask->length = remain_bytes;
				}

				if (set_recv_event(pTask) == 0)
				{
					client_sock_read(pTask->event.fd,		// 通过socket fd读取数据
						IOEVENT_READ, pTask);		// 读取数据
				}
				result = 0;
				break;
			case FDFS_STORAGE_STAGE_NIO_SEND:
				result = storage_send_add_event(pTask);	// 数据发送
				break;
			case FDFS_STORAGE_STAGE_NIO_CLOSE:
				result = EIO;   //close this socket
				break;
			default:
				logError("file: "__FILE__", line: %d, " \
					"invalid stage: %d", __LINE__, \
					pClientInfo->stage);
				result = EINVAL;
				break;
		}

		if (result != 0)
		{
			ioevent_add_to_deleted_list(pTask);		// 如果出错再将对应的task加入到删除队列进行处理
		}
	}
}

static int storage_nio_init(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	struct storage_nio_thread_data *pThreadData;

	pClientInfo = (StorageClientInfo *)pTask->arg;	//取工作线程:依据pClientInfo->nio_thread_index 参数
	pThreadData = g_nio_thread_data + pClientInfo->nio_thread_index;	  //在重新绑定socket读写事件,绑定到Task上面

	pClientInfo->stage = FDFS_STORAGE_STAGE_NIO_RECV;
	return ioevent_set(pTask, &pThreadData->thread_data,			// 设置可读触发
			pTask->event.fd, IOEVENT_READ, client_sock_read,
			g_fdfs_network_timeout);
}

int storage_send_add_event(struct fast_task_info *pTask)
{
	pTask->offset = 0;	 //发送是先重置pTask->offset为

	/* direct send */
	client_sock_write(pTask->event.fd, IOEVENT_WRITE, pTask);

	return 0;
}

static void client_sock_read(int sock, short event, void *arg)
{
	int bytes;
	int recv_bytes;
	struct fast_task_info *pTask;
        StorageClientInfo *pClientInfo;

	pTask = (struct fast_task_info *)arg;
    pClientInfo = (StorageClientInfo *)pTask->arg;
	if (pTask->canceled)
	{
		return;
	}

	if (pClientInfo->stage != FDFS_STORAGE_STAGE_NIO_RECV)
	{
		if (event & IOEVENT_TIMEOUT) {
			pTask->event.timer.expires = g_current_time +
				g_fdfs_network_timeout;
			fast_timer_add(&pTask->thread_data->timer,
				&pTask->event.timer);
		}

		return;
	}

	if (event & IOEVENT_TIMEOUT)
	{
		if (pClientInfo->total_offset == 0)
		{
            if (pTask->req_count > 0)
            {
                pTask->event.timer.expires = g_current_time +
                    g_fdfs_network_timeout;
                fast_timer_add(&pTask->thread_data->timer,
                        &pTask->event.timer);
            }
            else
            {
                logWarning("file: "__FILE__", line: %d, "
                        "client ip: %s, recv timeout. "
                        "after the connection is established, "
                        "you must send a request before %ds timeout, "
                        "maybe connections leak in you application.",
                        __LINE__, pTask->client_ip, g_fdfs_network_timeout);
                task_finish_clean_up(pTask);
            }
		}
		else
        {
            logError("file: "__FILE__", line: %d, "
                    "client ip: %s, recv timeout, "
                    "recv offset: %d, expect length: %d, "
                    "req_count: %"PRId64, __LINE__, pTask->client_ip,
                    pTask->offset, pTask->length, pTask->req_count);
            task_finish_clean_up(pTask);
        }

		return;
	}

	if (event & IOEVENT_ERROR)
	{
		logDebug("file: "__FILE__", line: %d, " \
			"client ip: %s, recv error event: %d, "
			"close connection", __LINE__, pTask->client_ip, event);

		task_finish_clean_up(pTask);
		return;
	}

	fast_timer_modify(&pTask->thread_data->timer,
		&pTask->event.timer, g_current_time +
		g_fdfs_network_timeout);
	while (1)
	{
		if (pClientInfo->total_length == 0) //recv header	//初始时pClientInfo->total_length=0 pTask->offset=0
		{
			recv_bytes = sizeof(TrackerHeader) - pTask->offset;
		}
		else  // 至少读到了10个字节后 sizeof(TrackerHeader)
		{
			recv_bytes = pTask->length - pTask->offset;	//在次接受上传文件的数据包时,因为发生storage_nio_notify(pTask)
		}

		/*
		logInfo("total_length=%"PRId64", recv_bytes=%d, "
			"pTask->length=%d, pTask->offset=%d",
			pClientInfo->total_length, recv_bytes, 
			pTask->length, pTask->offset);
		*/

		bytes = recv(sock, pTask->data + pTask->offset, recv_bytes, 0);		// 根据buffer情况读取数据
		if (bytes < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
			}
			else if (errno == EINTR)
			{
				continue;
			}
			else
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, recv failed, " \
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					errno, STRERROR(errno));

				task_finish_clean_up(pTask);
			}

			return;
		}
		else if (bytes == 0)
		{
			logDebug("file: "__FILE__", line: %d, " \
				"client ip: %s, recv failed, " \
				"connection disconnected.", \
				__LINE__, pTask->client_ip);

			task_finish_clean_up(pTask);
			return;
		}

		if (pClientInfo->total_length == 0) //header
		{  // 要来解析header
			if (pTask->offset + bytes < sizeof(TrackerHeader)) // 还没有读够 header
			{
				pTask->offset += bytes;		
				return;
			}

			pClientInfo->total_length=buff2long(((TrackerHeader *) \	//确定包data的总长度:比如下载文件时,接收的包,就只有包的长度， 这里不包括header
						pTask->data)->pkg_len);
			if (pClientInfo->total_length < 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, pkg length: " \
					"%"PRId64" < 0", \
					__LINE__, pTask->client_ip, \
					pClientInfo->total_length);

				task_finish_clean_up(pTask);
				return;
			}
			 //包的总长度=包头+包体的长度  //设想发送的场景:包头+包体+包体+...(其中在包头里面含有多个包体的总长度)
			pClientInfo->total_length += sizeof(TrackerHeader);		//因为默认的接收缓冲只有K,所以会分次发送， 计算出来包括header的长度
			if (pClientInfo->total_length > pTask->size)
			{
				pTask->length = pTask->size;	//如果包的总长大于包的分配的长度,那么任务长度等于任务分配的长度， 读到对应的数据就去触发dio
			}
			else
			{
				pTask->length = pClientInfo->total_length;	//确定任务的长度
			}
		}

		pTask->offset += bytes;  // offset增加
		if (pTask->offset >= pTask->length) //recv current pkg done  //接收到当前包完成
		{
			if (pClientInfo->total_offset + pTask->length >= \  //上次操作接收的总的偏移量+这次接收的数据长度,如果大于包的总长度,那么说明包接收完毕
					pClientInfo->total_length)
			{
				/* current req recv done */
				pClientInfo->stage = FDFS_STORAGE_STAGE_NIO_SEND;
				pTask->req_count++;
			}

			if (pClientInfo->total_offset == 0)
			{ // 说明还没有开始处理
				pClientInfo->total_offset = pTask->length;	 //数据服务器进行处理
				storage_deal_task(pTask); // 解析header 以及我们协议附加的信息
			}
			else
			{
				pClientInfo->total_offset += pTask->length;	 //否则继续写文件

				/* continue write to file */
				storage_dio_queue_push(pTask); // 比如文件增加
			}

			return;
		}
	}

	return;
}

static void client_sock_write(int sock, short event, void *arg)
{
	int bytes;
	struct fast_task_info *pTask;
        StorageClientInfo *pClientInfo;

	pTask = (struct fast_task_info *)arg;
        pClientInfo = (StorageClientInfo *)pTask->arg;
	if (pTask->canceled)
	{
		return;
	}

	if (event & IOEVENT_TIMEOUT)
	{
		logError("file: "__FILE__", line: %d, "
			"client ip: %s, send timeout, offset: %d, "
            "remain bytes: %d", __LINE__, pTask->client_ip,
            pTask->offset, pTask->length - pTask->offset);

		task_finish_clean_up(pTask);
		return;
	}

	if (event & IOEVENT_ERROR)
	{
		logDebug("file: "__FILE__", line: %d, "
			"client ip: %s, recv error event: %d, "
			"close connection", __LINE__, pTask->client_ip, event);

		task_finish_clean_up(pTask);
		return;
	}

	while (1)
	{
		fast_timer_modify(&pTask->thread_data->timer,
			&pTask->event.timer, g_current_time +
			g_fdfs_network_timeout);
		bytes = send(sock, pTask->data + pTask->offset, \
				pTask->length - pTask->offset,  0);
		//printf("%08X sended %d bytes\n", (int)pTask, bytes);
		if (bytes < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				set_send_event(pTask);
			}
			else if (errno == EINTR)
			{
				continue;
			}
			else
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, recv failed, " \
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					errno, STRERROR(errno));

				task_finish_clean_up(pTask);
			}

			return;
		}
		else if (bytes == 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"send failed, connection disconnected.", \
				__LINE__);

			task_finish_clean_up(pTask);
			return;
		}

		pTask->offset += bytes;
		if (pTask->offset >= pTask->length)
		{
			if (set_recv_event(pTask) != 0)
			{
				return;
			}

			pClientInfo->total_offset += pTask->length;
			if (pClientInfo->total_offset>=pClientInfo->total_length)
			{
				if (pClientInfo->total_length == sizeof(TrackerHeader)
					&& ((TrackerHeader *)pTask->data)->status == EINVAL)
				{
					logDebug("file: "__FILE__", line: %d, "\
						"close conn: #%d, client ip: %s", \
						__LINE__, pTask->event.fd,
						pTask->client_ip);
					task_finish_clean_up(pTask);
					return;
				}

				/*  response done, try to recv again */
				pClientInfo->total_length = 0;
				pClientInfo->total_offset = 0;
				pTask->offset = 0;
				pTask->length = 0;

				pClientInfo->stage = FDFS_STORAGE_STAGE_NIO_RECV;
			}
			else  //continue to send file content
			{
				pTask->length = 0;

				/* continue read from file */
				storage_dio_queue_push(pTask);		// 继续发送数据
			}

			return;
		}
	}
}

