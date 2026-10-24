// USB-MIDI host on the Tab5 USB-A port (USB 2.0 HS OTG; class-compliant devices plugged in directly).
//
// The BSP's bsp_usb_host_start() is not used: it switches USB 5 V through the BSP expander driver, which
// resets the 0x44 expander (charger enable and C6 supply drop). Here the 5 V rail is set register by
// register (board.cpp) and the USB Host Library is installed directly.
// One library task and one client task (core 0, priority 5). The client opens every new device, looks for
// an Audio / MIDIStreaming interface (class 1, subclass 3) with a bulk (or interrupt) IN and OUT endpoint,
// claims it and keeps one IN transfer queued. Each 4-byte USB-MIDI event packet becomes a MidiMsg for the
// hook (called from the client task). midiSend() queues packets for the OUT endpoint (any task).
// Hubs and several MIDI devices at once are not supported (first device wins).
#include <string.h>
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/hal.h"
#include "hal/hal_private.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

namespace hal {

namespace {
const char* TAG = "orbit_midi";
constexpr int kRing = 64;          // queued OUT packets
constexpr int kMaxOutPackets = 16; // per OUT transfer

enum class State : uint8_t { None = 0, Open, Closing };

struct Device {
    usb_device_handle_t hdl = nullptr;
    uint8_t intf = 0;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;
    usb_transfer_t* xin = nullptr;
    usb_transfer_t* xout = nullptr;
    bool in_busy = false, out_busy = false, claimed = false, flushed = false;
    State state = State::None;
};

bool s_started = false;
MidiHook s_hook = nullptr;
void* s_hook_user = nullptr;
usb_host_client_handle_t s_client = nullptr;
Device s_dev;
uint8_t s_new_addr = 0;            // client task only
bool s_connected = false;          // __atomic
char s_name[48] = "";

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // OUT ring, out_busy, state
uint8_t s_ring[kRing][4];
unsigned s_head = 0, s_tail = 0;

void deliver(const uint8_t* p)
{
    const uint8_t cin = p[0] & 0x0F;
    MidiMsg m = {p[1], p[2], p[3], (uint8_t)(p[0] >> 4)};
    switch (cin) {
    case 0x2: case 0x3:                        // system common, 2 / 3 bytes
    case 0x8: case 0x9: case 0xA: case 0xB:    // channel voice
    case 0xC: case 0xD: case 0xE:
        break;
    case 0x5:                                  // single-byte system common (not a SysEx end)
        if (m.status < 0xF1 || m.status == 0xF7) return;
        m.d1 = m.d2 = 0;
        break;
    case 0xF:                                  // single byte: real-time (clock, start, stop, ...)
        if (m.status < 0xF8) return;
        m.d1 = m.d2 = 0;
        break;
    default:                                   // SysEx and reserved codes: not used
        return;
    }
    if (cin == 0xC || cin == 0xD) m.d2 = 0;    // program change / channel pressure: 2 bytes
    MidiHook hook = s_hook;
    if (hook) hook(m, s_hook_user);
}

// fill and submit the OUT transfer when idle (any task, and the OUT callback)
void kickOut()
{
    usb_transfer_t* x = nullptr;
    portENTER_CRITICAL(&s_mux);
    if (s_dev.state == State::Open && s_dev.xout && !s_dev.out_busy && s_head != s_tail) {
        x = s_dev.xout;
        const int max = s_dev.mps_out / 4 < kMaxOutPackets ? s_dev.mps_out / 4 : kMaxOutPackets;
        int n = 0;
        while (s_tail != s_head && n < max) {
            memcpy(x->data_buffer + 4 * n, s_ring[s_tail], 4);
            s_tail = (s_tail + 1) % kRing;
            n++;
        }
        x->num_bytes = 4 * n;
        s_dev.out_busy = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (x && usb_host_transfer_submit(x) != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_dev.out_busy = false;
        portEXIT_CRITICAL(&s_mux);
    }
}

void inDone(usb_transfer_t* t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i + 4 <= t->actual_num_bytes; i += 4) {
            if (t->data_buffer[i] | t->data_buffer[i + 1]) deliver(t->data_buffer + i);
        }
    }
    const bool again = s_dev.state == State::Open && (t->status == USB_TRANSFER_STATUS_COMPLETED ||
                                                      t->status == USB_TRANSFER_STATUS_TIMED_OUT);
    s_dev.in_busy = again && usb_host_transfer_submit(t) == ESP_OK;
}

void outDone(usb_transfer_t*)
{
    portENTER_CRITICAL(&s_mux);
    s_dev.out_busy = false;
    portEXIT_CRITICAL(&s_mux);
    kickOut();
}

void copyName(const usb_str_desc_t* d)
{
    s_name[0] = 0;
    if (!d) return;
    size_t o = 0;
    const int chars = (d->bLength - 2) / 2;
    for (int i = 0; i < chars && o + 4 < sizeof(s_name); i++) {
        const uint16_t c = d->wData[i];
        if (c < 0x80) {
            s_name[o++] = (char)c;
        } else if (c < 0x800) {
            s_name[o++] = (char)(0xC0 | (c >> 6));
            s_name[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            s_name[o++] = (char)(0xE0 | (c >> 12));
            s_name[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            s_name[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    s_name[o] = 0;
}

void openDevice(uint8_t addr)
{
    usb_device_handle_t hdl = nullptr;
    if (usb_host_device_open(s_client, addr, &hdl) != ESP_OK) return;
    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(hdl, &cfg) != ESP_OK || !cfg) {
        usb_host_device_close(s_client, hdl);
        return;
    }
    // first Audio / MIDIStreaming interface with streaming endpoints
    int offset = 0;
    const usb_standard_desc_t* d = reinterpret_cast<const usb_standard_desc_t*>(cfg);
    bool in_midi = false, found = false;
    uint8_t intf = 0, alt = 0, ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;
    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &offset)) != nullptr) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (found) break;   // the endpoints of the chosen interface are complete
            const usb_intf_desc_t* i = reinterpret_cast<const usb_intf_desc_t*>(d);
            in_midi = i->bInterfaceClass == USB_CLASS_AUDIO && i->bInterfaceSubClass == 3;
            if (in_midi) {
                intf = i->bInterfaceNumber;
                alt = i->bAlternateSetting;
                ep_in = ep_out = 0;
            }
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_midi) {
            const usb_ep_desc_t* e = reinterpret_cast<const usb_ep_desc_t*>(d);
            const int type = e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
            if (type != USB_BM_ATTRIBUTES_XFER_BULK && type != USB_BM_ATTRIBUTES_XFER_INT) continue;
            if (e->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                if (!ep_in) {
                    ep_in = e->bEndpointAddress;
                    mps_in = USB_EP_DESC_GET_MPS(e);
                }
            } else if (!ep_out) {
                ep_out = e->bEndpointAddress;
                mps_out = USB_EP_DESC_GET_MPS(e);
            }
            if (ep_in || ep_out) found = true;
        }
    }
    if (!found) {
        ESP_LOGI(TAG, "USB device %u: no MIDI interface", addr);
        usb_host_device_close(s_client, hdl);
        return;
    }
    if (usb_host_interface_claim(s_client, hdl, intf, alt) != ESP_OK) {
        ESP_LOGW(TAG, "MIDI interface %u busy", intf);
        usb_host_device_close(s_client, hdl);
        return;
    }
    s_dev = Device{};
    s_dev.hdl = hdl;
    s_dev.intf = intf;
    s_dev.claimed = true;
    s_dev.ep_in = ep_in;
    s_dev.ep_out = ep_out;
    s_dev.mps_in = mps_in ? mps_in : 64;
    s_dev.mps_out = mps_out ? mps_out : 64;
    if (ep_in && usb_host_transfer_alloc(s_dev.mps_in, 0, &s_dev.xin) == ESP_OK) {
        s_dev.xin->device_handle = hdl;
        s_dev.xin->bEndpointAddress = ep_in;
        s_dev.xin->callback = inDone;
        s_dev.xin->num_bytes = s_dev.mps_in;
    }
    if (ep_out && usb_host_transfer_alloc(s_dev.mps_out, 0, &s_dev.xout) == ESP_OK) {
        s_dev.xout->device_handle = hdl;
        s_dev.xout->bEndpointAddress = ep_out;
        s_dev.xout->callback = outDone;
    }
    usb_device_info_t info = {};
    if (usb_host_device_info(hdl, &info) == ESP_OK) copyName(info.str_desc_product);
    if (!s_name[0]) strcpy(s_name, "USB MIDI");
    portENTER_CRITICAL(&s_mux);
    s_dev.state = State::Open;
    s_head = s_tail = 0;
    portEXIT_CRITICAL(&s_mux);
    if (s_dev.xin) s_dev.in_busy = usb_host_transfer_submit(s_dev.xin) == ESP_OK;
    __atomic_store_n(&s_connected, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "MIDI device \"%s\": interface %u, IN 0x%02x (%u), OUT 0x%02x (%u)", s_name, intf, ep_in,
             s_dev.mps_in, ep_out, s_dev.mps_out);
}

