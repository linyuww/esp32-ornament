#include "ornament_state.h"

#include <string.h>

void ornament_state_init(ornament_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->status = ORNAMENT_STATUS_IDLE;
    state->primary_remaining_percent = -1;
    state->secondary_remaining_percent = -1;
}

ornament_status_t ornament_status_from_text(const char *value)
{
    if (value == NULL) {
        return ORNAMENT_STATUS_IDLE;
    }
    if (strcmp(value, "running") == 0) {
        return ORNAMENT_STATUS_RUNNING;
    }
    if (strcmp(value, "done") == 0) {
        return ORNAMENT_STATUS_DONE;
    }
    if (strcmp(value, "error") == 0) {
        return ORNAMENT_STATUS_ERROR;
    }
    if (strcmp(value, "event") == 0) {
        return ORNAMENT_STATUS_EVENT;
    }
    return ORNAMENT_STATUS_IDLE;
}
