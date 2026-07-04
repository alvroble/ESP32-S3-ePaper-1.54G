#ifndef SDCARD_BSP_H
#define SDCARD_BSP_H
#include "driver/sdmmc_host.h"
#include "list.h"

extern sdmmc_card_t *card_host;

#define MAX_FILES 100

typedef struct
{
    char sdcard_name[400];
	int name_score;       
}sdcard_node_t; 

extern sdmmc_card_t *card_host;
extern list_t *sdcard_scan_listhandle;

#ifdef __cplusplus
extern "C" {
#endif

uint8_t _sdcard_init(void);


uint32_t s_example_read_from_offset(const char *path, char *buffer, uint32_t len, uint32_t offset);
uint32_t s_example_wriet_from_offset(const char *path, char *buffer, uint32_t len, uint8_t mode);

int list_iterator(void); 
int sdcard_write_file(const char *path, const void *data, size_t data_len);
int sdcard_read_file(const char *path, uint8_t *buffer, size_t *outLen);
void list_scan_dir(const char *path);

int scan_imgs_and_save_list();
int load_file_list(const char *list_path, char file_names[MAX_FILES][128], int *file_count);
int get_current_img_path(char *out_path, int out_size);
int get_img_name_by_index(char *pathName, int pathLen);

list_node_t *get_Currently_node(void);
void set_Currently_node(list_node_t *node);

int get_bmp_count_by_scan(const char *path);

#ifdef __cplusplus
}
#endif

#endif