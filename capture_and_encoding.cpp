#include <string.h>    
#include <errno.h>      

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/uio.h>

#include <ini.h>

#include <imp/imp_isp.h>
#include <imp/imp_system.h>
#include <imp/imp_log.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
#include <imp/imp_osd.h>

#include "imp-common.h" 
#include "capture_and_encoding.h"

#ifdef SUPPORT_RGB555LE
#include "bgramapinfo_rgb555le.h"
#else
#include "bgramapinfo.h"
#endif

#define TAG "capture_and_encoding"

extern struct chn_conf chn[];

extern "C" {
extern int IMP_OSD_SetPoolSize(int newPoolSize);
extern int IMP_Encoder_SetPoolSize(int newPoolSize0);
}

//#define ENABLED_OSD 1
//#define NIGHTMODE_SWITCH 1

int grpNum = 0;
IMPRgnHandle *prHander;
static volatile int g_osd_active = 0;

int destory()
{
	int ret, i;

	printf("[capture] Teardown starting...\n");

	/* Signal OSD update thread to stop before tearing down handles */
	g_osd_active = 0;
	usleep(200000); /* give thread one iteration to notice and exit */

	/* Step.a Stop receiving pictures before teardown */
	ret = IMP_Encoder_StopRecvPic(0);
	if (ret < 0) {
		printf("[capture] IMP_Encoder_StopRecvPic() failed: %d\n", ret);
		return -1;
	}

	/* Step.b Stream Off */
	ret = sample_framesource_streamoff();
	if (ret < 0) {
		printf("[capture] FrameSource StreamOff failed: %d\n", ret);
		return -1;
	}

	/* Step.c UnBind */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_UnBind(&chn[i].framesource_chn, &chn[i].imp_encoder);
			if (ret < 0) {
				printf("[capture] UnBind channel%d failed: %d\n", i, ret);
				return -1;
			}
		}
	}

	/* Step.d Encoder exit */
	ret = sample_encoder_exit();
	if (ret < 0) {
		printf("[capture] Encoder exit failed: %d\n", ret);
		return -1;
	}

	/* Step.e FrameSource exit */
	ret = sample_framesource_exit();
	if (ret < 0) {
		printf("[capture] FrameSource exit failed: %d\n", ret);
		return -1;
	}

	/* Step.f System exit */
	ret = sample_system_exit();
	if (ret < 0) {
		printf("[capture] sample_system_exit() failed: %d\n", ret);
		return -1;
	}

	printf("[capture] Teardown complete\n");
	return 0;
}

#define MAX_IOV_PACKS 32

int save_stream_to_fd(int fd, IMPEncoderStream *stream)
{
	int i, nr_pack = stream->packCount;
	struct iovec iov[MAX_IOV_PACKS];
	size_t total = 0;

	if (nr_pack > MAX_IOV_PACKS)
		nr_pack = MAX_IOV_PACKS;

	for (i = 0; i < nr_pack; i++) {
		iov[i].iov_base = (void *)stream->pack[i].virAddr;
		iov[i].iov_len = stream->pack[i].length;
		total += stream->pack[i].length;
	}

	/* Write with EINTR retry and partial-write handling */
	size_t total_written = 0;
	i = 0;
	while (total_written < total) {
		ssize_t written = writev(fd, iov + i, nr_pack - i);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			printf("stream writev error:%s\n", strerror(errno));
			return -1;
		}
		total_written += written;
		/* Advance iov past fully-written entries */
		while (i < nr_pack && (size_t)written >= iov[i].iov_len) {
			written -= iov[i].iov_len;
			i++;
		}
		if (i < nr_pack) {
			iov[i].iov_base = (char *)iov[i].iov_base + written;
			iov[i].iov_len -= written;
		}
	}

	return 0;
}

int start_encoder_receiving(int chn)
{
	int ret = IMP_Encoder_StartRecvPic(chn);
	if (ret < 0) {
		printf("IMP_Encoder_StartRecvPic(%d) failed\n", chn);
		return -1;
	}
	return 0;
}


// THE OSD PARTS STARTS HERE

static int osd_show(void)
{
        int ret;

        ret = IMP_OSD_ShowRgn(prHander[0], grpNum, 1);
        if (ret != 0) {
                printf("IMP_OSD_ShowRgn() timeStamp error\n");
                return -1;
        }

        return 0;
}

