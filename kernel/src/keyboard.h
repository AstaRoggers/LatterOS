#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);
void keyboard_irq_handler(void);
void keyboard_poll(void);

#endif