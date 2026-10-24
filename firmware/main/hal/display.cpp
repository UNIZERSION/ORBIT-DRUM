// Tear-free landscape presentation for the Tab5 MIPI-DSI panel (native 720x1280 portrait).
//
// LVGL renders the logical 1280x720 screen in pieces (PARTIAL mode) into two internal-SRAM draw buffers
// (1280x32, reserved first thing at boot by displayReserveBuffers(), so the size never depends on how
// fragmented the heap is by then; PSRAM 1280x96 only when that reservation failed).
// A presentation task rotates every piece of one LVGL refresh (PPA SRM, 90 degrees) into a HIDDEN DPI frame
// buffer and publishes the buffer once, after the last piece, with esp_lcd_panel_draw_bitmap() on the
// panel's own buffer (no copy: the DPI DMA switches to it at the next frame start). Three DPI buffers:
// LVGL renders into a free one while the previous frame waits for scan-out. A buffer that was not the back
// buffer for some frames misses their changes ("stale" areas); those areas are copied from the newest
// frame while the task is idle between refreshes, and whatever is left before reuse, except what the
// coming refresh redraws anyway (LVGL's invalid areas). The bookkeeping is hal/hal_logic.h FrameBook
// (unit-tested on the PC).
// The PPA rotates piece N while LVGL renders piece N+1 (flush_cb queues, flush_wait_cb waits).
// When the PPA is unavailable or fails, the same operations run on the CPU (slower, still tear-free).
// The backlight comes on with the first frame of the real UI (displayReveal(), armed by app_main after the
// UI is built; 3 s after the start without it as a fallback).
// Algorithm after the user's Tab5 launcher (esp/base/main/l5_display.c), rewritten for ORBIT DRUM.
#include <string.h>
#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/axi_icm_ll.h"
#include "lvgl.h"
#include "src/display/lv_display_private.h"   // inv_areas: what the running refresh redraws (LVGL 9.3)
#include "hal/hal.h"
#include "hal/hal_logic.h"
#include "hal/hal_private.h"

namespace hal {

namespace {
const char* TAG = "orbit_display";
using logic::Rect;

constexpr int kNativeW = 720;
constexpr int kNativeH = 1280;
constexpr size_t kFrameBytes = (size_t)kNativeW * kNativeH * 2;
constexpr int kMaxDamage = 64;
constexpr int kMaxCover = 8;
constexpr int kMaxRest = 24;
constexpr int64_t kBackBufferTimeoutUs = 150000;
constexpr int kSramRows = 32;                    // 2 x 80 KB internal SRAM (reserved at boot)
constexpr int kPsramRows = 96;                   // fallback when the reservation failed
constexpr uint32_t kIdleRepairPx = 96 * 1024;    // pixels copied per idle step (~ 133 native rows)
constexpr int kIdlePollMs = 10;                  // presentation task poll while nothing is queued
constexpr int64_t kRevealFallbackUs = 3000000;   // backlight on without displayReveal() after this
constexpr int64_t kTouchSettleUs = 500000;       // touch / TDDI power-up before the ST712x probe

esp_lcd_panel_handle_t s_panel = nullptr;
lv_display_t* s_disp = nullptr;
bool s_port_ready = false;
ppa_client_handle_t s_ppa = nullptr;
bool s_use_ppa = false;
uint8_t* s_frames[3] = {};
logic::FrameBook<kMaxDamage, kMaxRest> s_book;   // presentation task only (after displayStart)
uint32_t s_published_seq = 0;
int64_t s_published_at = 0;
uint32_t s_presents = 0;
// bring-up diagnostics (displayDebug): where the presentation task and the LVGL flush path are
int s_stage = 0;                   // __atomic, presentation: 1 queue wait, 2 beginFrame, 3 rotate, 4 present, 5 idle repair
int s_lv_stage = 0;                // __atomic, LVGL: 10 wait for done, 11 queued job, 12 flush returned
uint32_t s_jobs = 0;               // __atomic
uint32_t s_ppa_timeouts = 0;       // __atomic
TaskHandle_t s_present_task = nullptr;
SemaphoreHandle_t s_ppa_done = nullptr;
// The panel is brought up by a background task (probe + BSP init are ~1.9 s of fixed delays), while app_main
// builds the App and the UI. The first flush waits for one of these bits.
EventGroupHandle_t s_panel_events = nullptr;
constexpr EventBits_t kPanelReady = BIT0;
constexpr EventBits_t kPanelFailed = BIT1;
bool s_panel_ok = false;   // LVGL task: kPanelReady seen
inline void stage(int* v, int s) { __atomic_store_n(v, s, __ATOMIC_RELAXED); }
int64_t s_started_at = 0;                         // displayStart finished (reveal fallback)

// LVGL draw buffers reserved by displayReserveBuffers()
void* s_resv[2] = {};
int s_resv_rows = 0;

SemaphoreHandle_t s_vsync = nullptr;
volatile uint32_t s_vsync_seq = 0;

struct Job {
    lv_area_t area;
    const uint8_t* pixels;
    bool last;
    uint8_t ncover;
    uint32_t refr;               // LVGL refresh the piece belongs to (s_refr_seq)
    Rect cover[kMaxCover];
};
QueueHandle_t s_queue = nullptr;
SemaphoreHandle_t s_done = nullptr;
bool s_transfer_pending = false;   // LVGL task only
bool s_frame_first = false;        // LVGL task only
uint32_t s_refr_seq = 0;           // LVGL task only: refreshes started
TaskHandle_t s_lvgl_task = nullptr;   // __atomic: the LVGL port task (known from its first refresh)

// backlight: applied with the first complete frame of the real UI
volatile int s_brightness = -1;    // requested (-1: not set yet)
volatile bool s_revealed = false;
uint32_t s_reveal_from = 0;        // __atomic: first refresh that may light the backlight, 0 = not armed
int s_frame_cap = 60;

// diagnostics: worst cases, written by the presentation task (__atomic)
uint32_t s_rotate_max_us = 0;
uint32_t s_repair_max_us = 0;
uint32_t s_publish_max_us = 0;

// panel probe
bool s_st712x = false;
uint8_t s_touch_fw = 0;
bool s_probed = false;

void noteMax(uint32_t* slot, int64_t us)
{
    const uint32_t v = us < 0 ? 0u : us > 0x7FFFFFFF ? 0x7FFFFFFFu : (uint32_t)us;
    if (v > __atomic_load_n(slot, __ATOMIC_RELAXED)) __atomic_store_n(slot, v, __ATOMIC_RELAXED);
}

bool IRAM_ATTR onRefreshDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*)
{
    __atomic_add_fetch(&s_vsync_seq, 1, __ATOMIC_RELEASE);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &wake);
    return wake == pdTRUE;
}

