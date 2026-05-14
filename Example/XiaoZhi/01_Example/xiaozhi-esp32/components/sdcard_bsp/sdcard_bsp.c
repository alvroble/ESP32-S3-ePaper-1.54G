#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdcard_bsp.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "_sdcard";

#define SDMMC_D0_PIN    40  
#define SDMMC_CLK_PIN   39
#define SDMMC_CMD_PIN   41

#define SDlist "/sdcard" 

// Image position, list position, image count position
#define img_list        "/sdcard/bmp"
#define img_path        "/sdcard/fileList.txt"
#define img_index       "/sdcard/index.txt"

sdmmc_card_t *card_host = NULL;

list_t *sdcard_scan_listhandle = NULL;

static list_node_t *Currently_node = NULL; 

uint8_t _sdcard_init(void) {
    sdcard_scan_listhandle = list_new();
    esp_vfs_fat_sdmmc_mount_config_t mount_config =
        {
            .format_if_mount_failed = false,         
            .max_files              = 5,             
            .allocation_unit_size   = 16 * 1024 * 3, 
        };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width = 1;           //1线
  		slot_config.clk = SDMMC_CLK_PIN;
  		slot_config.cmd = SDMMC_CMD_PIN;
  		slot_config.d0 = SDMMC_D0_PIN;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_vfs_fat_sdmmc_mount(SDlist, &host, &slot_config, &mount_config, &card_host));

    if (card_host != NULL) {
        sdmmc_card_print_info(stdout, card_host);
        return 1;
    }
    return 0;
}


/**
 * @brief  write data
 * 
 * @param path file address
 * @param data The data to be written
 * @return * esp_err_t 
 */

 int sdcard_write_file(const char *path, const void *data, size_t data_len) {
    if (card_host == NULL) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (sdmmc_get_status(card_host) != ESP_OK) {
        ESP_LOGE(TAG, "SD card not ready");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);

    if (written != data_len) {
        ESP_LOGE(TAG, "Write failed (%zu/%zu bytes)", written, data_len);
        return ESP_FAIL;
    }

    //ESP_LOGI(TAG, "File written: %s (%zu bytes)", path, data_len);
    return ESP_OK;
}
esp_err_t s_example_write_file(const char *path, char *data)
{
  esp_err_t err;
  if(card_host == NULL)
  {
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card_host);
  if(err != ESP_OK)
  {
    return err;
  }
  FILE *f = fopen(path, "w");
  if(f == NULL)
  {
    printf("path:Write Wrong path\n");
    return ESP_ERR_NOT_FOUND;
  }
  fprintf(f, data); //写入
  fclose(f);
  return ESP_OK;
}

/**
 * @brief   reading data
 * 
 * @param path      file address
 * @param pxbuf     The address for saving the read data
 * @param outLen    Read the length of the data
 * @return esp_err_t 
 */
esp_err_t s_example_read_file(const char *path,uint8_t *pxbuf,uint32_t *outLen)
{
  esp_err_t err;
  if(card_host == NULL)
  {
    printf("path:card == NULL\n");
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card_host);
  if(err != ESP_OK)
  {
    printf("path:card == NO\n");
    return err;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    printf("path:Read Wrong path\n");
    return ESP_ERR_NOT_FOUND;
  }
  fseek(f, 0, SEEK_END); 
  uint32_t unlen = ftell(f);
  //fgets(pxbuf, unlen, f); 
  fseek(f, 0, SEEK_SET);
  uint32_t poutLen = fread((void *)pxbuf,1,unlen,f);
  printf("pxlen: %ld,outLen: %ld\n",unlen,poutLen);
  *outLen = poutLen;
  fclose(f);
  return ESP_OK;
}

