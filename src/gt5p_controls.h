#pragma once
/* -2 when unavailable; -1 until the saved player-one configuration is available; otherwise 0/1. */
int gt5p_modern_controls_state();
bool gt5p_modern_controls_pending();
bool gt5p_modern_controls_request(int yes);
void gt5p_controls_install();
