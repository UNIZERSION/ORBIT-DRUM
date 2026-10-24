#pragma once
// Sample library on the SD card and project files. Loader/UI task only (stdio + dirent).
//
// Card layout (created on first start when missing):
//   /sdcard/ORBIT/KITS/<kit name>/*.wav        a kit: up to 16 WAVs, sorted by name -> pads A1..D4
//                                              (optional kit.txt: one "PAD=FILE" line per pad)
//   /sdcard/ORBIT/SAMPLES/<Category>/...       browser categories (Drums, Bass, Synth, Vocals, FX)
//   /sdcard/ORBIT/USER/...                     recordings, edited samples, imports ("User")
//   /sdcard/ORBIT/PROJECTS/<name>.orb           project (JSON), samples referenced by path
//   /sdcard/ORBIT/PROJECTS/<name>/              samples edited inside the project (WAV)
#include <stdint.h>
#include "config.h"
#include "project.h"
#include "sample.h"

namespace orbit {

constexpr int kMaxLibraryFiles = 4096;

enum class Category : uint8_t { Kits = 0, All, Drums, Bass, Synth, Vocals, Fx, User, Count };
const char* categoryName(Category c);   // "Kits", "All Samples", "Drums", ... (template labels)

struct LibEntry {
    char path[kPathLen];
    char name[kNameLen + 8];   // display name (file stem, may be shortened)
    uint32_t bytes = 0;
    Category cat = Category::User;
    uint8_t is_kit = 0;        // a kit folder (Category::Kits)
    uint8_t probed = 0;        // info valid
    uint8_t _pad = 0;
    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
    float seconds = 0.0f;
};

class Library {
public:
    // root e.g. "/sdcard/ORBIT" (device) or "<repo>/sdcard/ORBIT" (PC). Creates missing folders.
    bool open(const char* root);
    const char* root() const;
    // Walks KITS, SAMPLES and USER (depth <= 4), fills entries; probes WAV headers lazily.
    // Safe to call again (SCAN SD). Returns number of entries.
    int scan();
    int count() const;
    // The last scan() stopped at kMaxLibraryFiles (USER is walked before SAMPLES, so dropped
    // files are SAMPLES ones). The UI should say so ("list truncated").
    bool truncated() const;
    const LibEntry& at(int i) const;
    void probe(int i);                         // fills rate/channels/seconds from the header
    // Filter by category and case-insensitive substring (UTF-8 aware for Polish letters).
    // Writes matching indices to out (max), returns count.
    int filter(Category c, const char* query, int* out, int max) const;
    // Kit folder -> up to 16 file paths (pad order). Returns count.
    int kitFiles(int entry, char paths[][kPathLen], int max) const;
    // Projects
    int listProjects(char names[][32], int max) const;
    bool projectPath(const char* name, char* out, int len) const;     // .../PROJECTS/<name>.orb
    bool projectDir(const char* name, char* out, int len) const;      // .../PROJECTS/<name>/
    bool userPath(const char* file, char* out, int len) const;        // .../USER/<file>
    uint64_t freeBytes() const;
    uint64_t totalBytes() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---------------------------------------------------------------- project files (JSON)
// Sample references are stored per pad layer as {"path": "...", "name": "..."}; built-in kit
// samples as {"builtin": "TR-808/BD"}. Unknown keys are ignored, missing keys keep defaults,
// so older/newer files load. Returns false + message on error.
struct SampleRef {
    char path[kPathLen] = {};    // SD path, or "builtin:<kit>/<name>"
    char name[kNameLen] = {};
};
bool projectSave(const char* path, const Project& p, const SampleRef refs[kPads][kLayers], char* err, int errlen);
bool projectLoad(const char* path, Project& p, SampleRef refs[kPads][kLayers], char* err, int errlen);
// Exports the same JSON with a "state" header (EXPORT JSON in Settings/Project manager).
bool projectExportJson(const char* path, const Project& p, const SampleRef refs[kPads][kLayers]);

}  // namespace orbit
