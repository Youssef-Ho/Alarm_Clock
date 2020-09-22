/*
 * Alarm_Clock.c
 *
 * Created: 20.03.2018 18:32:07
 * Author : chaos
 */ 

//#include <avr/io.h>
#include "avr_compiler.h"
#include "pmic_driver.h"
#include "TC_driver.h"
#include "clksys_driver.h"
#include "sleepConfig.h"
#include "port_driver.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"
#include "stack_macros.h"

#include "mem_check.h"

#include "init.h"
#include "utils.h"
#include "errorHandler.h"
#include "NHD0420Driver.h"
#include "ButtonHandler.h"

#define BUTTON1SHORTPRESSEDMASK     0x01
#define BUTTON2SHORTPRESSEDMASK     0x02
#define BUTTON3SHORTPRESSEDMASK     0x04
#define BUTTON4SHORTPRESSEDMASK     0x08
#define BUTTON1LONGPRESSEDMASK     0x10
#define BUTTON2LONGPRESSEDMASK     0x20
#define BUTTON3LONGPRESSEDMASK     0x40
#define BUTTON4LONGPRESSEDMASK     0x80


extern void vApplicationIdleHook( void );
void vLedBlink(void *pvParameters);
void vButtonTask(void *pvParameters);
void vInitHeartbeatCounter(void);
void vUserInterface(void);
void xSetTime(uint8_t ucDigitToSet, uint8_t ucButtonValue, uint8_t *ucSeconds, uint8_t *ucMinutes, uint8_t *ucHours);
void vHeartbeat(void);

TaskHandle_t xledTask;
TaskHandle_t xButtonTaskHandle;
TaskHandle_t xHeartbeatTaskHandle;
TaskHandle_t xUserInterfaceHandle;

uint8_t ucHeartbeatTimerCounter;

uint8_t ucClockSeconds = 0;
uint8_t ucClockMinutes = 0;
uint8_t ucClockHours = 0;
uint8_t ucAlarmClockSeconds = 0;
uint8_t ucAlarmClockMinutes = 0;
uint8_t ucAlarmClockHours = 0;


typedef enum
{
    Clock,
    SetClock,
    SetAlarmClock
} eClockStates;
eClockStates eClockStateMachine = Clock;

void vApplicationIdleHook( void )
{	
	
}

int main(void)
{

	vInitClock();
	vInitDisplay();
    vInitHeartbeatCounter();
    
    sei();
    
	
	xTaskCreate(vLedBlink, (const char *) "ledBlink", configMINIMAL_STACK_SIZE+10, NULL, 1, &xledTask);
	xTaskCreate(vButtonTask, (const char *) "ButtonTask", configMINIMAL_STACK_SIZE, NULL, 1, &xButtonTaskHandle);
    xTaskCreate(vHeartbeat, (const char *) "Heartbeat", configMINIMAL_STACK_SIZE, NULL, 2, &xHeartbeatTaskHandle);
    xTaskCreate(vUserInterface, (const char *) "UserInterface", configMINIMAL_STACK_SIZE, NULL, 1, &xUserInterfaceHandle);


	vTaskStartScheduler();
	return 0;
}

void vLedBlink(void *pvParameters) {
	(void) pvParameters;
	PORTF.DIRSET = PIN0_bm; /*LED1*/
	PORTF.OUT = 0x01;
    uint32_t ulBlinkStatus = 0;
	for(;;) {
        xTaskNotifyWait(0, 0xffffffff, &ulBlinkStatus, pdMS_TO_TICKS(5));
        if (ulBlinkStatus)
        {
		    PORTF.OUTCLR = 0x01;				
		    vTaskDelay(100 / portTICK_RATE_MS);
		    PORTF.OUTSET = 0x01;
		    vTaskDelay(100 / portTICK_RATE_MS);
        }
	}
}
/*———————————————————–*/

void vButtonTask(void *pvParameters) {
    initButtons();
    vTaskDelay(3000);
    for(;;) {
        updateButtons();
        
        if(getButtonPress(BUTTON1) == SHORT_PRESSED) {
            xTaskNotify(xUserInterfaceHandle, BUTTON1SHORTPRESSEDMASK, eSetBits);
        }
        if(getButtonPress(BUTTON2) == SHORT_PRESSED) {
            xTaskNotify(xUserInterfaceHandle, BUTTON2SHORTPRESSEDMASK, eSetBits);
        }
        if(getButtonPress(BUTTON3) == SHORT_PRESSED) {
            xTaskNotify(xUserInterfaceHandle, BUTTON3SHORTPRESSEDMASK, eSetBits);
        }
        if(getButtonPress(BUTTON4) == SHORT_PRESSED) {
            xTaskNotify(xUserInterfaceHandle, BUTTON4SHORTPRESSEDMASK, eSetBits);
        }
        if(getButtonPress(BUTTON1) == LONG_PRESSED) {
            xTaskNotify(xUserInterfaceHandle, BUTTON1LONGPRESSEDMASK, eSetBits);
        }
        if(getButtonPress(BUTTON2) == LONG_PRESSED) {
            xTaskNotify(xUserInterfaceHandle, BUTTON2LONGPRESSEDMASK, eSetBits);
        }
        if(getButtonPress(BUTTON3) == LONG_PRESSED) {
            xTaskNotify(xUserInterfaceHandle, BUTTON3LONGPRESSEDMASK, eSetBits);
        }
        if(getButtonPress(BUTTON4) == LONG_PRESSED) {
            xTaskNotify(xUserInterfaceHandle, BUTTON4LONGPRESSEDMASK, eSetBits);
        }
        vTaskDelay((1000/BUTTON_UPDATE_FREQUENCY_HZ)/portTICK_RATE_MS);
    }
}
/*———————————————————–*/

