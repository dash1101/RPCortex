// tips: recommended actions (tips.cpp, core/advice.cpp).
#ifndef RPC_TIPS_H
#define RPC_TIPS_H
void     tips_register(void);
void     tips_note_crash(bool crashed);   // at boot: did the last run end badly
unsigned tips_count(void);                // what `tips` would list now
void     tips_login_line(void);           // "2 recommended actions: 'tips' ..."
#endif
