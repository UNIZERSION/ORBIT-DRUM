// Tab5 board services: I2C bus, IO expanders 0x43 (opened once: LCD / touch power, HP_DET input) and 0x44
// (charger, USB 5 V, ESP32-C6 supply), battery (INA226), board info, diagnostics, NVS init, return to the
// Tab5 launcher, the service timer.
#include <stdio.h>
#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
// esp_ota_ops.h (component app_update) is used when firmware/main/CMakeLists.txt REQUIRES app_update;
// without it the same thing is done through esp_partition (see restartToLauncher).
#if defined(__has_include)
#if __has_include("esp_ota_ops.h")
#include "esp_ota_ops.h"
#define ORBIT_HAVE_OTA_OPS 1
#endif
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "hal/hal_private.h"
#include "nvs_flash.h"

namespace hal {

namespace {
const char* TAG = "orbit_board";

// PI4IOE5V6408 registers
constexpr uint8_t kRegDir = 0x03, kRegOut = 0x05, kRegHighZ = 0x07, kRegPullEn = 0x0B;

bool s_i2c_ok = false;
bool s_board_ok = false;
esp_io_expander_handle_t s_ioe1 = nullptr;  // 0x43, BSP driver (__atomic: the aux task reads it)
int64_t s_touch_on_at = 0;                  // __atomic
i2c_master_dev_handle_t s_ioe2 = nullptr;   // 0x44
i2c_master_dev_handle_t s_ina = nullptr;    // 0x41 battery monitor
SemaphoreHandle_t s_ioe2_mutex = nullptr;
char s_info[96];

esp_err_t ioe2Update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t v = 0;
    esp_err_t err = detail::i2cRead(s_ioe2, reg, &v, 1);
    if (err != ESP_OK) return err;
    return detail::i2cWrite(s_ioe2, reg, (uint8_t)((v & ~mask) | (value & mask)));
}

bool ioe2Open()
{
    if (!s_ioe2_mutex) s_ioe2_mutex = xSemaphoreCreateMutex();
    if (s_ioe2) return true;
    if (!detail::i2cReady()) return false;
    return detail::i2cAdd(0x44, &s_ioe2) == ESP_OK;
}

// INA226 bus voltage in mV, -1 on error
int batteryMv()
{
    uint8_t d[2];
    if (!s_ina || detail::i2cRead(s_ina, 0x02, d, 2) != ESP_OK) return -1;
    return (((int)d[0] << 8) | d[1]) * 5 / 4;   // LSB 1.25 mV
}
}  // namespace

// ---------------------------------------------------------------- detail: I2C + expander 0x44
namespace detail {

#if !configUSE_TRACE_FACILITY
#error "I2cCeiling reads the base priority with vTaskGetInfo: CONFIG_FREERTOS_USE_TRACE_FACILITY=y"
#endif

I2cCeiling::I2cCeiling()
{
    if (uxTaskPriorityGet(nullptr) >= kI2cPrio) return;   // touch (200 Hz) and audio: no vTaskGetInfo
    TaskStatus_t st;
    vTaskGetInfo(nullptr, &st, pdFALSE, eRunning);
    base_ = st.uxBasePriority;   // vTaskPrioritySet back to it keeps a priority inherited meanwhile
    vTaskPrioritySet(nullptr, kI2cPrio);
    raised_ = true;
}

I2cCeiling::~I2cCeiling()
{
    if (raised_) vTaskPrioritySet(nullptr, base_);
}

bool i2cReady()
{
    if (s_i2c_ok) return true;
    for (int attempt = 0; attempt < 3; ++attempt) {
        esp_err_t err = bsp_i2c_init();
        i2c_master_bus_handle_t bus = err == ESP_OK ? bsp_i2c_get_handle() : nullptr;
        if (bus) {
            s_i2c_ok = true;
            return true;
        }
        ESP_LOGW(TAG, "I2C init %d/3: %s", attempt + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(50 * (attempt + 1)));
    }
    return false;
}

esp_err_t i2cAdd(uint16_t addr, i2c_master_dev_handle_t* dev)
{
    if (!i2cReady()) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr;
    cfg.scl_speed_hz = 400000;
    return i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, dev);
}

