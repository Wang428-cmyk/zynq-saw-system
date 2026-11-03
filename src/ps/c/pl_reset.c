#include "xgpio.h"
#include "sleep.h"

#define AXI_GPIO_DEVICE_ID	XPAR_AXI_GPIO_0_DEVICE_ID

XGpio xGpioInst;

void PL_Reset()
{
	//Initial GPIO
	XGpio_Initialize(&xGpioInst, AXI_GPIO_DEVICE_ID);

	//Reset FPGA
	//Set AXI GPIO Output Mode
	XGpio_SetDataDirection(&xGpioInst, 1, 0x0000);
	//Provide the reset for the FPGA
	XGpio_DiscreteSet(&xGpioInst, 1, 0x01);

	//Delay 20 ms
	usleep(200);

	//Disable the reset for the FPGA
	XGpio_DiscreteClear(&xGpioInst, 1, 0x01);

}