// Device gone (or shutting down): wait until both transfers have come back, then release everything.
void closeDevice(bool force)
{
    if (s_dev.state == State::None) return;
    portENTER_CRITICAL(&s_mux);
    s_dev.state = State::Closing;
    const bool busy = s_dev.in_busy || s_dev.out_busy;
    portEXIT_CRITICAL(&s_mux);
    __atomic_store_n(&s_connected, false, __ATOMIC_RELEASE);
    if (busy && !force) {
        if (!s_dev.flushed) {   // cancel what is queued: the callbacks then report it
            s_dev.flushed = true;
            if (s_dev.ep_in) {
                usb_host_endpoint_halt(s_dev.hdl, s_dev.ep_in);
                usb_host_endpoint_flush(s_dev.hdl, s_dev.ep_in);
            }
            if (s_dev.ep_out) {
                usb_host_endpoint_halt(s_dev.hdl, s_dev.ep_out);
                usb_host_endpoint_flush(s_dev.hdl, s_dev.ep_out);
            }
        }
        return;   // called again from the client loop
    }
    if (s_dev.claimed) usb_host_interface_release(s_client, s_dev.hdl, s_dev.intf);
    if (s_dev.xin) usb_host_transfer_free(s_dev.xin);
    if (s_dev.xout) usb_host_transfer_free(s_dev.xout);
    usb_host_device_close(s_client, s_dev.hdl);
    ESP_LOGI(TAG, "MIDI device \"%s\" closed", s_name);
    portENTER_CRITICAL(&s_mux);
    s_dev = Device{};
    portEXIT_CRITICAL(&s_mux);
    s_name[0] = 0;
}