esp_err_t i2cRead(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* data, size_t len)
{
    if (!dev) return ESP_ERR_INVALID_STATE;
    I2cCeiling ceiling;
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, 50);
}

esp_err_t i2cWrite(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    if (!dev) return ESP_ERR_INVALID_STATE;
    const uint8_t w[2] = {reg, value};
    I2cCeiling ceiling;
    return i2c_master_transmit(dev, w, 2, 50);
}

esp_err_t ioe2Output(uint8_t mask, bool high)
{
    if (!ioe2Open()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ioe2_mutex, portMAX_DELAY);
    // level first, then push-pull, then output: never a glitch on a pin that was an input
    esp_err_t err = ioe2Update(kRegOut, mask, high ? mask : 0);
    if (err == ESP_OK) err = ioe2Update(kRegHighZ, mask, 0);
    if (err == ESP_OK) err = ioe2Update(kRegDir, mask, mask);
    xSemaphoreGive(s_ioe2_mutex);
    return err;
}

esp_err_t ioe2Input(uint8_t mask)
{
    if (!ioe2Open()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ioe2_mutex, portMAX_DELAY);
    esp_err_t err = ioe2Update(kRegDir, mask, 0);
    if (err == ESP_OK) err = ioe2Update(kRegPullEn, mask, 0);
    if (err == ESP_OK) err = ioe2Update(kRegHighZ, mask, 0);
    xSemaphoreGive(s_ioe2_mutex);
    return err;
}

esp_io_expander_handle_t ioe1() { return __atomic_load_n(&s_ioe1, __ATOMIC_ACQUIRE); }

esp_io_expander_handle_t ioe1Open()
{
    esp_io_expander_handle_t io = ioe1();
    if (io) return io;
    if (!i2cReady()) return nullptr;
    I2cCeiling ceiling;   // app_main (priority 1) while the panel task (5) starts the display
    for (int attempt = 0; attempt < 3 && !io; ++attempt) {
        if (attempt) {
            i2c_master_bus_reset(bsp_i2c_get_handle());   // the bus can hang at power-up
            vTaskDelay(pdMS_TO_TICKS(20 * attempt));
        }
        io = bsp_io_expander_init();   // creates and resets the chip (all pins Hi-Z outputs, pull-down)
    }
    if (!io) {
        ESP_LOGW(TAG, "IO expander 0x43 not found");
        return nullptr;
    }
    // HP_DET (P7): after the reset a Hi-Z "output" with the internal pull-down on, which fights the jack
    // detect network. Plain input, no pull.
    esp_err_t err = esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_INPUT);
    if (err == ESP_OK) err = esp_io_expander_set_pullupdown(io, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_PULL_NONE);
    if (err != ESP_OK) ESP_LOGW(TAG, "HP_DET (0x43 P7) as input: %s", esp_err_to_name(err));
    uint32_t in = 0;
    if (esp_io_expander_get_level(io, 0xFF, &in) == ESP_OK) {
        ESP_LOGI(TAG, "IO expander 0x43: IN_STA 0x%02x (HP_DET = bit 7)", (unsigned)in);
    }
    __atomic_store_n(&s_ioe1, io, __ATOMIC_RELEASE);
    return io;
}

int64_t touchPowerOnUs() { return __atomic_load_n(&s_touch_on_at, __ATOMIC_ACQUIRE); }

}  // namespace detail