uint32_t vsyncSeq() { return __atomic_load_n(&s_vsync_seq, __ATOMIC_ACQUIRE); }

// esp_restart() runs this first: without DSI video the panel shows noise until the next app sets it up
void blankOnRestart()
{
    bsp_display_brightness_set(0);
    if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);
    esp_rom_delay_us(40 * 1000);   // DISPOFF takes effect with the next frame
}

// logical 1280x720 area -> native 720x1280 area (LV_DISPLAY_ROTATION_90)
Rect nativeArea(const lv_area_t& a) { return logic::toNative(Rect{a.x1, a.y1, a.x2, a.y2}, kNativeH); }

// rows y1..y2 of a frame buffer written by the CPU -> memory (the DPI DMA reads memory)
void syncRows(uint8_t* fb, int y1, int y2)
{
    uint8_t* start = fb + (size_t)y1 * kNativeW * 2;
    esp_cache_msync(start, (size_t)(y2 - y1 + 1) * kNativeW * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

bool IRAM_ATTR onPpaDone(ppa_client_handle_t, ppa_event_data_t*, void*)
{
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(s_ppa_done, &wake);
    return wake == pdTRUE;
}

// ESP32-P4 PPA SRM can stall forever on some rotated RGB565 blocks while the DPI frame is scanned out
// (esp-idf #19096 / #19023, fixed only on IDF master). Destinations are 4-pixel aligned to avoid it (see
// roundArea); as a last line of defence the transaction runs non-blocking with a timeout, so a stall can never
// freeze the UI: the caller falls back to the CPU (a stalled PPA stays unusable until reset).
esp_err_t ppaRun(ppa_srm_oper_config_t& op)
{
    op.mode = PPA_TRANS_MODE_NON_BLOCKING;
    (void)xSemaphoreTake(s_ppa_done, 0);
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_ppa_done, pdMS_TO_TICKS(80)) != pdTRUE) {   // a 1280x32 piece takes ~2 ms
        __atomic_add_fetch(&s_ppa_timeouts, 1, __ATOMIC_RELAXED);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void switchToCpu(const char* why, esp_err_t err)
{
    if (!s_use_ppa) return;
    s_use_ppa = false;
    ESP_LOGW(TAG, "PPA %s failed (%s): CPU rotation from now on", why, esp_err_to_name(err));
    // the CPU reads frames the PPA wrote: drop any cached copies first
    for (int i = 0; i < 3; i++) esp_cache_msync(s_frames[i], kFrameBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

void cpuRotate(const Job& job)
{
    const lv_area_t& a = job.area;
    const int w = a.x2 - a.x1 + 1;
    const uint16_t* src = reinterpret_cast<const uint16_t*>(job.pixels);
    uint16_t* dst = reinterpret_cast<uint16_t*>(s_frames[s_book.back]);
    for (int lx = a.x1; lx <= a.x2; ++lx) {
        uint16_t* row = dst + (size_t)(kNativeH - 1 - lx) * kNativeW;
        const uint16_t* s = src + (lx - a.x1);
        for (int ly = a.y1; ly <= a.y2; ++ly) row[ly] = s[(size_t)(ly - a.y1) * w];
    }
    syncRows(s_frames[s_book.back], kNativeH - 1 - a.x2, kNativeH - 1 - a.x1);
}

void rotatePiece(const Job& job)
{
    const int64_t t0 = esp_timer_get_time();
    const Rect native = nativeArea(job.area);
    if (s_use_ppa) {
        ppa_srm_oper_config_t op = {};
        op.in.buffer = job.pixels;
        op.in.pic_w = (uint32_t)(job.area.x2 - job.area.x1 + 1);
        op.in.pic_h = (uint32_t)(job.area.y2 - job.area.y1 + 1);
        op.in.block_w = op.in.pic_w;
        op.in.block_h = op.in.pic_h;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = s_frames[s_book.back];
        op.out.buffer_size = kFrameBytes;
        op.out.pic_w = kNativeW;
        op.out.pic_h = kNativeH;
        op.out.block_offset_x = (uint32_t)native.x1;
        op.out.block_offset_y = (uint32_t)native.y1;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
        op.scale_x = 1.0f;
        op.scale_y = 1.0f;
        const esp_err_t err = ppaRun(op);
        if (err != ESP_OK) switchToCpu("rotation", err);
    }
    if (!s_use_ppa) cpuRotate(job);
    s_book.damaged(native);
    noteMax(&s_rotate_max_us, esp_timer_get_time() - t0);
}

// copy an area the candidate buffer missed from the newest frame
void repairPiece(unsigned candidate, const Rect& stale)
{
    // 4-pixel aligned columns (esp-idf #19096); the extra columns come from the newest frame, which is harmless
    Rect r = stale;
    r.x1 &= ~3;
    r.x2 |= 3;
    if (r.x2 > kNativeW - 1) r.x2 = kNativeW - 1;
    if (s_use_ppa) {
        ppa_srm_oper_config_t op = {};
        op.in.buffer = s_frames[s_book.latest];
        op.in.pic_w = kNativeW;
        op.in.pic_h = kNativeH;
        op.in.block_w = (uint32_t)logic::rectW(r);
        op.in.block_h = (uint32_t)logic::rectH(r);
        op.in.block_offset_x = (uint32_t)r.x1;
        op.in.block_offset_y = (uint32_t)r.y1;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = s_frames[candidate];
        op.out.buffer_size = kFrameBytes;
        op.out.pic_w = kNativeW;
        op.out.pic_h = kNativeH;
        op.out.block_offset_x = (uint32_t)r.x1;
        op.out.block_offset_y = (uint32_t)r.y1;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        op.scale_x = 1.0f;
        op.scale_y = 1.0f;
        const esp_err_t err = ppaRun(op);
        if (err == ESP_OK) return;
        switchToCpu("repair", err);
    }
    const size_t row_bytes = (size_t)logic::rectW(r) * 2;
    for (int y = r.y1; y <= r.y2; ++y) {
        const size_t off = ((size_t)y * kNativeW + r.x1) * 2;
        memcpy(s_frames[candidate] + off, s_frames[s_book.latest] + off, row_bytes);
    }
    syncRows(s_frames[candidate], r.y1, r.y2);
}

void repairInto(int dst, const Rect& r) { repairPiece((unsigned)dst, r); }

// First piece of a refresh: pick a back buffer that is no longer scanned (2 VSYNC edges after it was
// replaced: IDF 5.5 handles VSYNC and the DMA restart in separate interrupts) and bring it up to date,
// leaving out what this refresh redraws (job.cover). Usually the idle repairs already did most of it.
void beginFrame(const Job& job)
{
    if (!s_book.pending) return;
    int candidate = -1;
    for (;;) {
        candidate = s_book.candidate(vsyncSeq());
        if (candidate >= 0) break;
        if (esp_timer_get_time() - s_published_at >= kBackBufferTimeoutUs) {
            // no VSYNC: never hang LVGL, take the buffer retired first (may tear once)
            candidate = s_book.oldestRetired();
            break;
        }
        (void)xSemaphoreTake(s_vsync, pdMS_TO_TICKS(5));
    }
    if (candidate < 0) candidate = (int)((s_book.latest + 1) % 3);
    const int64_t t0 = esp_timer_get_time();
    const Rect piece = nativeArea(job.area);
    const Rect* cover = job.ncover ? job.cover : &piece;
    const int ncover = job.ncover ? job.ncover : 1;
    s_book.prepare((unsigned)candidate, cover, ncover, repairInto);
    noteMax(&s_repair_max_us, esp_timer_get_time() - t0);
}

void reveal()
{
    // the frame is published: wait until it is on the glass, then light the backlight
    const int64_t t0 = esp_timer_get_time();
    while ((int32_t)(vsyncSeq() - s_published_seq) < 2 && esp_timer_get_time() - t0 < 150000) {
        (void)xSemaphoreTake(s_vsync, pdMS_TO_TICKS(5));
    }
    s_revealed = true;
    const int b = s_brightness;
    bsp_display_brightness_set(b >= 0 ? b : 80);
}

// displayReveal() was never called (UI build failed or stuck): light whatever is on the panel
void revealFallback()
{
    if (s_revealed || !s_presents || esp_timer_get_time() - s_started_at < kRevealFallbackUs) return;
    ESP_LOGW(TAG, "backlight on without displayReveal()");
    reveal();
}

void present(const Job& job)
{
    const int64_t t0 = esp_timer_get_time();
    // The DPI driver selects the frame by its address and flips to it at the next frame start. Its cache
    // write-back covers rows [y_start, y_end): one row here, because the frame already is in memory (the
    // PPA writes it by DMA, the CPU paths sync their rows). A range would mean a write-back of up to 1.8 MB
    // under the global cache-sync spinlock, which i2s_channel_write on the audio core spins on.
    const esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, kNativeW, 1, s_frames[s_book.back]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "publish: %s", esp_err_to_name(err));
        return;   // keep drawing into the same back buffer
    }
    s_published_seq = vsyncSeq();
    s_published_at = esp_timer_get_time();
    // the old frame may have started scanning right before the switch: it is held for two VSYNCs
    s_book.published(s_published_seq);
    ++s_presents;
    noteMax(&s_publish_max_us, esp_timer_get_time() - t0);
    if (!s_revealed) {
        const uint32_t from = __atomic_load_n(&s_reveal_from, __ATOMIC_ACQUIRE);
        if (from && (int32_t)(job.refr - from) >= 0) reveal();
        else revealFallback();
    }
}

void presentationTask(void*)
{
    static Job job;   // ~190 bytes, off the stack
    for (;;) {
        // Between refreshes the stale areas of a writable buffer are repaired in small steps, so the next
        // refresh rarely waits for a big copy. In PPA mode the step sleeps on the PPA; the CPU copy yields
        // one tick between steps so the LVGL task (lower priority) keeps rendering.
        const bool idle_work = s_book.idleTarget(vsyncSeq()) >= 0;
        const TickType_t wait = idle_work ? (s_use_ppa ? 0 : 1) : pdMS_TO_TICKS(kIdlePollMs);
        stage(&s_stage, 1);
        if (xQueueReceive(s_queue, &job, wait) == pdTRUE) {
            stage(&s_stage, 2);
            beginFrame(job);
            stage(&s_stage, 3);
            rotatePiece(job);
            stage(&s_stage, 4);
            if (job.last) present(job);
            __atomic_add_fetch(&s_jobs, 1, __ATOMIC_RELAXED);
            xSemaphoreGive(s_done);
            continue;
        }
        revealFallback();
        stage(&s_stage, 5);
        if (idle_work) s_book.idleStep(vsyncSeq(), kIdleRepairPx, repairInto);
    }
}

void waitForFlush(lv_display_t*)
{
    if (s_transfer_pending) {
        // LVGL calls this after rendering into its OTHER buffer: CPU and PPA overlap
        stage(&s_lv_stage, 10);
        xSemaphoreTake(s_done, portMAX_DELAY);
        s_transfer_pending = false;
    }
}

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* pixels)
{
    if (!s_panel_ok) {
        const EventBits_t b = xEventGroupWaitBits(s_panel_events, kPanelReady | kPanelFailed, pdFALSE, pdFALSE,
                                                  portMAX_DELAY);
        if (!(b & kPanelReady)) {   // no panel: drop the frame, keep LVGL running
            lv_display_flush_ready(disp);
            return;
        }
        s_panel_ok = true;
    }
    waitForFlush(disp);
    static Job job;
    job.area = *area;
    job.pixels = pixels;
    job.last = lv_display_flush_is_last(disp);
    job.ncover = 0;
    job.refr = s_refr_seq;
    if (s_frame_first) {   // the areas this refresh redraws, each rendered in full
        s_frame_first = false;
        for (uint32_t i = 0; i < disp->inv_p; i++) {
            if (disp->inv_area_joined[i]) continue;
            if (job.ncover == kMaxCover) {
                job.ncover = 0;   // too many: leave out only this piece
                break;
            }
            job.cover[job.ncover++] = nativeArea(disp->inv_areas[i]);
        }
    }
    s_transfer_pending = true;
    stage(&s_lv_stage, 11);
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {   // cannot happen: one job in flight at most
        s_transfer_pending = false;
        lv_display_flush_ready(disp);
    }
    stage(&s_lv_stage, 12);
    // no lv_display_flush_ready(): LVGL 9.3 clears `flushing` after flush_wait_cb returns
}

// Rounds every invalidated (logical) area to 4-row boundaries: logical y becomes the PPA destination x of the
// 90-degree rotation, and x % 4 == 2 can stall the PPA (esp-idf #19096). LVGL also sends this event to round
// the height of its partial-render chunks, so every flushed piece starts and ends on a multiple of 4.
void roundArea(lv_event_t* e)
{
    lv_area_t* a = static_cast<lv_area_t*>(lv_event_get_param(e));
    if (!a) return;
    a->y1 &= ~3;
    a->y2 |= 3;
    if (a->y2 > kNativeW - 1) a->y2 = kNativeW - 1;
}

// Runs in the LVGL port task, which is awake by definition: no wake-up (that would only make its next
// wait return at once). Remembers the task so wakeDisplay() can tell its own invalidations apart.
void refrStart(lv_event_t*)
{
    s_frame_first = true;
    ++s_refr_seq;
    __atomic_store_n(&s_lvgl_task, xTaskGetCurrentTaskHandle(), __ATOMIC_RELAXED);
}

// The display is not registered through lvgl_port_add_disp_*, so the port task is woken here for
// invalidations made by other tasks (under hal::lvLock). The port task's own invalidations need none: its
// next sleep is computed from the refresh timer they resume.
void wakeDisplay(lv_event_t*)
{
    if (xTaskGetCurrentTaskHandle() == __atomic_load_n(&s_lvgl_task, __ATOMIC_RELAXED)) return;
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, nullptr);
}

