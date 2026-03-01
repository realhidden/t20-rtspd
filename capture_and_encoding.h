#ifndef __CAPTURE_AND_ENCODING_H
#define __CAPTURE_AND_ENCODING_H

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>

int capture_and_encoding(void);
int destory(void);
int save_stream_to_fd(int fd, IMPEncoderStream *stream);
int start_encoder_receiving(int chn);

#endif
