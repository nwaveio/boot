/**************************************************************************//**
 * @file
 * @brief EFM32 Bootloader. Preinstalled on all new EFM32 devices
 * @author Energy Micro AS
 * @version 1.65
 ******************************************************************************
 * @section License
 * <b>(C) Copyright 2013 Energy Micro AS, http://www.energymicro.com</b>
 *******************************************************************************
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 * 4. The source and compiled code may only be used on Energy Micro "EFM32"
 *    microcontrollers and "EFR4" radios.
 *
 * DISCLAIMER OF WARRANTY/LIMITATION OF REMEDIES: Energy Micro AS has no
 * obligation to support this Software. Energy Micro AS is providing the
 * Software "AS IS", with no express or implied warranties of any kind,
 * including, but not limited to, any implied warranties of merchantability
 * or fitness for any particular purpose or warranties against infringement
 * of any proprietary rights of a third party.
 *
 * Energy Micro AS will not be liable for any consequential, incidental, or
 * special damages, or any other relief, or for any claim by any third party,
 * arising from your use of this Software.
 *
 *****************************************************************************/
#include <stdbool.h>
#include "em_device.h"
#include "usart.h"
#include "xmodem.h"
#include "boot.h"
#include "debuglock.h"
#include "autobaud.h"
#include "crc.h"
#include "config.h"
#include "flash.h"

//nwave edition
#ifndef NDEBUG
#include "debug.h"
#include <stdio.h>
#endif
#include "nwrm_uart.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_chip.h"
#include "nwrm_cmu.h"


/** Version string, used when the user connects */
#define BOOTLOADER_VERSION_STRING "1.63 "

/* Vector table in RAM. We construct a new vector table to conserve space in
 * flash as it is sparsly populated. */
#if defined (__ICCARM__)
#pragma location=0x20000000
__no_init uint32_t vectorTable[47];
#elif defined (__CC_ARM)
uint32_t vectorTable[47] __attribute__((at(0x20000000)));
#elif defined (__GNUC__)
uint32_t vectorTable[47] __attribute__((aligned(512)));
#else
#error Undefined toolkit, need to define alignment
#endif


/*
 * This variable holds the computed CRC-16 of the bootloader and is used during
 * production testing to ensure the correct programming of the bootloader.
 * This can safely be omitted if you are rolling your own bootloader.
 */
#if defined ( __ICCARM__ )
#pragma location=0x200000bc
__no_init uint16_t bootloaderCRC;
#elif defined (__CC_ARM)
uint16_t bootloaderCRC __attribute__((at(0x200000bc)));
#elif defined (__GNUC__)
uint16_t bootloaderCRC __attribute__((at(0x200000bc)));
#else
#error Undefined toolkit, need to define alignment
#endif

/* If this flag is set the bootloader will be reset when the RTC expires.
 * This is used when autobaud is started. If there has been no synchronization
 * until the RTC expires the entire bootloader is reset.
 *
 * Essentially, this makes the RTC work as a watchdog timer.
 */
bool resetEFM32onRTCTimeout = false;

/**************************************************************************//**
 * Strings.
 *****************************************************************************/
uint8_t crcString[]     = "\r\nCRC: ";
uint8_t newLineString[] = "\r\n";
uint8_t readyString[]   = "\r\nReady\r\n";
uint8_t okString[]      = "\r\nOK\r\n";
uint8_t failString[]    = "\r\nFail\r\n";
uint8_t unknownString[] = "\r\n?\r\n";

/**************************************************************************//**
 * @brief RTC IRQ Handler
 *   The RTC is used to keep the power consumption of the bootloader down while
 *   waiting for the pins to settle, or work as a watchdog in the autobaud
 *   sequence.
 *****************************************************************************/