// The DPI frame buffer reader (DW-GDMA master port 1) must win against bursty PSRAM traffic (redraws,
// SD reads), otherwise a missed scan-out deadline flashes the panel blue.
void prioritizeScanout()
{
    const uint32_t write_prio = AXI_ICM.mst_awqos_reg0.reg_gdma_mst2_awqos;
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, write_prio, 15);
}

// BSP 1.3.2 keeps its board detection private and runs the ST7121 at 1000 Mbps; the ST7121 needs 965.
// Same probe: ST712x touch at 0x55, firmware byte at register 0 (1 = ST7121, 3 = ST7123). The touch is
// normally powered since boardInit, so only what is left of its 500 ms settle time is waited here.
uint32_t probeLaneRate()
{
    s_probed = true;
    int64_t on_at = detail::touchPowerOnUs();
    if (!on_at) {
        if (bsp_feature_enable(BSP_FEATURE_TOUCH, true) != ESP_OK) return 1000;
        on_at = esp_timer_get_time();
    }
    const int64_t left = kTouchSettleUs - (esp_timer_get_time() - on_at);
    if (left > 0) vTaskDelay(pdMS_TO_TICKS((uint32_t)((left + 999) / 1000)));
    if (i2c_master_probe(bsp_i2c_get_handle(), 0x55, 100) != ESP_OK) return 1000;
    s_st712x = true;
    i2c_master_dev_handle_t dev = nullptr;
    if (detail::i2cAdd(0x55, &dev) != ESP_OK) return 1000;
    const uint8_t reg[2] = {0, 0};
    uint8_t fw = 0;
    const esp_err_t err = i2c_master_transmit_receive(dev, reg, 2, &fw, 1, 100);
    i2c_master_bus_rm_device(dev);
    if (err == ESP_OK) s_touch_fw = fw;
    return err == ESP_OK && fw == 1 ? 965 : 1000;
}