static void *update_thread(void *p)
{
        int ret;

        /*generate time*/
        char DateStr[40];
        time_t currTime;
        struct tm *currDate;
        unsigned i = 0, j = 0;
        void *dateData = NULL;
        uint32_t *data = (uint32_t *)p;
        IMPOSDRgnAttrData rAttrData;

        ret = osd_show();
        if (ret < 0) {
                printf("OSD show error\n");
                return NULL;
        }

        while(g_osd_active) {
                        int penpos_t = 0;
                        int fontadv = 0;

                        time(&currTime);
                        currDate = localtime(&currTime);
                        memset(DateStr, 0, 40);
                        strftime(DateStr, 40, "%Y-%m-%d %H:%M:%S", currDate);
                        for (i = 0; i < 20; i++) {
                                switch(DateStr[i]) {
                                        case '0' ... '9':
                                                dateData = (void *)gBgramap[DateStr[i] - '0'].pdata;
                                                fontadv = gBgramap[DateStr[i] - '0'].width;
                                                penpos_t += gBgramap[DateStr[i] - '0'].width;
                                                break;
                                        case '-':
                                                dateData = (void *)gBgramap[10].pdata;
                                                fontadv = gBgramap[10].width;
                                                penpos_t += gBgramap[10].width;
                                                break;
                                        case ' ':
                                                dateData = (void *)gBgramap[11].pdata;
                                                fontadv = gBgramap[11].width;
                                                penpos_t += gBgramap[11].width;
                                                break;
                                        case ':':
                                                dateData = (void *)gBgramap[12].pdata;
                                                fontadv = gBgramap[12].width;
                                                penpos_t += gBgramap[12].width;
                                                break;
                                        default:
                                                break;
                                }
#ifdef SUPPORT_RGB555LE
                                for (j = 0; j < OSD_REGION_HEIGHT; j++) {
                                        memcpy((void *)((uint16_t *)data + j*20*OSD_REGION_WIDTH + penpos_t),
                                                        (void *)((uint16_t *)dateData + j*fontadv), fontadv*2);
                                }
#else
                                for (j = 0; j < OSD_REGION_HEIGHT; j++) {
                                        memcpy((void *)((uint32_t *)data + j*20*OSD_REGION_WIDTH + penpos_t),
                                                        (void *)((uint32_t *)dateData + j*fontadv), fontadv*4);
                                }

#endif
                        }
                        if (!g_osd_active) break;
                        rAttrData.picData.pData = data;
                        IMP_OSD_UpdateRgnAttrData(prHander[0], &rAttrData);

                        sleep(1);
        }

        return NULL;
}

int capture_and_encoding()
{
	int ret = 0;
	int i = 0;

	
	printf("[capture] Initializing pipeline...\n");

	// undocumented functions to increase pool size
#ifdef ENABLED_OSD
	IMP_OSD_SetPoolSize(0x64000);
#endif
	IMP_Encoder_SetPoolSize(0x100000);

	ret = sample_system_init();
	if (ret < 0) {
		printf("[capture] IMP_System_Init() failed\n");
		return -1;
	}
	printf("[capture] IMP system initialized\n");

	/* Step.2 FrameSource init */
	ret = sample_framesource_init();
	if (ret < 0) {
		printf("[capture] FrameSource init failed\n");
		return -1;
	}
	printf("[capture] FrameSource initialized\n");

	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_Encoder_CreateGroup(chn[i].index);
			if (ret < 0) {
				printf("IMP_Encoder_CreateGroup(%d) error !\n", i);
				return -1;
			}
		}
	}

	/* Step.3 Encoder init */
	ret = sample_encoder_init();
	if (ret < 0) {
		printf("[capture] Encoder init failed\n");
		return -1;
	}
	printf("[capture] Encoder initialized\n");

#ifdef ENABLED_OSD
	// Create the group for the OSD
	if (IMP_OSD_CreateGroup(0) < 0) {
			printf("IMP_OSD_CreateGroup(0) error !\n");
			return -1;
	}

	// Initialize the OSD that we want to use here
	prHander = sample_osd_init(grpNum);
			if (prHander <= 0) {
					printf("OSD init failed\n");
					return -1;
	}


	/* Step.4 Bind */
	// Create a bind chain for the framesource and OSD
	IMPCell osdcell = {DEV_ID_OSD, 0, 0};
	ret = IMP_System_Bind(&chn[0].framesource_chn, &osdcell);
	if (ret < 0) {
			printf("Bind FrameSource channel0 and OSD failed\n");
			return -1;
	}
	// Then bind the previous chain which is OSD with the encoder
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_Bind(&osdcell, &chn[i].imp_encoder);
			if (ret < 0) {
				printf("Bind FrameSource channel%d and Encoder failed\n",i);
				return -1;
			}
		}
	}

		/* Step.5 Create OSD bgramap update thread */
	pthread_t tid;
#ifdef SUPPORT_RGB555LE
	uint32_t *timeStampData = malloc(20 * OSD_REGION_HEIGHT * OSD_REGION_WIDTH * 2);
#else
	uint32_t *timeStampData = (uint32_t *)malloc(20 * OSD_REGION_HEIGHT * OSD_REGION_WIDTH * 4);
#endif
	if (timeStampData == NULL) {
			printf("valloc timeStampData error\n");
			return -1;
	}

	ret = pthread_create(&tid, NULL, update_thread, timeStampData);
	if (ret) {
			printf("thread create error\n");
			return -1;
	}
	g_osd_active = 1;
#else
    // bind without OST
    for (i = 0; i < FS_CHN_NUM; i++) {
    		if (chn[i].enable) {
                ret = IMP_System_Bind(&chn[0].framesource_chn, &chn[i].imp_encoder);
                if (ret < 0) {
                        printf("Bind FrameSource channel0 and Encoder failed\n");
                        return -1;
                }
            }
    }
#endif

	/* Step.6 Stream On */
	ret = sample_framesource_streamon();
	if (ret < 0) {
		printf("[capture] FrameSource stream-on failed\n");
		return -1;
	}
	printf("[capture] FrameSource streaming\n");


	// start thread for activating night mode & IR cut filter
#ifdef NIGHTMODE_SWITCH
    printf("[capture] Night mode thread starting\n");
	pthread_t thread_info;
	pthread_create(&thread_info, NULL, sample_soft_photosensitive_ctrl, NULL);
#else
    printf("[capture] Night mode disabled at compile time\n");
#endif

	printf("[capture] Pipeline ready\n");
	return 0;
}
