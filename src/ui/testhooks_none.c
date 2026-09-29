/*
 * testhooks_none.c - builds without CONV_TEST_HOOKS: no test driver.
 */
#include "testhooks.h"

void testhooks_install(GtkApplication *app)
{
    (void)app;
}
