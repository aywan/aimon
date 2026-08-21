#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start periodically sampling the battery voltage ADC and pushing
 *        percent/present into state.
 */
void battery_init(void);

#ifdef __cplusplus
}
#endif
