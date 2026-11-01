// Desc: The NFC app's entry point - see novagui_nfc.cpp.
// File: novagui_nfc.h
#ifndef NOVA_GUI_NFC_H
#define NOVA_GUI_NFC_H

namespace nova {
namespace screens {

void open_nfc(void);        // read a card, save it, dump its memory, browse the library

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_NFC_H
