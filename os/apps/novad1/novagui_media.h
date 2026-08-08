// Desc: The media player — a WAV, a folder of them, and a Bluetooth speaker.
// File: novagui_media.h
//
// A flat sibling of novagui, importing only the UI leaf, the same as
// novagui_ble and novagui_files.
//
// ONE open function, deliberately. The player is four screens — a front page, a
// file browser, a now-playing view and a speaker picker — but only the front
// page belongs in the app catalogue: the other three are places the front page
// takes you, and a catalogue row for each would be three ways into the middle of
// one app rather than four apps.
#ifndef NOVA_GUI_MEDIA_H
#define NOVA_GUI_MEDIA_H

namespace nova {
namespace screens {

// The App table's open function, so its shape is fixed by that.
void open_media(void);

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_MEDIA_H