// ---------------------------------------------------------------- board
bool boardInit()
{
    if (s_board_ok) return true;
    // NVS is shared with the Tab5 launcher (its namespaces "l5", "nvs.net80211"): never erased here.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "NVS init: %s (settings stay on the SD card only)", esp_err_to_name(err));

    if (!detail::i2cReady()) {
        ESP_LOGE(TAG, "I2C bus not available");
        return false;
    }
    // IO expander 0x43 (LCD/touch enable, SPK_EN, HP_DET): the BSP driver resets it once, here,
    // before the display and the audio use it (up to 3 attempts; lcdPower retries once more).
    if (detail::ioe1Open()) {
        detail::I2cCeiling ceiling;
        // Panel and touch power in the BSP's order (LCD_RST released, then TP_RST), as bsp_display_new
        // does it: the touch controller's 500 ms settle time then passes during the SD mount and App::init
        // instead of in displayStart (probeLaneRate waits only for what is left of it).
        if (bsp_feature_enable(BSP_FEATURE_LCD, true) == ESP_OK &&
            bsp_feature_enable(BSP_FEATURE_TOUCH, true) == ESP_OK) {
            __atomic_store_n(&s_touch_on_at, esp_timer_get_time(), __ATOMIC_RELEASE);
        }
    }

    // Expander 0x44: charging allowed (as the launcher), the ESP32-C6 (Wi-Fi) stays powered down.
    if (detail::ioe2Output(detail::kIoe2ChgEn, true) != ESP_OK ||
        detail::ioe2Input(detail::kIoe2ChgStat) != ESP_OK ||
        detail::ioe2Output(detail::kIoe2Wlan, false) != ESP_OK) {
        ESP_LOGW(TAG, "IO expander 0x44 not configured");
    }

    // INA226 battery monitor: 16 samples, 1.1 ms conversions, continuous
    if (detail::i2cAdd(0x41, &s_ina) == ESP_OK) {
        const uint8_t cfg[3] = {0x00, 0x05, 0x27};
        esp_err_t ina_err;
        {
            detail::I2cCeiling ceiling;
            ina_err = i2c_master_transmit(s_ina, cfg, 3, 50);
        }
        if (ina_err != ESP_OK) {
            i2c_master_bus_rm_device(s_ina);
            s_ina = nullptr;
            ESP_LOGW(TAG, "INA226 (0x41) not answering: battery unknown");
        }
    }

    detail::audioAuxStart();
    s_board_ok = true;
    ESP_LOGI(TAG, "%s", boardInfo());
    return true;
}

const char* boardInfo()
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    const unsigned mb = (unsigned)(esp_psram_get_size() / (1024u * 1024u));
    snprintf(s_info, sizeof(s_info), "ESP32-P4 v%d.%d, %s, %u MB PSRAM", ci.revision / 100, ci.revision % 100,
             detail::panelName(), mb);
    return s_info;
}

