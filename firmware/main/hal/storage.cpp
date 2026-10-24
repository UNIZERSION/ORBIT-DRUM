// SD card (SDMMC slot 0, 4-bit, 40 MHz, FAT32 + exFAT, /sdcard) and device settings.
//
// Settings live in /sdcard/ORBIT/settings.bin (header: magic, version, payload size, CRC-32) and are
// mirrored to NVS (namespace "orbit", key "settings", same bytes). A flash write stalls the cache of both
// cores, so the NVS mirror is written only while the App reports the audio as idle (setAudioIdle) or the
// stream is stopped; otherwise the aux task (audio.cpp) writes it at the next idle moment.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bsp/m5stack_tab5.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "hal/hal_private.h"
#include "nvs.h"
#include "sdmmc_cmd.h"

namespace hal {

namespace {
const char* TAG = "orbit_storage";
constexpr const char* kRoot = "/sdcard";
constexpr const char* kDir = "/sdcard/ORBIT";
constexpr const char* kFile = "/sdcard/ORBIT/settings.bin";
constexpr const char* kTmp = "/sdcard/ORBIT/settings.tmp";
constexpr uint32_t kMagic = 0x5342524Fu;   // "ORBS"
constexpr uint16_t kVersion = 1;
constexpr size_t kMaxPayload = 512;

bool s_mounted = false;
bool s_idle = true;                        // __atomic, from setAudioIdle
SemaphoreHandle_t s_nvs_mutex = nullptr;
bool s_nvs_pending = false;
orbit::Settings s_nvs_copy;

struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t crc;
};
static_assert(sizeof(Header) == 12, "settings header layout");
static_assert(sizeof(orbit::Settings) <= kMaxPayload, "settings payload");

uint32_t crc32(const void* p, size_t n) { return esp_rom_crc32_le(0, static_cast<const uint8_t*>(p), (uint32_t)n); }

void encode(const orbit::Settings& s, uint8_t* out, size_t* len)
{
    Header h = {kMagic, kVersion, (uint16_t)sizeof(orbit::Settings), crc32(&s, sizeof(s))};
    memcpy(out, &h, sizeof(h));
    memcpy(out + sizeof(h), &s, sizeof(s));
    *len = sizeof(h) + sizeof(s);
}

uint8_t flag(uint8_t v) { return v ? 1 : 0; }

// keep a damaged or foreign file from producing values the engine / UI cannot handle
void sanitize(orbit::Settings& s)
{
    orbit::Settings d;
    orbit::settingsDefaults(d);
    if ((uint8_t)s.output >= (uint8_t)orbit::AudioOut::Count) s.output = d.output;
    if (s.sample_rate != 44100 && s.sample_rate != 48000) s.sample_rate = d.sample_rate;
    if (s.buffer_frames != 64 && s.buffer_frames != 128 && s.buffer_frames != 256) s.buffer_frames = d.buffer_frames;
    if (s.volume > 100) s.volume = 100;
    if (s.midi_channel > 16) s.midi_channel = d.midi_channel;
    if (s.brightness < 10 || s.brightness > 100) s.brightness = d.brightness;
    if (s.knob_sensitivity < 1 || s.knob_sensitivity > 100) s.knob_sensitivity = d.knob_sensitivity;
    if ((uint8_t)s.pad_velocity >= (uint8_t)orbit::PadVelocity::Count) s.pad_velocity = d.pad_velocity;
    if (s.fixed_velocity < 1 || s.fixed_velocity > 127) s.fixed_velocity = d.fixed_velocity;
    if (s.voice_limit < 8 || s.voice_limit > 32) s.voice_limit = d.voice_limit;   // Engine::kMinVoiceLimit..kVoices
    s.autosave = flag(s.autosave);
    s.pad_audition = flag(s.pad_audition);
    s.master_limiter = flag(s.master_limiter);
    s.midi_usb = flag(s.midi_usb);
    s.midi_clock_in = flag(s.midi_clock_in);
    s.midi_clock_out = flag(s.midi_clock_out);
    s.animations = flag(s.animations);
    s.meters = flag(s.meters);
    s.low_power = flag(s.low_power);
    s.touch_feedback = flag(s.touch_feedback);
    s.haptic_indicator = flag(s.haptic_indicator);
    s.double_tap_reset = flag(s.double_tap_reset);
    s.bt_enabled = flag(s.bt_enabled);
    s.bt_local = flag(s.bt_local);
    if (s.bt_volume > 100) s.bt_volume = 100;
}

// header + payload -> settings over the defaults (a shorter payload from an older build keeps the
// defaults of the fields it does not have)
bool decode(const uint8_t* buf, size_t len, orbit::Settings& s)
{
    Header h;
    if (len < sizeof(h)) return false;
    memcpy(&h, buf, sizeof(h));
    if (h.magic != kMagic || h.version != kVersion || h.size == 0 || h.size > kMaxPayload) return false;
    if (len < sizeof(h) + h.size || crc32(buf + sizeof(h), h.size) != h.crc) return false;
    orbit::Settings out;
    orbit::settingsDefaults(out);
    memcpy(&out, buf + sizeof(h), h.size < sizeof(out) ? h.size : sizeof(out));
    sanitize(out);
    s = out;
    return true;
}

bool readFile(const char* path, orbit::Settings& s)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[sizeof(Header) + kMaxPayload];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return decode(buf, n, s);
}