// The panel type found by the last probe, kept in NVS so later boots skip the probe's touch settle wait
// (~0.5 s: the BSP waits for the same settle time again inside bsp_display_new). 1 = ST7121, 3 = ST7123,
// 0xFE = no ST712x (ILI9881C + GT911), 0 = unknown.
constexpr const char* kHwNs = "orbit_hw";
uint8_t panelCode() { return s_st712x ? s_touch_fw : (uint8_t)0xFE; }

uint8_t cachedPanel()
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(kHwNs, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "panel", &v) != ESP_OK) v = 0;
        nvs_close(h);
    }
    return (v == 1 || v == 3 || v == 0xFE) ? v : 0;
}

void storePanel(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(kHwNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "panel", v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

bool lcdPower()
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (detail::i2cReady()) {
            // the cached 0x43 handle of boardInit; a failed boardInit attempt is retried once here
            err = detail::ioe1Open() ? bsp_feature_enable(BSP_FEATURE_LCD, true) : ESP_FAIL;
            if (err == ESP_OK) return true;
            if (attempt == 0) i2c_master_bus_reset(bsp_i2c_get_handle());   // the bus can hang at power-up
        }
        ESP_LOGW(TAG, "LCD power %d/3: %s", attempt + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(50 * (attempt + 1)));
    }
    return false;
}