void vInitHeartbeatCounter(void)
{
    /* Initializes the Heartbeat timer counter */
    TCC1.CTRLA |= (0b0101) << TC1_CLKSEL_gp;            // CLKdiv = 64 -> fTimer = 500kHz
    TCC1.INTCTRLA |= 1 << TC1_OVFINTLVL_gp;             // Overflow interrupt enable at low-level priority
    TCC1.PER = 4999;                                    // TOP after 5000 counts -> Tp = 1/(500kHz) * 5000 = 10ms
    PMIC.CTRL |= 1 << PMIC_LOLVLEN_bp;                  // enable low-level interrupt
}
/*———————————————————–*/

void vUserInterface(void)
{
    BaseType_t xButtonTaskNotification = pdFALSE;
    uint8_t ucButtonValue = 0;
    uint8_t ucClockSetDigit = 0;
    uint8_t ucAlarmClockSetDigit = 0;
    uint8_t ucAlarmActivated = 0;
    uint8_t ucAlarmTriggered = 0;
    
    while (1)
    {
        char TimeString[9];
        
        xButtonTaskNotification = xTaskNotifyWait(0, 0xffffffff, &ucButtonValue, pdMS_TO_TICKS(5));
                
        switch (eClockStateMachine)
        {
            case Clock:
            {
                if((ucClockHours == ucAlarmClockHours) && (ucClockMinutes == ucAlarmClockMinutes) && (ucClockSeconds == ucAlarmClockSeconds))
                {
                    if (ucAlarmActivated)
                    {
                        ucAlarmTriggered = 1;
                    }
                }
                if (ucAlarmTriggered)
                {
                    xTaskNotify(xledTask, 0x01, pdTRUE);
                }
                else
                {
                    xTaskNotify(xledTask, 0x00, pdTRUE);
                }
                if (ucButtonValue & BUTTON2SHORTPRESSEDMASK)
                {
                    if (ucAlarmTriggered || ucAlarmActivated)
                    {
                        ucAlarmActivated = 0;
                        ucAlarmTriggered = 0;
                    }
                    else
                    {
                        ucAlarmActivated = 1;
                    }
                }
                
                if (ucButtonValue & BUTTON4LONGPRESSEDMASK)
                {
                    eClockStateMachine = SetClock;
                    vTaskSuspend(xHeartbeatTaskHandle);
                }
                else
                {
                    if (ucButtonValue & BUTTON3LONGPRESSEDMASK)
                    {
                        eClockStateMachine = SetAlarmClock;
                    }
                }
                
                break;
            }
            case SetClock:
            {
                xSetTime (ucClockSetDigit, ucButtonValue, &ucClockSeconds, &ucClockMinutes, &ucClockHours);
                if (ucButtonValue & BUTTON2SHORTPRESSEDMASK)
                {
                    if (++ucClockSetDigit > 5)
                    {
                        eClockStateMachine = Clock;
                        vTaskResume(xHeartbeatTaskHandle);                  // if clock should count, then activate heartbeat task
                    }
                }
                else if (ucButtonValue & BUTTON2LONGPRESSEDMASK)
                {
                    ucClockSetDigit = (ucClockSetDigit > 0) ? (ucClockSetDigit - 1) : 0;
                }
                break;
            }
            case SetAlarmClock:
            {
                xSetTime(ucAlarmClockSetDigit, ucButtonValue, &ucAlarmClockSeconds, &ucAlarmClockMinutes, &ucAlarmClockHours);
                if (ucButtonValue & BUTTON2SHORTPRESSEDMASK)
                {
                    if (++ucAlarmClockSetDigit > 5)
                    {
                        eClockStateMachine = Clock;
                    }
                }
                else if (ucButtonValue & BUTTON2LONGPRESSEDMASK)
                {
                    ucAlarmClockSetDigit = (ucAlarmClockSetDigit > 0) ? (ucAlarmClockSetDigit - 1) : 0;
                }
                break;
            }
            default:
            {
                eClockStateMachine = Clock;
                break;
            }
        }
        
        vDisplayClear();
        vDisplayWriteStringAtPos(0,0,"Alarm Clock");
        sprintf(TimeString, "%02d:%02d:%02d", ucClockHours, ucClockMinutes, ucClockSeconds);
        vDisplayWriteStringAtPos(1,0,"  Time:  %s", TimeString);
        sprintf(TimeString, "%02d:%02d:%02d", ucAlarmClockHours, ucAlarmClockMinutes, ucAlarmClockSeconds);
        vDisplayWriteStringAtPos(2,0,"  Alarm: %s", TimeString);
        if (eClockStateMachine == SetClock)
        {
            vDisplayWriteStringAtPos(1,0," >");
        }
        else if (eClockStateMachine == SetAlarmClock)
        {
            vDisplayWriteStringAtPos(2,0," >");
        }
        if (ucAlarmActivated)
        {
            vDisplayWriteStringAtPos(3,0,"  Alarm enabled");
        }
        else
        {
            vDisplayWriteStringAtPos(3,0,"  Alarm disabled");
        }
        
        vTaskDelay(200 / portTICK_RATE_MS);
    }
}
/*———————————————————–*/