//nwave edition
#if 0
void RTC_IRQHandler(void)
{
  /* Clear interrupt flag */
  RTC->IFC = RTC_IFC_COMP1 | RTC_IFC_COMP0 | RTC_IFC_OF;
  /* Check if EFM should be reset on timeout */
  if (resetEFM32onRTCTimeout)
  {
#ifndef NDEBUG
    printf("Autobaud Timeout. Resetting EFM32.\r\n");
#endif
    /* Write to the Application Interrupt/Reset Command Register to reset
     * the EFM32. See section 9.3.7 in the reference manual. */
    SCB->AIRCR = 0x05FA0004;
  }
}
#endif
/**************************************************************************//**
 * @brief
 *   This function is an infinite loop. It actively waits for one of the
 *   following conditions to be true:
 *   1) The SWDClk Debug pins is not asserted and a valid application is
 *      loaded into flash.
 *      In this case the application is booted.
 *   OR:
 *   2) The SWD Clk pin is asserted and there is an incoming packet
 *      on the USART RX line
 *      In this case we start sensing to measure the baudrate of incoming packets.
 *
 *   If none of these conditions are met, the EFM32G is put to EM2 sleep for
 *   250 ms.
 *****************************************************************************/
volatile unsigned int val = 0;

void waitForBootOrUSART(void)
{
  uint32_t SWDpins;
#ifndef NDEBUG
  uint32_t oldPins = 0xf;
#endif
  /* Initialize RTC */
  /* Clear interrupt flags */
  RTC->IFC = RTC_IFC_COMP1 | RTC_IFC_COMP0 | RTC_IFC_OF;
  /* 250 ms wakeup time */
  RTC->COMP0 = (PIN_LOOP_INTERVAL * LFRCO_FREQ) / 1000;
  /* Enable Interrupts on COMP0 */
  RTC->IEN = RTC_IEN_COMP0;
  /* Enable RTC interrupts */
  NVIC_EnableIRQ(RTC_IRQn);
  /* Enable RTC */
  RTC->CTRL = RTC_CTRL_COMP0TOP | RTC_CTRL_DEBUGRUN | RTC_CTRL_EN;

  
  //nwave edition
  //GPIO->P[1].MODEH = (GPIO->P[1].MODEH & ~ _GPIO_P_MODEH_MODE11_MASK) |
  //GPIO_P_MODEH_MODE11_INPUTPULLFILTER;
  GPIO_PinModeSet(gpioPortB, 11, /*gpioModeInput*/ gpioModeInputPull , 0);
  //GPIO->P[1].DOUT = GPIO->P[1].DOUT | (1 << 11) ;   
  val = GPIO_PinInGet(gpioPortB, 11);
  
  
  //NWRM_UART_Send("S",1);
  
  
  while (1)
  {
    /* The SWDCLK signal is used to determine if the application
     * Should be booted or if the bootloader should be started
     * SWDCLK (F0) has an internal pull-down and should be pulled high
     *             to enter bootloader mode.
     */
    /* Check input pins */
    //nwave edition
    //SWDpins = GPIO->P[5].DIN & 0x1;
    SWDpins = val;//(GPIO->P[1].DIN & 0x800) >> 11;
    
#ifndef NDEBUG
    if (oldPins != SWDpins)
    {
      oldPins = SWDpins;
      printf("New pin: %x \r\n", SWDpins);
    }
#endif

    //nwave edition
    NWRM_UART_Init(9600, true, false);     
    NWRM_UART_Send("Awaiting for the bootloader entering...\n",40);          
    int ch;
    for (int i = 0; i < 0xfffff; i++ )
    {
     ch = NWRM_UART_GetChar();
     if (ch!= -1 ) break;
    }
    //ch = USART_rxByte();
    
    /* Check if pins are not asserted AND firmware is valid */
    //if ((SWDpins != 0x1) && (BOOT_checkFirmwareIsValid()))
    if ((ch != 'U') && (BOOT_checkFirmwareIsValid()))
    {
      /* Boot application */
      NWRM_UART_Send("Booting application...\n",24);      
#ifndef NDEBUG
      printf("Booting application \r\n");
#endif
      BOOT_boot();
    }

    /* SWDCLK (F0) is pulled high and SWDIO (F1) is pulled low */
    /* Enter bootloader mode */
    //nwave edition
    //if (SWDpins == 0x1)
    if (ch == 'U')    
    {
      /* Increase timeout to 30 seconds */
      RTC->COMP0 = AUTOBAUD_TIMEOUT * LFRCO_FREQ;
      /* If this timeout occurs the EFM32 is rebooted. */
      /* This is done so that the bootloader cannot get stuck in autobaud sequence */
      resetEFM32onRTCTimeout = true;
#ifndef NDEBUG
      printf("Starting autobaud sequence\r\n");
#endif
      return;
    }
    /* Go to EM2 and wait for RTC wakeup. */
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    __WFI();
  }
}