void clientEvent(const usb_host_client_event_msg_t* msg, void*)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (s_dev.state == State::None) s_new_addr = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (s_dev.state != State::None && msg->dev_gone.dev_hdl == s_dev.hdl) {
            portENTER_CRITICAL(&s_mux);
            s_dev.state = State::Closing;
            portEXIT_CRITICAL(&s_mux);
        }
    }
}

void clientTask(void*)
{
    usb_host_client_config_t cfg = {};
    cfg.is_synchronous = false;
    cfg.max_num_event_msg = 5;
    cfg.async.client_event_callback = clientEvent;
    cfg.async.callback_arg = nullptr;
    if (usb_host_client_register(&cfg, &s_client) != ESP_OK) {
        ESP_LOGE(TAG, "USB client");
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(50));
        if (s_new_addr) {
            const uint8_t a = s_new_addr;
            s_new_addr = 0;
            openDevice(a);
        }
        if (s_dev.state == State::Closing) closeDevice(false);
    }
}

void libTask(void*)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

uint8_t cinFor(uint8_t status)
{
    if (status >= 0xF8) return 0xF;            // real-time
    if (status >= 0x80 && status < 0xF0) return status >> 4;
    switch (status) {
    case 0xF1: case 0xF3: return 0x2;
    case 0xF2: return 0x3;
    case 0xF6: return 0x5;
    default: return 0;                         // SysEx and undefined: not sent
    }
}
}  // namespace

bool midiStart(MidiHook hook, void* user)
{
    s_hook_user = user;
    s_hook = hook;
    if (s_started) return true;
    // 5 V on the USB-A port (IO expander 0x44 P3), without resetting the expander
    if (detail::ioe2Output(detail::kIoe2Usb5v, true) != ESP_OK) ESP_LOGW(TAG, "USB 5 V switch not set");
    usb_host_config_t cfg = {};
    cfg.skip_phy_setup = false;
    cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    const esp_err_t err = usb_host_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host: %s", esp_err_to_name(err));
        return false;
    }
    if (xTaskCreatePinnedToCore(libTask, "orbit_usb", 4096, nullptr, 5, nullptr, 0) != pdPASS ||
        xTaskCreatePinnedToCore(clientTask, "orbit_midi", 4096, nullptr, 5, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "USB tasks");
        return false;
    }
    s_started = true;
    ESP_LOGI(TAG, "USB-MIDI host started");
    return true;
}

bool midiConnected() { return __atomic_load_n(&s_connected, __ATOMIC_ACQUIRE); }

const char* midiDeviceName() { return midiConnected() ? s_name : ""; }

bool midiSend(const MidiMsg& m)
{
    const uint8_t cin = cinFor(m.status);
    if (!cin || !midiConnected()) return false;
    bool queued = false;
    portENTER_CRITICAL(&s_mux);
    const unsigned next = (s_head + 1) % kRing;
    if (s_dev.state == State::Open && s_dev.xout && next != s_tail) {
        uint8_t* p = s_ring[s_head];
        p[0] = (uint8_t)(((m.cable & 0x0F) << 4) | cin);
        p[1] = m.status;
        p[2] = cin == 0xF || cin == 0x5 ? 0 : m.d1;
        p[3] = cin == 0xF || cin == 0x5 || cin == 0x2 || cin == 0xC || cin == 0xD ? 0 : m.d2;
        s_head = next;
        queued = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (queued) kickOut();
    return queued;
}

}  // namespace hal