int sdcard_read_file(const char *path, uint8_t *buffer, size_t *outLen) {
    if (card_host == NULL) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (sdmmc_get_status(card_host) != ESP_OK) {
        ESP_LOGE(TAG, "SD card not ready");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size");
        fclose(f);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_SET);

    size_t bytes_read = fread(buffer, 1, file_size, f);
    fclose(f);

    if (outLen) *outLen = bytes_read;

    //ESP_LOGI(TAG, "Read %zu/%ld bytes from %s", bytes_read, file_size, path);
    return (bytes_read > 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief   reading data
 * 
 * @param path      file address
 * @param buffer    The address for saving the read data
 * @param len       Read the length of the data
 * @param offset    File offset address
 * @return esp_err_t 
 */
uint32_t s_example_read_from_offset(const char *path, char *buffer, uint32_t len, uint32_t offset)
{
  esp_err_t err;
  if (card_host == NULL)
  {
    ESP_LOGE(TAG, "SD card not initialized (card == NULL)");
    return 0;
  }
  err = sdmmc_get_status(card_host);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card status check failed (card not present or unresponsive)");
    return 0;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return 0;
  }

  fseek(f, offset, SEEK_SET);
  uint32_t bytesRead = fread((void *)buffer, 1, len, f);
  fclose(f);
  //ESP_LOGI(TAG, "Read %u bytes from file: %s (offset: %u)", bytesRead, path, offset);
  return bytesRead;
}
/**
 * @brief   write data
 * 
 * @param path      file address
 * @param buffer    The data to be written
 * @param len       data length
 * @param offset    File offset address
 * @return esp_err_t 
 */
uint32_t s_example_wriet_from_offset(const char *path, char *buffer, uint32_t len, uint8_t mode)
{
  esp_err_t err;
  if (card_host == NULL)
  {
    ESP_LOGE(TAG, "SD card not initialized (card == NULL)");
    return 0;
  }
  err = sdmmc_get_status(card_host);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card status check failed (card not present or unresponsive)");
    return 0;
  }
  FILE *f = NULL;
  if(mode == 0)
  {
    f = fopen(path, "w");
    if (f == NULL)
    {
      ESP_LOGE(TAG, "Failed to open file: %s", path);
      return 0;
    }
    fclose(f);
    return 0;
  }
  else
  {
    f = fopen(path, "ab");
    if (f == NULL)
    {
      ESP_LOGE(TAG, "Failed to open file: %s", path);
      return 0;
    }
    uint32_t bytesRead = fwrite((void *)buffer, 1, len, f);
    fclose(f);
    return bytesRead;
  }
}


// Read the file and sort it
int scan_imgs_and_save_list()
{
    char *file_names[MAX_FILES];
    int file_count = 0;

    DIR *dir = opendir(img_list);
    if (!dir) {
        ESP_LOGE("sdscan", "Failed to open directory: %s", img_list);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && file_count < MAX_FILES) {
        if (entry->d_type == DT_REG) {
            file_names[file_count] = strdup(entry->d_name);
            file_count++;
        }
    }
    closedir(dir);

    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i + 1; j < file_count; j++) {
            if (strcmp(file_names[i], file_names[j]) > 0) {
                char *tmp = file_names[i];
                file_names[i] = file_names[j];
                file_names[j] = tmp;
            }
        }
    }

    FILE *list_file = fopen(img_path, "w");
    if (!list_file) {
        ESP_LOGE("sdscan", "Failed to create file: %s", img_path);
        for (int i = 0; i < file_count; i++) free(file_names[i]);
        return -2;
    }
    for (int i = 0; i < file_count; i++) {
        fprintf(list_file, "%s\n", file_names[i]);
        free(file_names[i]);
    }
    fclose(list_file);

    int old_index = 0, old_count = 0;
    FILE *index_file = fopen(img_index, "r");
    bool need_reset = false;
    if (index_file) {
        if (fscanf(index_file, "%d\n%d", &old_index, &old_count) != 2) {
            need_reset = true;
        }
        fclose(index_file);
        if (old_count != file_count) {
            need_reset = true;
        }
    } else {
        need_reset = true;
    }

    index_file = fopen(img_index, "w");
    if (!index_file) {
        ESP_LOGE("sdscan", "Failed to create file: %s", img_index);
        return -3;
    }
    if (need_reset) {
        fprintf(index_file, "0\n%d\n", file_count);
    } else {
        int write_index = (old_index >= file_count || old_index < 0) ? 0 : old_index;
        fprintf(index_file, "%d\n%d\n", write_index, file_count);
    }
    fclose(index_file);

    return file_count;
}

// Read the file list
int load_file_list(const char *list_path, char file_names[MAX_FILES][128], int *file_count)
{
    FILE *f = fopen(list_path, "r");
    if (!f) return -1;
    int count = 0;
    while (count < MAX_FILES && fgets(file_names[count], 128, f)) {
        char *newline = strchr(file_names[count], '\n');
        if (newline) *newline = '\0';
        count++;
    }
    fclose(f);
    *file_count = count;
    return 0;
}

// Read the current index
int load_index(const char *index_path)
{
    FILE *f = fopen(index_path, "r");
    if (!f) return 0;
    int idx = 0;
    fscanf(f, "%d", &idx);
    fclose(f);
    return idx;
}