/**************************************************************************//**
 * @brief
 *   Helper function to print flash write verification using CRC
 * @param start
 *   The start of the block to calculate CRC of.
 * @param end
 *   The end of the block. This byte is not included in the checksum.
 *****************************************************************************/
__ramfunc void verify(uint32_t start, uint32_t end)
{
  USART_printString(crcString);
  USART_printHex(CRC_calc((void *) start, (void *) end));
  USART_printString(newLineString);
}

/**************************************************************************//**
 * @brief
 *   The main command line loop. Placed in Ram so that it can still run after
 *   a destructive write operation.
 *   NOTE: __ramfunc is a IAR specific instruction to put code into RAM.
 *   This allows the bootloader to survive a destructive upload.
 *****************************************************************************/
__ramfunc void nwave_bootloader(void);

__ramfunc void commandlineLoop(void)
{
  uint32_t flashSize;
  uint8_t  c;
  uint8_t *returnString;

  /* Find the size of the flash. DEVINFO->MSIZE is the
   * size in KB so left shift by 10. */
  flashSize = ((DEVINFO->MSIZE & _DEVINFO_MSIZE_FLASH_MASK) >> _DEVINFO_MSIZE_FLASH_SHIFT)
              << 10;
  //nwave edition
  NWRM_UART_Send("S",1);
  /* The main command loop */
  while (1)
  {
    /* Retrieve new character */
    c = USART_rxByte();
    /* Echo */
    if (c != 0)
    {
      USART_txByte(c);
    }
    switch (c)
    {
    /* Upload command */
    case 'u':
      USART_printString(readyString);
      //nwave edition
      nwave_bootloader();
      //XMODEM_download(BOOTLOADER_SIZE, flashSize);
      break;
    /* Destructive upload command */
    case 'd':
      USART_printString(readyString);
      //XMODEM_download(0, flashSize);
      break;
    /* Write to user page */
    case 't':
      USART_printString(readyString);
      //XMODEM_download(XMODEM_USER_PAGE_START, XMODEM_USER_PAGE_END);
      break;
    /* Write to lock bits */
    case 'p':
      //DEBUGLOCK_startDebugInterface();
      USART_printString(readyString);
#ifdef USART_OVERLAPS_WITH_BOOTLOADER
      /* Since the UART overlaps, the bit-banging in DEBUGLOCK_startDebugInterface()
       * Will generate some traffic. To avoid interpreting this as UART communication,
       * we need to flush the LEUART data buffers. */
      BOOTLOADER_USART->CMD = LEUART_CMD_CLEARRX;
#endif
      //XMODEM_download(XMODEM_LOCK_PAGE_START, XMODEM_LOCK_PAGE_END);
      break;
    /* Boot into new program */
    case 'b':
      BOOT_boot();
      break;
    /* Debug lock */
    case 'l':
#ifndef NDEBUG
      /* We check if there is a debug session active in DHCSR. If there is we
       * abort the locking. This is because we wish to make sure that the debug
       * lock functionality works without a debugger attatched. */
      if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0x0)
      {
        printf("\r\n\r\n **** WARNING: DEBUG SESSION ACTIVE. NOT LOCKING!  **** \r\n\r\n");
        USART_printString("Debug active.\r\n");
      }
      else
      {
        printf("Starting debug lock sequence.\r\n");
#endif
      if (/*DEBUGLOCK_lock()*/ 1)
      {
        returnString = okString;
      }
      else
      {
        returnString = failString;
      }
      USART_printString(returnString);
#ifndef NDEBUG
        printf("Debug lock word: 0x%x \r\n", *((uint32_t *) DEBUG_LOCK_WORD));
      }
#endif
      break;
    /* Verify content by calculating CRC of entire flash */
    case 'v':
      verify(0, flashSize);
      break;
    /* Verify content by calculating CRC of application area */
    case 'c':
      verify(BOOTLOADER_SIZE, flashSize);
      break;
    /* Verify content by calculating CRC of user page.*/
    case 'n':
      verify(XMODEM_USER_PAGE_START, XMODEM_USER_PAGE_END);
      break;
    /* Verify content by calculating CRC of lock page */
    case 'm':
      verify(XMODEM_LOCK_PAGE_START, XMODEM_LOCK_PAGE_END);
      break;
    /* Reset command */
    case 'r':
      /* Write to the Application Interrupt/Reset Command Register to reset
       * the EFM32. See section 9.3.7 in the reference manual. */
      SCB->AIRCR = 0x05FA0004;
      break;
    /* Unknown command */
    case 0:
      /* Timeout waiting for RX - avoid printing the unknown string. */
      break;
    default:
      USART_printString(unknownString);
    }
  }
}

