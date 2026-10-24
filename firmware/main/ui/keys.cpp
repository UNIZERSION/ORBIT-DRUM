// The M5Stack Tab5 Keyboard (0.4.6, hal/keyboard.cpp) with Ableton Live's default shortcuts where ORBIT has the same
// thing. The LVGL task takes the keys every UI tick (poll), so everything here is ordinary UI code.
//
//   typing        while the on-screen keyboard is open: characters, Backspace, Enter = OK, Esc = cancel
//   Esc           closes a dialog / popup, else brings a maximized panel back
//   Space         play from the start / stop (Live)        Shift+Space  continue from where it stopped (Live)
//   F9            record on / off (Live: arrangement record)
//   Tab           next view, Shift+Tab previous view (Live: Session / Arrangement)
//   Ctrl+Z        undo (pattern, then pads)               Ctrl+S save, Ctrl+O / Ctrl+N / Ctrl+Shift+S projects
//   Ctrl+,        settings (Live: Preferences)              Shift+F1     the guide
//   Up / Down     previous / next scene (Live: scene selection; ORBIT switches at once, on the bar while playing)
//   Enter         play the selected scene (Live: launch)    Left / Right previous / next pad (Live: track)
//   F1..F8        mute pads 1..8 of the shown bank (Live: track activators)   0  mute the selected pad (Live)
//   F11           maximize the work area (BASS: the whole screen) (Live: full screen)
//   Ctrl+Alt+B    BROWSER, Ctrl+Alt+M MIXER (Live: show the browser / the mixer)
//   computer MIDI keyboard (Live, M turns it on / off): A W S E D F T G Y H U J K O L P ; '
//                 = semitones from C. In BASS: the bass (KEYS range, ACCENT, SLIDE: legato when keys overlap).
//                 Elsewhere: pads 1..16 (A = pad 1, W = pad 2 ... P = pad 16, like a Drum Rack).
//   Z / X         octave down / up (the bass range)         C / V  velocity -20 / +20 (pads)
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "hal/hal.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace orbit::ui::keys {

namespace {

constexpr char kNotes[] = "awsedftgyhujkolp;'";   // 18 keys: C .. F of the next octave
bool s_midi = true;          // computer MIDI keyboard on (Live: M)
int s_vel = 100;             // pad velocity (Live: C / V)
int s_held = 0;              // note keys down since the last release
uint32_t s_pads_held = 0;    // pads started by note keys (released on Release)
bool s_bass_held = false;

// tab order on the top bar
const View kTabs[] = {View::Sample, View::Sequence, View::Bass, View::Mixer, View::Browser, View::Fx};

void stepView(int d)
{
    const View cur = app().ui().view;
    int i = 0;
    while (i < 6 && kTabs[i] != cur) ++i;
    app().setView(kTabs[(i + d + 6) % 6]);
}

void releaseNotes()
{
    for (int p = 0; p < kPads; ++p)
        if (s_pads_held & (1u << p)) app().padUp(p, Producer::Ui);
    s_pads_held = 0;
    if (s_bass_held) app().bassKeyOff(Producer::Ui);
    s_bass_held = false;
    s_held = 0;
}

void noteKey(int semi)
{
    if (app().ui().view == View::Bass) {
        const bool legato = s_bass_held && view_bass::liveSlide();
        app().bassKeyOn(view_bass::liveBase() + semi, view_bass::liveAccent(), legato, Producer::Ui);
        s_bass_held = true;
    } else if (semi < kPads) {
        app().padDown(semi, s_vel, Producer::Ui);
        s_pads_held |= 1u << semi;
    }
    ++s_held;
}

void octave(int d)
{
    view_bass::setLiveBase(view_bass::liveBase() + 12 * d);
    char lo[16], hi[16];
    static const char* const kN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int b = view_bass::liveBase();
    snprintf(lo, sizeof lo, "%s%d", kN[b % 12], b / 12 - 1);
    snprintf(hi, sizeof hi, "%s%d", kN[(b + 24) % 12], (b + 24) / 12 - 1);
    // the pads have no octaves: Z / X always move the bass keyboard (named so outside BASS too)
    app().toast("BASS OCTAVE: %s \xE2\x80\x93 %s", lo, hi);
}

void undo()
{
    App& a = app();
    if (a.canUndoPattern()) {
        a.undoPattern();
        a.toast("UNDO");
    } else if (a.canUndoPads()) {
        a.undoPads();
    } else {
        a.toast("Nothing to undo");
    }
}

void toggleMax()
{
    if (app().ui().view == View::Bass) panelToggleMaxEx(Panel::Work, true);
    else panelToggleMaxEx(Panel::Work, false);
}

}  // namespace