// Get the current image path
int get_current_img_path(char *out_path, int out_size)
{
    char file_names[MAX_FILES][128];
    int file_count = 0;
    if (load_file_list(img_path, file_names, &file_count) != 0) return -1;
    int idx = load_index(img_index);
    if (idx < 0 || idx >= file_count) return -2;
    snprintf(out_path, out_size, "%s/%s", img_list, file_names[idx]);
    return 0;
}

// Obtain the file name of the index line in fileList.txt and concatenate the complete path to pathName
int get_img_name_by_index(char *pathName, int pathLen)
{
    FILE *index_file = fopen(img_index, "r");
    int targetIndex = 0, file_count = 0;
    if (index_file) {
        fscanf(index_file, "%d\n%d", &targetIndex, &file_count);
        fclose(index_file);
    } else {
        return -1;
    }

    if (file_count == 0) return -2;
    if (targetIndex >= file_count || targetIndex < 0) targetIndex = 0;

    FILE *fil = fopen(img_path, "r");
    if (!fil) return -1;
    int i = 0;
    char fileName[128] = {0};
    while (fgets(fileName, sizeof(fileName), fil)) {
        if (i == targetIndex) {
            char *newline = strchr(fileName, '\n');
            if (newline) *newline = '\0';
            snprintf(pathName, pathLen, "%s/%s", img_list, fileName);
            fclose(fil);

            int newIndex = targetIndex + 1;
            if (newIndex >= file_count) newIndex = 0;
            FILE *index_file2 = fopen(img_index, "w");
            if (index_file2) {
                fprintf(index_file2, "%d\n%d\n", newIndex, file_count);
                fclose(index_file2);
            }
            return 0;
        }
        i++;
    }
    fclose(fil);
    return -2; 
}

void list_scan_dir(const char *path) {
    struct dirent *entry;
    DIR           *dir = opendir(path);

    if (dir == NULL) {
        ESP_LOGE("sdscan", "Failed to open directory: %s", path);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) { 
            ESP_LOGI("sdscan", "Directory: %s", entry->d_name);
        } else {
            if (strstr(entry->d_name, ".bmp") == NULL) {
                continue;
            }
            uint16_t       _strlen   = strlen(path) + strlen(entry->d_name) + 1 + 1; 
            sdcard_node_t *node_data = (sdcard_node_t *) LIST_MALLOC(sizeof(sdcard_node_t));
            assert(node_data);
            if (_strlen > 96) {
                ESP_LOGE("sdcard", "scan file fill _strlen:%d", _strlen);
                continue;
            }
            node_data->name_score = 0;
            snprintf(node_data->sdcard_name, sizeof(node_data->sdcard_name) - 2, "%s/%s", path, entry->d_name); 
            list_rpush(sdcard_scan_listhandle, list_node_new(node_data));                                       
        }
    }
    closedir(dir);
}

int list_iterator(void) 
{
    int              Quantity = 0;
    list_iterator_t *it       = list_iterator_new(sdcard_scan_listhandle, LIST_HEAD); 
    list_node_t     *node     = list_iterator_next(it);
    while (node != NULL) {
        sdcard_node_t *sdcard_node = (sdcard_node_t *) node->val;
        ESP_LOGI("sdscan", "File: %s", sdcard_node->sdcard_name);
        node = list_iterator_next(it);
        Quantity++;
    }
    list_iterator_destroy(it); 
    return Quantity;
}

list_node_t *get_Currently_node(void) {
    return Currently_node;
}
void set_Currently_node(list_node_t *node) {
    Currently_node = node;
}

int get_bmp_count_by_scan(const char *path) {
    if (card_host == NULL) {
        ESP_LOGE(TAG, "SD卡未初始化");
        return -1;
    }

    if (sdmmc_get_status(card_host) != ESP_OK) {
        ESP_LOGE(TAG, "SD卡未就绪");
        return -1;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "打开目录失败：%s", path);
        return -1;
    }

    int bmp_count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        
        char *suffix = strrchr(entry->d_name, '.');
        if (suffix && (strcmp(suffix, ".bmp") == 0 || strcmp(suffix, ".BMP") == 0)) {
            bmp_count++;
            ESP_LOGD(TAG, "找到BMP文件：%s/%s", path, entry->d_name);
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "目录%s下BMP文件数量：%d", path, bmp_count);
    return bmp_count;
}