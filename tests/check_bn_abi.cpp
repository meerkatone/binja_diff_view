#include <cstdint>
#include <stdio.h>

#include "binaryninjacore.h"

int main(void)
{
	const uint32_t runtime = BNGetCurrentCoreABIVersion();
	printf("headers=%u runtime=%u\n", (unsigned)BN_CURRENT_CORE_ABI_VERSION,
	    (unsigned)runtime);
	return runtime == BN_CURRENT_CORE_ABI_VERSION ? 0 : 1;
}