bool writeNvs(const orbit::Settings& s)
{
    uint8_t buf[sizeof(Header) + sizeof(orbit::Settings)];
    size_t len = 0;
    encode(s, buf, &len);
    nvs_handle_t h;
    if (nvs_open("orbit", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "settings", buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool readNvs(orbit::Settings& s)
{
    nvs_handle_t h;
    if (nvs_open("orbit", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t buf[sizeof(Header) + kMaxPayload];
    size_t len = sizeof(buf);
    const esp_err_t err = nvs_get_blob(h, "settings", buf, &len);
    nvs_close(h);
    return err == ESP_OK && decode(buf, len, s);
}

void ensureMutex()
{
    if (!s_nvs_mutex) s_nvs_mutex = xSemaphoreCreateMutex();
}

const char* mountError(esp_err_t err)
{
    switch (err) {
    case ESP_FAIL: return "no FAT32/exFAT file system";
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_NOT_FOUND: return "no card";
    default: return esp_err_to_name(err);
    }
}
}  // namespace

namespace detail {
bool audioIdleNow() { return __atomic_load_n(&s_idle, __ATOMIC_RELAXED) || !audioRunning(); }

bool settingsFlushDeferred()
{
    ensureMutex();
    if (!__atomic_load_n(&s_nvs_pending, __ATOMIC_ACQUIRE)) return true;
    bool ok = true;
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    if (s_nvs_pending) {
        orbit::Settings copy = s_nvs_copy;
        s_nvs_pending = false;   // one attempt per save: a broken NVS must not write flash every 200 ms
        ok = writeNvs(copy);
        if (!ok) ESP_LOGW(TAG, "NVS settings mirror not written");
    }
    xSemaphoreGive(s_nvs_mutex);
    return ok;
}
}  // namespace detail

// ---------------------------------------------------------------- SD card
bool sdMount()
{
    if (s_mounted) return true;
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;   // never format a user's card
    mount.max_files = 12;
    mount.allocation_unit_size = 16 * 1024;
    sdmmc_slot_config_t slot = {};
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        sdmmc_host_t host = {};
        bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);   // 4-bit high speed (40 MHz)
        bsp_sdcard_cfg_t cfg = {};
        cfg.mount = &mount;
        cfg.host = &host;
        cfg.slot.sdmmc = &slot;
        err = bsp_sdcard_sdmmc_mount(&cfg);
        if (err == ESP_OK) break;
        // release the SD LDO channel the BSP acquired, otherwise the next attempt cannot get it
        bsp_sdcard_unmount();
        ESP_LOGW(TAG, "SD %d/3: %s (%s)", attempt + 1, esp_err_to_name(err), mountError(err));
        if (err == ESP_FAIL) break;   // a card answered but has no FAT/exFAT volume: retrying will not help
        vTaskDelay(pdMS_TO_TICKS(150 * (attempt + 1)));
    }
    if (err != ESP_OK) return false;
    s_mounted = true;
    sdmmc_card_t* card = bsp_sdcard_get_handle();
    if (card) {
        ESP_LOGI(TAG, "SD %s, %llu MB, %d kHz", card->cid.name,
                 (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024), card->max_freq_khz);
    }
    return true;
}

bool sdMounted() { return s_mounted; }

const char* sdRoot() { return kRoot; }

// ---------------------------------------------------------------- settings
bool settingsLoad(orbit::Settings& s)
{
    if (s_mounted && (readFile(kFile, s) || readFile(kTmp, s))) return true;
    if (readNvs(s)) return true;
    orbit::settingsDefaults(s);
    return false;
}

bool settingsSave(const orbit::Settings& s)
{
    ensureMutex();
    bool sd_ok = false;
    if (s_mounted) {
        mkdir(kDir, 0775);   // EEXIST is fine
        uint8_t buf[sizeof(Header) + sizeof(orbit::Settings)];
        size_t len = 0;
        encode(s, buf, &len);
        FILE* f = fopen(kTmp, "wb");
        if (f) {
            sd_ok = fwrite(buf, 1, len, f) == len;
            sd_ok = (fclose(f) == 0) && sd_ok;
        }
        if (sd_ok) {
            unlink(kFile);   // FatFs rename does not replace
            sd_ok = rename(kTmp, kFile) == 0;
        }
        if (!sd_ok) ESP_LOGW(TAG, "settings.bin not written (errno %d)", errno);
    }
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    s_nvs_copy = s;
    __atomic_store_n(&s_nvs_pending, true, __ATOMIC_RELEASE);
    xSemaphoreGive(s_nvs_mutex);
    // silent now: write the mirror at once, else the aux task writes it at the next idle moment
    const bool nvs_ok = detail::audioIdleNow() ? detail::settingsFlushDeferred() : true;
    return sd_ok || nvs_ok;
}

void setAudioIdle(bool idle) { __atomic_store_n(&s_idle, idle, __ATOMIC_RELAXED); }

}  // namespace hal

// Free / total bytes of the volume holding `path` (core/library.cpp uses it for STORAGE in Settings).
extern "C" bool orbit_fs_info(const char* path, uint64_t* total, uint64_t* free_bytes)
{
    if (!path || !total || !free_bytes) return false;
    char base[32];
    // mount point = first path component ("/sdcard/ORBIT/..." -> "/sdcard")
    const char* end = path[0] == '/' ? strchr(path + 1, '/') : nullptr;
    size_t n = end ? (size_t)(end - path) : strlen(path);
    if (n == 0 || n >= sizeof(base)) return false;
    memcpy(base, path, n);
    base[n] = 0;
    return esp_vfs_fat_info(base, total, free_bytes) == ESP_OK;
}
