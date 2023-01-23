#ifndef LAIEX_H_
#define LAIEX_H_

#include "lai/core.h"
#include "lai/helpers/resource.h"

int
laiex_view_resource(lai_nsnode_t *node, lai_variable_t *crs,
    struct lai_resource_view *view, lai_state_t *state);

#endif /* LAIEX_H_ */
