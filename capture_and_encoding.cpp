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

int grpNum = 0;
IMPRgnHandle *prHander;

int destory()
{

	int ret, i;

	/* Exit sequence as follow */
	/* Step.a Stream Off */
	ret = sample_framesource_streamoff();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "FrameSource StreamOff failed\n");
		return -1;
	}

	/* Step.b UnBind */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_UnBind(&chn[i].framesource_chn, &chn[i].imp_encoder);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "UnBind FrameSource channel%d and Encoder failed\n",i);
				return -1;
			}
		}
	}

	/* Step.c Encoder exit */
	ret = sample_encoder_exit();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Encoder exit failed\n");
		return -1;
	}

	/* Step.d FrameSource exit */
	ret = sample_framesource_exit();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "FrameSource exit failed\n");
		return -1;
	}

	/* Step.e System exit */
	ret = sample_system_exit();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "sample_system_exit() failed\n");
		return -1;
	}

	ret = IMP_Encoder_StopRecvPic(0);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_Encoder_StopRecvPic() failed\n");
		return -1;
	}

	return 0;
}

static int save_stream(int fd, IMPEncoderStream *stream)
{
	unsigned int ret;
	int i, nr_pack = stream->packCount;

	for (i = 0; i < nr_pack; i++) {
		ret = write(fd, (void *)stream->pack[i].virAddr,
					stream->pack[i].length);
		if (ret != stream->pack[i].length){
			IMP_LOG_ERR(TAG,"stream write error:%s\n", strerror(errno));
			return -1;
		}
	}

	return 0;
}

static int get_h264_stream(int fd, int chn)
{
	int ret;
	
	/* Polling H264 Stream, set timeout as 1000msec */
	ret = IMP_Encoder_PollingStream(chn, 100);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Polling stream timeout\n");
	}

	IMPEncoderStream stream;
	/* Get H264 Stream */
	ret = IMP_Encoder_GetStream(chn, &stream, 1);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_Encoder_GetStream() failed\n");
		return -1;
	}
	
	ret = save_stream(fd, &stream);
	if (ret < 0) {
		close(fd);
		return ret;
	}
	
	IMP_Encoder_ReleaseStream(chn, &stream);

	return 0;
}

void *get_stream(int fd, int chn)
{
	int  ret;
	
	ret = IMP_Encoder_StartRecvPic(chn);
	if (ret < 0){
		IMP_LOG_ERR(TAG, "IMP_Encoder_StartRecvPic(%d) failed\n", 1);
		return NULL;
	}
	ret = get_h264_stream(fd, chn);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Get H264 stream failed\n");
		return NULL;
	}
/*	ret = IMP_Encoder_StopRecvPic(chn);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_Encoder_StopRecvPic() failed\n");
		return NULL;
	}
*/
	return 0;
}


// THE OSD PARTS STARTS HERE

static int osd_show(void)
{
        int ret;

        ret = IMP_OSD_ShowRgn(prHander[0], grpNum, 1);
        if (ret != 0) {
                IMP_LOG_ERR(TAG, "IMP_OSD_ShowRgn() timeStamp error\n");
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
                IMP_LOG_ERR(TAG, "OSD show error\n");
                return NULL;
        }

        while(1) {
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

	
	printf(">>>>>caputre_and_encoding start\n");

	// undocumented functions to increase pool size
	IMP_OSD_SetPoolSize(0x64000);
	IMP_Encoder_SetPoolSize(0x100000);

	ret = sample_system_init();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_System_Init() failed\n");
		return -1;
	}

	/* Step.2 FrameSource init */
	ret = sample_framesource_init();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "FrameSource init failed\n");
		return -1;
	}

	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_Encoder_CreateGroup(chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_CreateGroup(%d) error !\n", i);
				return -1;
			}
		}
	}

	/* Step.3 Encoder init */
	ret = sample_encoder_init();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Encoder init failed\n");
		return -1;
	}

	// Create the group for the OSD
	if (IMP_OSD_CreateGroup(0) < 0) {
			IMP_LOG_ERR(TAG, "IMP_OSD_CreateGroup(0) error !\n");
			return -1;
	}

	// Initialize the OSD that we want to use here
	prHander = sample_osd_init(grpNum);
			if (prHander <= 0) {
					IMP_LOG_ERR(TAG, "OSD init failed\n");
					return -1;
	}


	/* Step.4 Bind */
	// Create a bind chain for the framesource and OSD
	IMPCell osdcell = {DEV_ID_OSD, 0, 0};
	ret = IMP_System_Bind(&chn[0].framesource_chn, &osdcell);
	if (ret < 0) {
			IMP_LOG_ERR(TAG, "Bind FrameSource channel0 and OSD failed\n");
			return -1;
	}
	// Then bind the previous chain which is OSD with the encoder
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_Bind(&osdcell, &chn[i].imp_encoder);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "Bind FrameSource channel%d and Encoder failed\n",i);
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
			IMP_LOG_ERR(TAG, "valloc timeStampData error\n");
			return -1;
	}

	ret = pthread_create(&tid, NULL, update_thread, timeStampData);
	if (ret) {
			IMP_LOG_ERR(TAG, "thread create error\n");
			return -1;
	}

	/* Step.6 Stream On */
	ret = sample_framesource_streamon();
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "ImpStreamOn failed\n");
		return -1;
	}


	return 0;
}
