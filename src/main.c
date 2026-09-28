#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
    printk("Lab 2 binary UART communication test\n");

    /* communication.c creates and starts its own thread */
    return 0;
}