void xSetTime(uint8_t ucDigitToSet, uint8_t ucButtonValue, uint8_t *ucSeconds, uint8_t *ucMinutes, uint8_t *ucHours)
{
    
    switch (ucDigitToSet)
    {
        case 0:
        {
            if (ucButtonValue & BUTTON1SHORTPRESSEDMASK)
            {
                *ucSeconds = (*ucSeconds < 59) ? (*ucSeconds + 1) : 0;
            }
            else if (ucButtonValue & BUTTON1LONGPRESSEDMASK)
            {
                *ucSeconds = (*ucSeconds > 0) ? (*ucSeconds - 1) : 59;
            }
            break;
        }
        case 1:
        {
            if (ucButtonValue & BUTTON1SHORTPRESSEDMASK)
            {
                *ucSeconds = (*ucSeconds < 50) ? (*ucSeconds + 10) : ((*ucSeconds + 10) - 60);
            }
            else if (ucButtonValue & BUTTON1LONGPRESSEDMASK)
            {
                *ucSeconds = (*ucSeconds > 9) ? (*ucSeconds - 10) : ((*ucSeconds + 60) - 10);
            }
            break;
        }
        case 2:
        {
            if (ucButtonValue & BUTTON1SHORTPRESSEDMASK)
            {
                *ucMinutes = (*ucMinutes < 59) ? (*ucMinutes + 1) : 0;
            }
            else if (ucButtonValue & BUTTON1LONGPRESSEDMASK)
            {
                *ucMinutes = (*ucMinutes > 0) ? (*ucMinutes - 1) : 59;
            }
            break;
        }
        case 3:
        {
            if (ucButtonValue & BUTTON1SHORTPRESSEDMASK)
            {
                *ucMinutes = (*ucMinutes < 50) ? (*ucMinutes + 10) : ((*ucMinutes + 10) - 60);
            }
            else if (ucButtonValue & BUTTON1LONGPRESSEDMASK)
            {
                *ucMinutes = (*ucMinutes > 9) ? (*ucMinutes - 10) : ((*ucMinutes + 60) - 10);
            }
            break;
        }
        case 4:
        {
            if (ucButtonValue & BUTTON1SHORTPRESSEDMASK)
            {
                *ucHours = (*ucHours < 23) ? (*ucHours + 1) : 0;
            }
            else if (ucButtonValue & BUTTON1LONGPRESSEDMASK)
            {
                *ucHours = (*ucHours > 0) ? (*ucHours - 1) : 23;
            }
            break;
        }
        case 5:
        {
            if (ucButtonValue & BUTTON1SHORTPRESSEDMASK)
            {
                *ucHours = (*ucHours < 14) ? (*ucHours + 10) : ((*ucHours + 10) - 24);
            }
            else if (ucButtonValue & BUTTON1LONGPRESSEDMASK)
            {
                *ucHours = (*ucHours > 9) ? (*ucHours - 10) : ((*ucHours + 24) - 10);
            }
            break;
        }
        default:
        {
            break;
        }
    }
}
/*———————————————————–*/

void vHeartbeat(void)
{
    uint8_t ucMsCounter = 0;
    uint8_t ucMsInterruptCounter = 0;
    BaseType_t xInterruptOccurred = pdFALSE;
    
    while (1)
    {
        
        if (xInterruptOccurred == pdTRUE)               // process time counting only when interrupt occurred
        {
            xInterruptOccurred = pdFALSE;
            
            ucMsCounter += ucMsInterruptCounter;
            if (ucMsCounter >= 100)
            {
                ucMsCounter = 0;
                ++ucClockSeconds;
                if (ucClockSeconds >= 60)
                {
                    ucClockSeconds = 0;
                    ++ucClockMinutes;
                    if (ucClockMinutes >= 60)
                    {
                        ucClockMinutes = 0;
                        ++ucClockHours;
                        if (ucClockHours >= 24)
                        {
                            ucClockHours = 0;
                        }
                    }
                }
            }
            
            ucMsInterruptCounter = 0;
        }
        
        
        xInterruptOccurred = xTaskNotifyWait(0, 0xffffffff, &ucMsInterruptCounter, pdMS_TO_TICKS(50));
    }
}
/*———————————————————–*/

ISR(TCC1_OVF_vect)
{
    /* Occurs all 10ms */
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    xTaskNotifyFromISR(xHeartbeatTaskHandle, ucHeartbeatTimerCounter, eIncrement, &xHigherPriorityTaskWoken);
}
/*———————————————————–*/