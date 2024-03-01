#include "vmp.h"

int
vmp_fetch_pte(kprocess_t *ps, vaddr_t vaddr, bool create, pte_t **pte_out)
{
	ipl_t ipl;
	int indexes[VMP_TABLE_LEVELS + 1];
	pte_t *table;

	vmp_addr_unpack(vaddr, indexes);

	table = (pte_t*)P2V(ps->vm->md.table);

	for (int level = VMP_TABLE_LEVELS; level > 0; level--) {
		pte_t *pte = &table[indexes[level]];

		/* note - level is 1-based */

		if (level == 1) {
			*pte_out = pte;
			return 0;
		}

		if (vmp_pte_characterise(pte) != kPTEKindValid)
			return -1;

		table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, level));
	}
	kfatal("unreached\n");
}
