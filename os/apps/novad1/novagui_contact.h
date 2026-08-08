// Desc: The contact readers — tap an NFC tag, touch an iButton key.
// File: novagui_contact.h
//
// A flat sibling of novagui, importing only the UI leaf, the same as
// novagui_ble and novagui_tools.
//
// Both screens are the same shape and share one worker, because both are the
// same job: hold something against a reader and wait. Neither has a list, a
// filter or a table — there is one answer at a time and it either arrived or it
// did not.
#ifndef NOVA_GUI_CONTACT_H
#define NOVA_GUI_CONTACT_H

namespace nova {
namespace screens {

// The App table's open functions, so their shape is fixed by it.
void open_nfc(void);        // poll a PN532 for an ISO14443-A tag
void open_ibutton(void);    // wait for a DS1990A key on the 1-Wire pin

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_CONTACT_H
