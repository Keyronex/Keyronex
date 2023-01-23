#include "laiex.h"

int
laiex_view_resource(lai_nsnode_t *node, lai_variable_t *crs,
    struct lai_resource_view *view, lai_state_t *state)
{
	lai_nsnode_t *hcrs;

	hcrs = lai_resolve_path(node, "_CRS");

	if (hcrs == NULL) {
		lai_warn("missing _CRS\n");
		return -1;
	}

	if (lai_eval(crs, hcrs, state)) {
		lai_warn("failed to eval _CRS");
		return -1;
	}

	*view = (struct lai_resource_view)LAI_RESOURCE_VIEW_INITIALIZER(crs);

	return 0;
}