void* allocDraw(size_t bytes, bool internal)
{
    const uint32_t caps = internal ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) : (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    return heap_caps_aligned_calloc(64, 1, bytes, caps);
}

void applyFrameCap()
{
    lv_timer_t* t = s_disp ? lv_display_get_refr_timer(s_disp) : nullptr;
    if (t) lv_timer_set_period(t, s_frame_cap <= 30 ? 33 : 16);
}
}  // namespace

namespace detail {
const char* panelName()
{
    if (!s_probed) return "?";
    if (!s_st712x) return "ILI9881C";
    return s_touch_fw == 1 ? "ST7121" : s_touch_fw == 3 ? "ST7123" : "ST712x";
}

int displayDebug(char* buf, size_t n)
{
    static const char st[] = "RrBSDX";   // eTaskState: Running, ready, Blocked, Suspended, Deleted, invalid
    TaskHandle_t lv = __atomic_load_n(&s_lvgl_task, __ATOMIC_RELAXED);
    const int ls = lv ? (int)eTaskGetState(lv) : 5;
    const int ps = s_present_task ? (int)eTaskGetState(s_present_task) : 5;
    return snprintf(buf, n, "present st %d task %c | lvgl st %d task %c | jobs %lu presents %lu refr %lu vsync %lu "
                    "pending %d ppa %d (timeouts %lu) idle %d revealed %d",
                    __atomic_load_n(&s_stage, __ATOMIC_RELAXED), st[ps > 5 ? 5 : ps],
                    __atomic_load_n(&s_lv_stage, __ATOMIC_RELAXED), st[ls > 5 ? 5 : ls],
                    (unsigned long)__atomic_load_n(&s_jobs, __ATOMIC_RELAXED),
                    (unsigned long)s_presents, (unsigned long)s_refr_seq, (unsigned long)vsyncSeq(),
                    s_transfer_pending ? 1 : 0, s_use_ppa ? 1 : 0,
                    (unsigned long)__atomic_load_n(&s_ppa_timeouts, __ATOMIC_RELAXED), s_book.idleTarget(vsyncSeq()),
                    s_revealed ? 1 : 0);
}

void displayDiag(Diagnostics* out, bool reset)
{
    if (out) {
        out->rotate_max_us = __atomic_load_n(&s_rotate_max_us, __ATOMIC_RELAXED);
        out->repair_max_us = __atomic_load_n(&s_repair_max_us, __ATOMIC_RELAXED);
        out->publish_max_us = __atomic_load_n(&s_publish_max_us, __ATOMIC_RELAXED);
        out->draw_rows = (uint16_t)(s_disp ? (s_resv_rows ? s_resv_rows : kPsramRows) : 0);
        out->draw_sram = s_resv_rows ? 1 : 0;
    }
    if (reset) {
        __atomic_store_n(&s_rotate_max_us, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&s_repair_max_us, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&s_publish_max_us, 0u, __ATOMIC_RELAXED);
    }
}
}  // namespace detail