/**************************************************************************//**
 * @brief  Create a new vector table in RAM.
 *         We generate it here to conserve space in flash.
 *****************************************************************************/
void generateVectorTable(void)
{
  vectorTable[AUTOBAUD_TIMER_IRQn + 16] = (uint32_t) TIMER_IRQHandler;
  vectorTable[RTC_IRQn + 16]            = (uint32_t) RTC_IRQHandler;
#ifdef USART_OVERLAPS_WITH_BOOTLOADER
  vectorTable[GPIO_EVEN_IRQn + 16]      = (uint32_t) GPIO_IRQHandler;
#endif
  SCB->VTOR                             = (uint32_t) vectorTable;
}

/**************************************************************************//**
 * @brief  Main function
 *****************************************************************************/
int main(void)
{
  uint32_t clkdiv;
  uint32_t periodTime24_8;
  uint32_t tuning;

  /* Handle potential chip errata */
  /* Uncomment the next line to enable chip erratas for engineering samples */
  //CHIP_init();

        
  /* Generate a new vector table and place it in RAM */
  //nwave edition        
  //generateVectorTable();

  /* Calculate CRC16 for the bootloader itself and the Device Information page. */
  /* This is used for production testing and can safely be omitted in */
  /* your own code. */
  bootloaderCRC  = CRC_calc((void *) 0x0, (void *) BOOTLOADER_SIZE);
  bootloaderCRC |= CRC_calc((void *) 0x0FE081B2, (void *) 0x0FE08200) << 16;
  /* End safe to omit. */

  /* Enable clocks for peripherals. */
  CMU->HFPERCLKDIV = CMU_HFPERCLKDIV_HFPERCLKEN;
  CMU->HFPERCLKEN0 = CMU_HFPERCLKEN0_GPIO | BOOTLOADER_USART_CLOCK |
                     AUTOBAUD_TIMER_CLOCK ;

  /* Enable LE and DMA interface */
  CMU->HFCORECLKEN0 = CMU_HFCORECLKEN0_LE | CMU_HFCORECLKEN0_DMA;

  /* Enable LFRCO for RTC */
  CMU->OSCENCMD = CMU_OSCENCMD_LFRCOEN;
  /* Setup LFA to use LFRCRO */
  CMU->LFCLKSEL = CMU_LFCLKSEL_LFA_LFRCO | CMU_LFCLKSEL_LFB_HFCORECLKLEDIV2;
  /* Enable RTC */
  CMU->LFACLKEN0 = CMU_LFACLKEN0_RTC;
#if 0
#ifndef NDEBUG
  DEBUG_init();
  printf("\r\n\r\n *** Debug output enabled. ***\r\n\r\n");
#endif
#endif
  /* Figure out correct flash page size */
  FLASH_CalcPageSize();

  /* Wait for a boot operation */
  waitForBootOrUSART();

#ifdef BOOTLOADER_LEUART_CLOCK
  /* Enable LEUART */
  CMU->LFBCLKEN0 = BOOTLOADER_LEUART_CLOCK;
#endif

  /* Change to 28MHz internal osciallator to increase speed of
   * bootloader */
  tuning = (DEVINFO->HFRCOCAL1 & _DEVINFO_HFRCOCAL1_BAND28_MASK)
           >> _DEVINFO_HFRCOCAL1_BAND28_SHIFT;

  CMU->HFRCOCTRL = CMU_HFRCOCTRL_BAND_28MHZ | tuning;
  /* Wait for the HFRCO to stabilize. This ensures that the period
   * we measure in the autobaud sequence is accurate and not affected
   * by the bootloader being inaccurate. */
  while (!(CMU->STATUS & CMU_STATUS_HFRCORDY)) ;

#ifndef NDEBUG
  /* Calculate new clock division based on the 28Mhz clock */
  DEBUG_USART->CLKDIV = 3634;
#endif

  /* Setup pins for USART */
  CONFIG_UsartGpioSetup();

  /* AUTOBAUD_sync() returns a value in 24.8 fixed point format */
  //nwave edition
  //periodTime24_8 = AUTOBAUD_sync();
#ifndef NDEBUG
  printf("Autobaud complete.\r\n");
  printf("Measured periodtime (24.8): %d.%d\r\n", periodTime24_8 >> 8, periodTime24_8 & 0xFF);
#endif

  /* When autobaud has completed, we can be fairly certain that
   * the entry into the bootloader is intentional so we can disable the timeout.
   */
  NVIC_DisableIRQ(RTC_IRQn);

#ifdef BOOTLOADER_LEUART_CLOCK
  clkdiv = ((periodTime24_8 >> 1) - 256);
  GPIO->ROUTE = 0;
#else
  /* To go from the period to the necessary clkdiv we need to use
   * Equation 16.2 in the reference manual. Note that
   * periodTime = HFperclk / baudrate */
  clkdiv = (periodTime24_8 - (16 << 8)) >> 4;

  /* Check if the clock division is too small, if it is, we change
   * to an oversampling rate of 4x and calculate a new clkdiv.
   */
  if (clkdiv < 3000)
  {
    clkdiv = (periodTime24_8 - (4 << 8)) >> 2;
    BOOTLOADER_USART->CTRL |= USART_CTRL_OVS_X4;
  }
#endif
#ifndef NDEBUG
  printf("BOOTLOADER_USART clkdiv = %d\r\n", clkdiv);
#endif

  /* Initialize the UART */
  //nwave edition
  //USART_init(clkdiv);
  NWRM_UART_Init(9600, true, false);  
  NWRM_UART_Send("Welcome to Nwave Bootloader!\n",29);  
  int watch_c;
  
  

  /* Print a message to show that we are in bootloader mode */
  USART_printString("\r\n\r\n" BOOTLOADER_VERSION_STRING  "ChipID: ");
  /* Print the chip ID. This is useful for production tracking */
  USART_printHex(DEVINFO->UNIQUEH);
  USART_printHex(DEVINFO->UNIQUEL);
  USART_printString("\r\n");

  /* Initialize flash for writing */
  FLASH_init();

  /* Start executing command line */
  commandlineLoop();
}



