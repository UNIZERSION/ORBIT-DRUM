// Test-only stand-in for host/tests/test_app.cpp. NEVER part of the firmware.
//
// orbit::ui::toast(): test_app does not link LVGL (ui/widgets.cpp holds the real toast), so the
// text is recorded here and the test checks the Polish messages of the App controller.
// ORBIT_TEST_VERBOSE=1 prints every toast.
// (A temporary engine stub lived here while core/engine.cpp did not exist; it was removed once
// the real engine landed.)
#include <cstdio>
#include <cstdlib>

namespace {
char g_toast[256] = {};
int g_toasts = 0;
}  // namespace

namespace orbit::ui {
void toast(const char* text)
{
    snprintf(g_toast, sizeof g_toast, "%s", text ? text : "");
    ++g_toasts;
    static const bool verbose = getenv("ORBIT_TEST_VERBOSE") != nullptr;
    if (verbose) std::printf("  [toast] %s\n", g_toast);
}
}  // namespace orbit::ui

namespace testhooks {
const char* lastToast() { return g_toast; }
int toastCount() { return g_toasts; }
}  // namespace testhooks