// ---------------------------------------------------------------- public
bool displayReserveBuffers()
{
    if (s_resv_rows) return true;
    const size_t bytes = (size_t)1280 * kSramRows * 2;
    void* a = allocDraw(bytes, true);
    void* b = allocDraw(bytes, true);
    if (!a || !b) {
        heap_caps_free(a);
        heap_caps_free(b);
        ESP_LOGW(TAG, "no internal SRAM for 2 x 1280x%d draw buffers", kSramRows);
        return false;
    }
    s_resv[0] = a;
    s_resv[1] = b;
    s_resv_rows = kSramRows;
    return true;
}

namespace {
// Background half of displayStart(): panel probe, BSP panel init (fixed delays), frame buffers, PPA, VSYNC,
// presentation task, touch. Core 0 (its interrupts must stay off the audio core), above the main task.
void panelTask(void*)
{
    int64_t t_step = esp_timer_get_time();
    auto mark = [&](const char* what) {   // boot profile
        const int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG, "panel %-12s %5lld ms", what, (long long)((now - t_step) / 1000));
        t_step = now;
    };
    bool ok = false;
    do {
        bsp_display_config_t cfg = {};
        const uint8_t cached = cachedPanel();
        if (cached) {   // known panel: no settle wait here (bsp_display_new waits for the touch anyway)
            s_probed = true;
            s_st712x = cached != 0xFE;
            s_touch_fw = s_st712x ? cached : 0;
            cfg.dsi_bus.lane_bit_rate_mbps = cached == 1 ? 965 : 1000;
        } else {
            cfg.dsi_bus.lane_bit_rate_mbps = probeLaneRate();
            if (s_st712x ? s_touch_fw != 0 : true) storePanel(panelCode());   // first boot (audio not running yet)
        }
        mark(cached ? "type cached" : "probe");
        prioritizeScanout();
        esp_lcd_panel_io_handle_t io = nullptr;
        esp_err_t err = bsp_display_new(&cfg, &s_panel, &io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel: %s", esp_err_to_name(err));
            break;
        }
        mark("init");
        if (cached) {
            // The touch has settled now (the BSP waited): confirm the cached type without waiting. A changed
            // panel takes effect from the next boot (this boot may run the old lane rate once).
            const uint8_t was = cached;
            probeLaneRate();
            if ((s_st712x ? s_touch_fw != 0 : true) && panelCode() != was) {
                ESP_LOGW(TAG, "panel type changed (%u -> %u): the lane rate follows on the next start", was, panelCode());
                storePanel(panelCode());
            }
        }
        err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 3, (void**)&s_frames[0], (void**)&s_frames[1],
                                                 (void**)&s_frames[2]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "3 DPI frame buffers (CONFIG_BSP_LCD_DPI_BUFFER_NUMS=3): %s", esp_err_to_name(err));
            break;
        }
        ppa_client_config_t ppa_cfg = {};
        ppa_cfg.oper_type = PPA_OPERATION_SRM;
        s_use_ppa = ppa_register_client(&ppa_cfg, &s_ppa) == ESP_OK;
        if (s_use_ppa) {
            ppa_event_callbacks_t pcb = {};
            pcb.on_trans_done = onPpaDone;
            if (ppa_client_register_event_callbacks(s_ppa, &pcb) != ESP_OK) s_use_ppa = false;
        }
        if (!s_use_ppa) ESP_LOGW(TAG, "no PPA client: CPU rotation");
        esp_lcd_dpi_panel_event_callbacks_t cbs = {};
        cbs.on_refresh_done = onRefreshDone;
        err = esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, nullptr);
        if (err != ESP_OK) ESP_LOGW(TAG, "VSYNC callback: %s (frames published without pacing)", esp_err_to_name(err));
        s_book.reset(kNativeW, kNativeH);
        if (xTaskCreatePinnedToCore(presentationTask, "orbit_present", 4096, nullptr, 5, &s_present_task, 0) != pdPASS) {
            ESP_LOGE(TAG, "presentation task");
            break;
        }
        esp_lcd_panel_disp_on_off(s_panel, true);
        if (esp_register_shutdown_handler(blankOnRestart) != ESP_OK) ESP_LOGW(TAG, "no shutdown handler");
        ok = true;
    } while (false);
    s_started_at = esp_timer_get_time();   // the reveal fallback counts from a live panel
    // Ready before the touch: touchStart() takes the LVGL lock, which the LVGL task may hold while its first
    // flush waits for this bit.
    xEventGroupSetBits(s_panel_events, ok ? kPanelReady : kPanelFailed);
    if (ok) {
        if (!detail::touchStart(s_disp)) ESP_LOGW(TAG, "touch not available");
        mark("touch");
        ESP_LOGI(TAG, "landscape 1280x720 tear-free: %s, panel %s, draw buffers 1280x%d %s", s_use_ppa ? "PPA" : "CPU",
                 detail::panelName(), s_resv_rows ? s_resv_rows : kPsramRows, s_resv_rows ? "SRAM" : "PSRAM");
    }
    vTaskDelete(nullptr);
}
}  // namespace