void handle(const hal::KeyEvent& e)
{
    using hal::Key;
    App& a = app();
    // 1. typing into the on-screen keyboard
    if (keyboardTypeKey(e.key == Key::Char, e.ch, e.key == Key::Backspace, e.key == Key::Enter, e.key == Key::Esc))
        return;
    if (e.key == Key::Release) {
        releaseNotes();
        return;
    }
    // 2. a dialog or popup: Esc closes it, the rest waits until it is closed
    if (overlayOpen()) {
        if (e.key == Key::Esc) overlayEscape();
        return;
    }
    const char c = e.key == Key::Char ? (char)tolower((unsigned char)e.ch) : 0;
    // 3. the computer MIDI keyboard (no Ctrl / Alt: those are shortcuts)
    if (c && s_midi && !e.ctrl && !e.alt) {
        const char* n = strchr(kNotes, c);
        if (n && c != ' ') {
            if (!e.repeat) noteKey((int)(n - kNotes));
            return;
        }
        switch (c) {
        case 'z': octave(-1); return;
        case 'x': octave(1); return;
        case 'c':
        case 'v':
            s_vel = c == 'c' ? (s_vel <= 20 ? 1 : s_vel - 20) : (s_vel >= 120 ? 127 : (s_vel == 1 ? 20 : s_vel + 20));
            a.toast("VELOCITY: %d", s_vel);
            return;
        default: break;
        }
    }
    // 4. shortcuts
    if (c == 'm' && !e.ctrl && !e.alt) {
        s_midi = !s_midi;
        releaseNotes();
        a.toast(s_midi ? "COMPUTER MIDI KEYBOARD: ON" : "COMPUTER MIDI KEYBOARD: OFF");
        return;
    }
    if (c == ' ') {
        if (a.playing()) a.stop();
        else if (e.shift) a.togglePlay();   // continue
        else a.play();                      // from the start
        return;
    }
    if (e.ctrl && e.alt && c == 'b') {
        a.setView(View::Browser);
        return;
    }
    if (e.ctrl && e.alt && c == 'm') {
        a.setView(View::Mixer);
        return;
    }
    if (e.ctrl && !e.alt) {
        switch (c) {
        case 'z': undo(); return;
        case 's':
            if (e.shift) dlg_project::open();
            else if (a.save()) a.toast("SAVED");
            return;
        case 'o':
        case 'n': dlg_project::open(); return;
        case ',': dlg_settings::open(); return;
        default: break;
        }
    }
    if (c == '0' && !e.ctrl && !e.alt) {
        a.toggleMute(a.ui().pad);
        return;
    }
    switch (e.key) {
    case Key::Esc:
        if (panelMaxed() != Panel::Count) panelToggleMaxEx(panelMaxed(), panelMaxedFull());
        return;
    case Key::Tab: stepView(e.shift ? -1 : 1); return;
    case Key::F9: a.toggleRecord(); return;
    case Key::F11: toggleMax(); return;
    case Key::Up:
        if (a.project().scene > 0) a.selectScene(a.project().scene - 1);
        return;
    case Key::Down:
        if (a.project().scene < kScenes - 1) a.selectScene(a.project().scene + 1);
        return;
    case Key::Enter:
        if (!a.playing()) a.play();
        return;
    case Key::Left:
        a.selectPad(a.ui().pad > 0 ? a.ui().pad - 1 : kPads - 1, false);
        return;
    case Key::Right:
        a.selectPad(a.ui().pad < kPads - 1 ? a.ui().pad + 1 : 0, false);
        return;
    default: break;
    }
    if (e.key >= Key::F1 && e.key <= Key::F8) {
        if (e.key == Key::F1 && e.shift) {
            dlg_help::open();
            return;
        }
        a.toggleMute(a.ui().seq_bank * 8 + ((int)e.key - (int)Key::F1));
        return;
    }
}

void poll()
{
    hal::KeyEvent e;
    for (int n = 0; n < 16 && hal::kbdGet(e); ++n) handle(e);
}

}  // namespace orbit::ui::keys
