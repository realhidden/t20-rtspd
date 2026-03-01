#ifndef __GRAFANA_PUSH_H__
#define __GRAFANA_PUSH_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int enabled;
	char push_url[512];
	char username[64];
	char api_key[256];
	int push_interval_ms;
} grafana_push_config_t;

int grafana_push_init(const grafana_push_config_t *config);
void grafana_push_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* __GRAFANA_PUSH_H__ */