lv_display_t* displayStart()
{
    if (s_disp) return s_disp;
    if (!lcdPower()) {
        ESP_LOGE(TAG, "LCD power failed");
        return nullptr;
    }
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = 4;
    port.task_stack = 16384;
    port.task_affinity = 0;
    port.task_max_sleep_ms = 50;
    port.timer_period_ms = 5;
    esp_err_t err = lvgl_port_init(&port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port: %s", esp_err_to_name(err));
        return nullptr;
    }
    s_port_ready = true;

    s_vsync = xSemaphoreCreateBinary();
    s_queue = xQueueCreate(1, sizeof(Job));
    s_done = xSemaphoreCreateBinary();
    s_ppa_done = xSemaphoreCreateBinary();
    s_panel_events = xEventGroupCreate();
    if (!s_vsync || !s_queue || !s_done || !s_ppa_done || !s_panel_events) return nullptr;

    // LVGL draw buffers: the internal-SRAM pair reserved at boot (fast CPU rendering and PPA reads), else
    // a late reservation attempt, else PSRAM
    if (!s_resv_rows) displayReserveBuffers();
    int rows = s_resv_rows;
    void* draw0 = s_resv[0];
    void* draw1 = s_resv[1];
    if (!rows) {
        rows = kPsramRows;
        draw0 = allocDraw((size_t)1280 * rows * 2, false);
        draw1 = allocDraw((size_t)1280 * rows * 2, false);
        if (!draw0 || !draw1) {
            ESP_LOGE(TAG, "no memory for the draw buffers");
            return nullptr;
        }
        ESP_LOGW(TAG, "draw buffers in PSRAM");
    }

    if (!lvgl_port_lock(1000)) return nullptr;
    lv_display_t* d = lv_display_create(kNativeW, kNativeH);
    if (d) {
        lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(d, draw0, draw1, (uint32_t)(1280 * rows * 2), LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(d, flushCb);
        lv_display_set_flush_wait_cb(d, waitForFlush);
        lv_display_set_rotation(d, LV_DISPLAY_ROTATION_90);
        lv_display_add_event_cb(d, roundArea, LV_EVENT_INVALIDATE_AREA, nullptr);   // first: rounds the area
        lv_display_add_event_cb(d, wakeDisplay, LV_EVENT_INVALIDATE_AREA, nullptr);
        lv_display_add_event_cb(d, wakeDisplay, LV_EVENT_REFR_REQUEST, nullptr);
        lv_display_add_event_cb(d, refrStart, LV_EVENT_REFR_START, nullptr);
        s_disp = d;
        applyFrameCap();
        s_started_at = esp_timer_get_time();
        // No refresh before the UI exists: the first flush waits for the panel while holding the LVGL lock,
        // which would stall the UI build. displayReveal() (after the build) resumes it.
        if (lv_timer_t* t = lv_display_get_refr_timer(d)) lv_timer_pause(t);
    }
    lvgl_port_unlock();
    if (!d) return nullptr;

    // The panel comes up in the background; the UI can be built meanwhile (the first flush waits for it).
    if (xTaskCreatePinnedToCore(panelTask, "orbit_panel", 6144, nullptr, 5, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "panel task");
        xEventGroupSetBits(s_panel_events, kPanelFailed);
    }
    return d;
}

bool displayWaitReady(uint32_t timeout_ms)
{
    if (!s_panel_events) return false;
    const EventBits_t b = xEventGroupWaitBits(s_panel_events, kPanelReady | kPanelFailed, pdFALSE, pdFALSE,
                                              timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY);
    return (b & kPanelReady) != 0;
}

void displayReveal()
{
    // The caller holds the LVGL lock, so no refresh runs now: the next one is the first of the finished UI.
    if (!s_disp || s_revealed) return;
    if (lv_timer_t* t = lv_display_get_refr_timer(s_disp)) lv_timer_resume(t);   // paused in displayStart()
    __atomic_store_n(&s_reveal_from, s_refr_seq + 1, __ATOMIC_RELEASE);
    lv_obj_t* scr = lv_display_get_screen_active(s_disp);
    if (scr) lv_obj_invalidate(scr);
}

bool lvLock(uint32_t timeout_ms) { return s_port_ready && lvgl_port_lock(timeout_ms); }

void lvUnlock()
{
    if (s_port_ready) lvgl_port_unlock();
}

void setBrightness(int percent)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s_brightness = percent;
    // before the first frame: reveal() applies it once the frame is visible
    if (s_revealed) bsp_display_brightness_set(percent);
}

void setFrameCap(int fps)
{
    s_frame_cap = fps;
    if (s_disp && lvLock(200)) {
        applyFrameCap();
        lvUnlock();
    }
}

bool displayReadBack(int step, void (*row)(const uint16_t* px, int w, int y, void* user), void* user)
{
    if (!s_frames[0] || !row) return false;
    step = step < 1 ? 1 : step > 4 ? 4 : step;
    // the newest published frame (the caller holds the LVGL lock: no refresh draws meanwhile); the PPA / DMA wrote
    // it behind the cache
    const uint8_t* f = s_frames[s_book.latest % 3];
    esp_cache_msync((void*)f, kFrameBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const uint16_t* fb = reinterpret_cast<const uint16_t*>(f);
    uint16_t line[kNativeW];
    const int w = kNativeW / step;
    for (int y = 0; y < kNativeH / step; ++y) {
        const uint16_t* src = fb + (size_t)y * step * kNativeW;
        for (int x = 0; x < w; ++x) line[x] = src[x * step];
        row(line, w, y, user);
    }
    return true;
}

}  // namespace hal
