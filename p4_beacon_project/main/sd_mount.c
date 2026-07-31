#include "sd_mount.h"
#include "beacon_config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sd";
static SemaphoreHandle_t s_lock;
static sdmmc_card_t *s_card;

void sd_lock(void)   { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void sd_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

esp_err_t sd_mount_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = BEACON_SD_SPI_FREQ_KHZ;

    spi_bus_config_t bus = {
        .mosi_io_num = BEACON_SD_MOSI,
        .miso_io_num = BEACON_SD_MISO,
        .sclk_io_num = BEACON_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed (%s)", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = BEACON_SD_CS;
    dev.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 0,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &dev, &mnt, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD (SPI) mount failed (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "Checks: module VCC (regulator-style HW-125 boards want 5V), "
                      "short wires, common ground, pins in beacon_config.h. "
                      "If init is flaky, lower BEACON_SD_SPI_FREQ_KHZ.");
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD mounted at %s over SPI (%d kHz)",
             SD_MOUNT, BEACON_SD_SPI_FREQ_KHZ);
    return ESP_OK;
}
