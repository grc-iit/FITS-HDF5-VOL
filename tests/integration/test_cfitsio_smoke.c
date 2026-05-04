/* M2.1 smoke: CFITSIO is linked and reports its version. */

#include <assert.h>
#include <stdio.h>
#include <fitsio.h>

int main(void)
{
    float v = 0.0f;
    fits_get_version(&v);
    assert(v >= 4.0f && "CFITSIO >= 4.0 required");
    printf("OK: cfitsio %.4f\n", v);
    return 0;
}
