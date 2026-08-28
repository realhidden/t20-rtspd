#ifndef __CAPTURE_AND_ENCODING_H
#define __CAPTURE_AND_ENCODING_H

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>

/* app_config_t is defined in imp-common.h (project-local), not the SDK one.
   We avoid including it here to prevent conflicts — cast from void* in .cpp */
int capture_and_encoding(void *config);
int destory(void);
int start_encoder_receiving(int chn);

#endif