__ramfunc unsigned short slow_crc16(unsigned short sum,
	unsigned char *p,
	unsigned int len)
{
	while (len--)
	{
		int i;
		unsigned char byte = *(p++);
		for (i = 0; i < 8; ++i)
		{
			unsigned long oSum = sum;
			sum <<= 1;
			if (byte & 0x80)
			sum |= 1;
			if (oSum & 0x8000)
			sum ^= 0x1021;
			byte <<= 1;
		}
	}
	return sum;
}

unsigned char length[4];
int length_i;
unsigned int j;
unsigned int curr_blck_ind;
unsigned int block_number;
unsigned short crc = 0;
unsigned short calculated_crc = 0;
unsigned short calc_blck_crc = 0;
unsigned short rcvd_blck_crc = 0;
unsigned short calc_crc_tail = 0;
unsigned int crc_blck_fails_cnt = 0;
int i = 0;

int block_cnt = 0;
int tail = 0;
int ch;
// 512;                /* Gecko and Tiny, 'G' or 'I' */

#define    BLOCK_SIZE    512 
#define    ACK_ENABLED   0

__ramfunc void nwave_bootloader(void)
{
  
   unsigned int baseAddress = BOOTLOADER_SIZE;//0x800;
  
      for (i = 0; i < 4; i++) {
       length[i] = USART_rxByte();
       *((unsigned char *)&length_i + i) = length[i];
    }
   
    FLASH_CalcPageSize();
    FLASH_init();
    
    block_cnt = length_i / BLOCK_SIZE;
    tail = length_i % BLOCK_SIZE;    
    unsigned char buffer[BLOCK_SIZE]; 
    
    for (j = 0; j < BLOCK_SIZE; j++) buffer[j] = 0;
      
    //$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$
    // Loop for all blocks without a tail
    for (block_number = 0; block_number <= block_cnt - 1; block_number++)
       {
           // 1. Receive current block to the serial port
            for (curr_blck_ind = 0; curr_blck_ind < BLOCK_SIZE; curr_blck_ind++) {
               buffer[curr_blck_ind] = USART_rxByte();
#if ACK_ENABLED > 0               
               NWRM_UART_Send("A",1);
#endif               
            }
           calc_blck_crc = 0;             
           calc_blck_crc = slow_crc16(0, (unsigned char*)buffer, BLOCK_SIZE );  
           
           
           rcvd_blck_crc = 0;
           //Addition
#if 1          
           NWRM_UART_Send("C", 1);           
           for (i = 0; i < 2; i++) {
            *((unsigned char *)&rcvd_blck_crc + i) = USART_rxByte();
            }           
           
           if (rcvd_blck_crc != calc_blck_crc) {
              crc_blck_fails_cnt++;
             
           }
#endif            
           //Addition 
            
           // 1.5 Flash the received block  
           // 

            
                FLASH_eraseOneBlock(baseAddress + block_number * BLOCK_SIZE);
            
                FLASH_writeBlock( (void *)(baseAddress + block_number * BLOCK_SIZE),
                0,
                BLOCK_SIZE,
                (unsigned char const *)buffer);
           // 2. Send in return and confirmation to the HOST 
                NWRM_UART_Send("B", 1);              
       }    
     //3. Receive the tail 
       //ADDITION
       //NWRM_UART_Send("T",1);    
       //ADDITION
       unsigned int tail_index;
       for ( tail_index = 0;  tail_index < tail;  tail_index++) {
               buffer[ tail_index] = USART_rxByte();
#if ACK_ENABLED > 0               
               NWRM_UART_Send("A",1);
#endif                
       }

        calc_crc_tail = slow_crc16(0, (unsigned char*)buffer, tail);
    
        FLASH_eraseOneBlock(baseAddress + block_cnt * BLOCK_SIZE );       
        // Flash the tail
        FLASH_writeBlock( (void *)(baseAddress + block_cnt * BLOCK_SIZE),
        0,
        BLOCK_SIZE,
        (unsigned char const *)buffer);
    
        NWRM_UART_Send("B", 1);    

    *((unsigned char *)&crc + 0) = USART_rxByte(); 
    NWRM_UART_Send("B", 1);        
    *((unsigned char *)&crc + 1) = USART_rxByte();            
    // Check CRC here
    // First Receive CRC

    calculated_crc = 0;
    calculated_crc = slow_crc16(0, (unsigned char*)baseAddress, length_i );    
    
    
    if (crc == calculated_crc ) NWRM_UART_Send("Z", 1);
      else NWRM_UART_Send("X", 1);
    
    
  
}