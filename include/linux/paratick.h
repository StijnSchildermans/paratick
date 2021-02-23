#ifndef PARATICK_H
#define PARATICK_H

void setup_paratick_timer(void);
void paratick_enter_idle(void);
void paratick_exit_idle(void);
void paratick_start_idle(void);
void paratick_irq_enter(void);
void paratick_irq_exit(void);

bool paratick_init(void);


#endif