uint32_t freeInternal() { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

void diagnostics(Diagnostics& out)
{
    out = Diagnostics{};
    out.ioe_inputs = -1;
    detail::audioDiag(&out, false);
    detail::displayDiag(&out, false);
}

void diagnosticsReset()
{
    detail::audioDiag(nullptr, true);
    detail::displayDiag(nullptr, true);
}
uint32_t freePsram() { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

namespace {
constexpr uint32_t kLayoutMagic = 0x4C41594Fu;   // "LAYO"
RTC_NOINIT_ATTR uint32_t s_layout_magic;
RTC_NOINIT_ATTR uint32_t s_layout_value;
}  // namespace

int memLayout() { return s_layout_magic == kLayoutMagic && s_layout_value <= 1u ? (int)s_layout_value : 1; }

void memLayoutRestart(int layout)
{
    s_layout_value = layout ? 1u : 0u;
    s_layout_magic = kLayoutMagic;
    esp_restart();
}

int batteryPercent()
{
    const int mv = batteryMv();
    if (mv < 5000 || mv > 9000) return -1;   // no pack (USB only) or no reading
    // 2S Li-ion, linear 6.4 V (empty) .. 8.3 V (full)
    const int pct = (mv - 6400) * 100 / (8300 - 6400);
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

bool charging()
{
    if (batteryPercent() < 0) return false;
    uint8_t d[2];
    if (detail::i2cRead(s_ina, 0x01, d, 2) != ESP_OK) return false;
    // shunt 5 mOhm, 2.5 uV/LSB = 0.5 mA; negative raw = current into the pack
    const int ma = -(int)(int16_t)(((uint16_t)d[0] << 8) | d[1]) / 2;
    return ma > 20;
}

uint64_t micros() { return (uint64_t)esp_timer_get_time(); }

void restart() { esp_restart(); }   // display.cpp blanks the panel in a shutdown handler

// ---------------------------------------------------------------- Tab5 launcher
// Launcher layout (tab5_launcher's table): nvs, otadata, launcher = factory app at 0x10000, app0 = ota_0
// (where the launcher installs ORBIT), vfs, ... Standalone layout (firmware/partitions.csv): ORBIT is the
// only app, in the factory slot, and there is no otadata partition.
namespace {
// The factory app partition when it holds a valid image of another project, else nullptr.
const esp_partition_t* launcherPartition()
{
    const esp_partition_t* f = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (!f) return nullptr;
    // Without otadata the bootloader always starts the factory app: that is this app.
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr)) return nullptr;
#if defined(ORBIT_HAVE_OTA_OPS)
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (run && run->address == f->address) return nullptr;
#endif
    // esp_image_header_t (24 B) + first segment header (8 B), then esp_app_desc_t:
    // magic 0xABCD5432 at +0, project_name[32] at +48.
    uint8_t img[32 + 80];
    memset(img, 0, sizeof img);
    if (esp_partition_read(f, 0, img, sizeof img) != ESP_OK || img[0] != 0xE9) return nullptr;   // no image
    uint32_t magic = 0;
    memcpy(&magic, img + 32, sizeof magic);
    if (magic == 0xABCD5432u) {
        char name[33];
        memcpy(name, img + 32 + 48, 32);
        name[32] = 0;
        if (strncmp(name, "orbit", 5) == 0) return nullptr;   // ORBIT itself was flashed into the factory slot
        ESP_LOGI(TAG, "launcher: \"%s\" in factory @0x%lx", name, (unsigned long)f->address);
    }
    return f;
}
}  // namespace

bool launcherAvailable() { return launcherPartition() != nullptr; }

bool restartToLauncher()
{
    const esp_partition_t* f = launcherPartition();
    if (!f) return false;
#if defined(ORBIT_HAVE_OTA_OPS)
    const esp_err_t err = esp_ota_set_boot_partition(f);
#else
    // What esp_ota_set_boot_partition(factory) does: an erased otadata makes the bootloader start the
    // factory app.
    const esp_partition_t* ota = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    const esp_err_t err = ota ? esp_partition_erase_range(ota, 0, ota->size) : ESP_ERR_NOT_FOUND;
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "launcher: boot partition not set (%s)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "restarting into the launcher");
    esp_restart();
    return true;
}

// ---------------------------------------------------------------- service timer
namespace {
esp_timer_handle_t s_svc = nullptr;
ServiceFn s_svc_fn = nullptr;    // __atomic
void* s_svc_user = nullptr;      // __atomic

void serviceTick(void*)
{
    const ServiceFn fn = __atomic_load_n(&s_svc_fn, __ATOMIC_ACQUIRE);
    if (fn) fn(__atomic_load_n(&s_svc_user, __ATOMIC_ACQUIRE));
}
}  // namespace

bool serviceTimerStart(ServiceFn fn, void* user, uint32_t period_us)
{
    if (s_svc) esp_timer_stop(s_svc);   // ESP_ERR_INVALID_STATE when it was not running: fine
    __atomic_store_n(&s_svc_fn, (ServiceFn) nullptr, __ATOMIC_RELEASE);
    if (!fn) return true;
    if (period_us < 500) period_us = 500;
    if (!s_svc) {
        esp_timer_create_args_t a = {};
        a.callback = serviceTick;
        a.arg = nullptr;
        a.dispatch_method = ESP_TIMER_TASK;
        a.name = "orbit_svc";
        a.skip_unhandled_events = true;
        if (esp_timer_create(&a, &s_svc) != ESP_OK) {
            s_svc = nullptr;
            ESP_LOGE(TAG, "service timer not created");
            return false;
        }
    }
    __atomic_store_n(&s_svc_user, user, __ATOMIC_RELEASE);
    __atomic_store_n(&s_svc_fn, fn, __ATOMIC_RELEASE);
    if (esp_timer_start_periodic(s_svc, period_us) != ESP_OK) {
        __atomic_store_n(&s_svc_fn, (ServiceFn) nullptr, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}

}  // namespace hal
