/*
The MIT License (MIT)

Copyright (c) 2017 Lancaster University.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

/**
  * Class definition for Pin.
  *
  * Commonly represents an I/O pin on the edge connector.
  */
#include "NRF52Pin.h"
#include "Button.h"
#include "PulseIn.h"
#include "Timer.h"
#include "ErrorNo.h"
#include "nrf.h"
#include "EventModel.h"
#include "codal_target_hal.h"
#include "NotifyEvents.h"


using namespace codal;

#ifdef NRF_P1
#define PORT (name < 32 ? NRF_P0 : NRF_P1)
#define PIN ((name) & 31)
#define NUM_PINS 48
#else
#define PORT (NRF_P0)
#define PIN (name)
#define NUM_PINS 32
#endif

static NRF52Pin *irq_pins[NUM_PINS];

MemorySource* NRF52Pin::pwmSource = NULL;
NRF52PWM* NRF52Pin::pwm = NULL;
uint16_t NRF52Pin::pwmBuffer[NRF52PIN_PWM_CHANNEL_MAP_SIZE] = {0,0,0,0};
int8_t NRF52Pin::pwmChannelMap[NRF52PIN_PWM_CHANNEL_MAP_SIZE] = {-1,-1,-1,-1};
uint8_t NRF52Pin::lastUsedChannel = 3;

NRF52ADC* NRF52Pin::adc = NULL;
TouchSensor* NRF52Pin::touchSensor = NULL;

#ifdef __cplusplus
extern "C" {
#endif

static void process_gpio_irq(NRF_GPIO_Type* GPIO_PORT, int pinNumberOffset)
{
    uint32_t    pinNumber;
    uint32_t    latch;
    NRF52Pin    *pin;

    // Take a snapshot of the latched values.
    latch = GPIO_PORT->LATCH;

    // Handle any events raised on this port.
    while(latch)
    {
        // Determine the most significant pin that has changed.
        asm("mov r11, #31              \r\n"
            "clz r12, %[value]         \r\n"
            "sub %[result], r11, r12   \r\n"

                : [result] "=r" (pinNumber)
                : [value] "r" (latch)
                : "r11", "r12", "cc"
        );

        // Record that we have received this change event
        latch &= ~(1 << pinNumber);

        // Determine the NRF52Pin associated with this IRQ event
        pin = irq_pins[pinNumber + pinNumberOffset];

        // If that pin is registered for edge events
        if (pin)
        {
            if ( pin->status & (IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_INTERRUPT_ON_EDGE))
            {
                // Flip the sense bit of this pin to the opposite polarity (to sense the next edge)
                GPIO_PORT->PIN_CNF[pinNumber] ^= 0x00010000;

                // Invoke rise/fall handler in the pin according to the sensed polarity of this event
                if (GPIO_PORT->PIN_CNF[pinNumber] & 0x00010000)
                    pin->rise();
                else
                    pin->fall();
            }

            if ( pin->isWakeOnActive())
            {
                if ( fiber_scheduler_get_deepsleep_pending())
                    Event(DEVICE_ID_NOTIFY, POWER_EVT_CANCEL_DEEPSLEEP);
            }
        }
    }

    GPIO_PORT->LATCH = 0xffffffff;
}

void GPIOTE_IRQHandler(void)
{
    if (NRF_GPIOTE->EVENTS_PORT)
    {
        // Acknowledge the interrupt
        NRF_GPIOTE->EVENTS_PORT = 0;

        process_gpio_irq(NRF_P0, 0);
        process_gpio_irq(NRF_P1, 32);
    }
}

#ifdef __cplusplus
}
#endif

/**
  * Constructor.
  * Create a Pin instance, generally used to represent a pin on the edge connector.
  *
  * @param id the unique EventModel id of this component.
  *
  * @param name the mbed PinName for this Pin instance.
  *
  * @param capability the capabilities this Pin instance should have.
  *                   (PIN_CAPABILITY_DIGITAL, PIN_CAPABILITY_ANALOG, PIN_CAPABILITY_AD, PIN_CAPABILITY_ALL)
  *
  * @code
  * Pin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_ALL);
  * @endcode
  */
NRF52Pin::NRF52Pin(int id, PinNumber name, PinCapability capability) : codal::Pin(id, name, capability)
{
    this->pullMode = DEVICE_DEFAULT_PULLMODE;
    CODAL_ASSERT(name < NUM_PINS, 50);
    irq_pins[name] = this;

    NRF_GPIOTE->INTENSET    = GPIOTE_INTENSET_PORT_Set << GPIOTE_INTENSET_PORT_Pos;
    NVIC_EnableIRQ  (GPIOTE_IRQn);
}

/**
 * Record that a given peripheral has been connected to this pin.
 */
void NRF52Pin::connect(PinPeripheral &p, bool deleteOnRelease)
{
    // If we're already attached to a peripheral and we're being asked to connect to a new one,
    // then attempt to release the old peripheral.

    if (obj != &p)
    {
        if(obj)
            disconnect();

        Pin::connect(p, deleteOnRelease);
        obj = &p;
    }
}

/**
  * Disconnect any attached peripherals from this pin.
  *
  * Used only when pin changes mode (i.e. Input/Output/Analog/Digital)
  *
  * TODO: Update release code for ADC, PWM, I2C, SPI
  */
void NRF52Pin::disconnect()
{
    // Avoid any potential recursive loops caused by pin swaps within a single peripheral
    if (isDisconnecting())
        return;

    // Detach any on chip peripherals attached to this pin.
    if (obj && !obj->isPinLocked())
    {
        // Indicate that this pin is in the process of being disconnected.
        status |= IO_STATUS_DISCONNECTING;

        obj->releasePin(*this);

        // If we have previously allocated a PWM channel to this pin through setAnalogValue(), mark that PWM channel as free for future allocation.
        if (obj == pwm)
        {
            for (int i = 0; i < NRF52PWM_PWM_CHANNELS; i++)
                if (pwmChannelMap[i] == name)
                    pwmChannelMap[i] = -1;
        }

        obj = NULL;
    }

    // Disable any interrupts that may be attached to the pin GPIO state.
    PORT->PIN_CNF[PIN] &= ~(GPIO_PIN_CNF_SENSE_Msk);

    // Reset status flags to zero, but retain preferred TouchSense, Polarity and wake modes.
    status &= IO_STATUS_MODES;
}

/**
  * Configures this IO pin as a digital output (if necessary) and sets the pin to 'value'.
  *
  * @param value 0 (LO) or 1 (HI)
  *
  * @return DEVICE_OK on success, DEVICE_INVALID_PARAMETER if value is out of range, or DEVICE_NOT_SUPPORTED
  *         if the given pin does not have digital capability.
  *
  * @code
  * Pin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
  * 
  * P0.setDigitalValue(1); // P0 is now HI
  * @endcode
  */
int NRF52Pin::setDigitalValue(int value)
{
    if ((status & IO_STATUS_DIGITAL_OUT) && (!obj || obj->isPinLocked()))
    {
        if (value)
            PORT->OUTSET = 1 << PIN;
        else
            PORT->OUTCLR = 1 << PIN;

        return DEVICE_OK;
    }

    // We're changing mode. reset to a known state.
    disconnect();

    if (value)
        PORT->OUTSET = 1 << PIN;
    else
        PORT->OUTCLR = 1 << PIN;

    PORT->PIN_CNF[PIN] |= 1;

    status |= IO_STATUS_DIGITAL_OUT;

    return DEVICE_OK;
}

/**
  * Configures this IO pin as a digital input (if necessary) and tests its current value.
  *
  *
  * @return 1 if this input is high, 0 if input is LO, or DEVICE_NOT_SUPPORTED
  *         if the given pin does not have digital capability.
  *
  * @code
  * Pin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
  * 
  * P0.getDigitalValue(); // P0 is either 0 or 1;
  * @endcode
  */
int NRF52Pin::getDigitalValue()
{
    // Optimisation: Permit fast changes between digital in and digital out, given its common use case.
    // we also preserve any interrupt status, pulse measurement events etc.
    if ((status & IO_STATUS_DIGITAL_IN) && (!obj || obj->isPinLocked()))
        return (PORT->IN & (1 << PIN)) ? 1 : 0;

    // We're changing mode. reset to a known state.
    disconnect();

    // Enable input mode, and input buffer
    PORT->PIN_CNF[PIN] &= 0xfffffffc;

    // Record our mode, so we can optimise later.
    status |= IO_STATUS_DIGITAL_IN;

    // Ensure the current pull up/down configuration for this pin is applied.
    setPull(pullMode);

    // return the current state of the pin
    return (PORT->IN & (1 << PIN)) ? 1 : 0;
}

/**
 * Configures this IO pin as a digital input (if necessary) and tests its current value.
 *
 *
 * @return 1 if this input is high, 0 if input is LO, or DEVICE_NOT_SUPPORTED
 *         if the given pin does not have digital capability.
 *
 * @code
 * DevicePin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
 * 
 * P0.getDigitalValue(); // P0 is either 0 or 1;
 * @endcode
 */
int NRF52Pin::getDigitalValue(PullMode pull)
{
    setPull(pull);
    return getDigitalValue();
}
/**
 * Instantiates the components required for PWM if not previously created
 */
int NRF52Pin::initialisePWM()
{
    if (pwmSource == NULL)
    {
        pwmSource = new MemorySource();
        pwmSource->setFormat(DATASTREAM_FORMAT_16BIT_UNSIGNED);
    }

    if (pwm == NULL)
    {
        pwm = new NRF52PWM(NRF_PWM0, *pwmSource, 50);
        pwm->setStreamingMode(false);
    }

    return DEVICE_OK;
}

/**
  * Configures this IO pin as an analog/pwm output, and change the output value to the given level.
  *
  * @param value the level to set on the output pin, in the range 0 - 1024
  *
  * @return DEVICE_OK on success, DEVICE_INVALID_PARAMETER if value is out of range, or DEVICE_NOT_SUPPORTED
  *         if the given pin does not have analog capability.
  */
int NRF52Pin::setAnalogValue(int value)
{
    // //check if this pin has an analogue mode...
     if(!(PIN_CAPABILITY_ANALOG & capability))
         return DEVICE_NOT_SUPPORTED;

    // //sanitise the level value
    if(value < 0 || value > DEVICE_PIN_MAX_OUTPUT)
         return DEVICE_INVALID_PARAMETER;

    if (!(status & IO_STATUS_ANALOG_OUT))
        disconnect();

    int channel = -1;

    // find existing channel
    for (int i = 0; i < NRF52PIN_PWM_CHANNEL_MAP_SIZE; i++)
        if (pwmChannelMap[i] == name)
            channel = i;

    // no existing channel found
    if (channel == -1)
    {
        initialisePWM();

        // alloc new channel by round robin allocation
        channel = (lastUsedChannel + 1) % NRF52PIN_PWM_CHANNEL_MAP_SIZE;
        pwmChannelMap[channel] = name;
        lastUsedChannel = channel;
        pwm->connectPin(*this, channel);
    }

    if (obj == NULL || obj->isPinLocked() == false)
        status |= IO_STATUS_ANALOG_OUT;

    // set new value
    pwmBuffer[channel] = (int)((float)pwm->getSampleRange() * (1 - (float)value / (float)(DEVICE_PIN_MAX_OUTPUT+1)));
    pwmSource->playAsync(pwmBuffer, NRF52PIN_PWM_CHANNEL_MAP_SIZE * sizeof(uint16_t));

    return DEVICE_OK;
}

/**
 * Configures this IO pin as an analog/pwm output (if necessary) and configures the period to be 20ms,
 * with a duty cycle between 500 us and 2500 us.
 *
 * A value of 180 sets the duty cycle to be 2500us, and a value of 0 sets the duty cycle to be 500us by default.
 *
 * This range can be modified to fine tune, and also tolerate different servos.
 *
 * @param value the level to set on the output pin, in the range 0 - 180.
 *
 * @param range which gives the span of possible values the i.e. the lower and upper bounds (center +/- range/2). Defaults to DEVICE_PIN_DEFAULT_SERVO_RANGE.
 *
 * @param center the center point from which to calculate the lower and upper bounds. Defaults to DEVICE_PIN_DEFAULT_SERVO_CENTER
 *
 * @return DEVICE_OK on success, DEVICE_INVALID_PARAMETER if value is out of range, or DEVICE_NOT_SUPPORTED
 *         if the given pin does not have analog capability.
 */
int NRF52Pin::setServoValue(int value, int range, int center)
{
    //check if this pin has an analogue mode...
    if(!(PIN_CAPABILITY_ANALOG & capability))
        return DEVICE_NOT_SUPPORTED;

    //sanitise the servo level
    if(value < 0 || range < 1 || center < 1)
        return DEVICE_INVALID_PARAMETER;

    //clip - just in case
    if(value > DEVICE_PIN_MAX_SERVO_RANGE)
        value = DEVICE_PIN_MAX_SERVO_RANGE;

    //calculate the lower bound based on the midpoint
    int lower = (center - (range / 2)) * 1000;

    value = value * 1000;

    //add the percentage of the range based on the value between 0 and 180
    int scaled = lower + (range * (value / DEVICE_PIN_MAX_SERVO_RANGE));

    return setServoPulseUs(scaled / 1000);
}

/**
 * Configures this IO pin as an analogue input (if necessary), and samples the Pin for its analog value.
 *
 * @return the current analogue level on the pin, in the range 0 - 1024, or
 *         DEVICE_NOT_SUPPORTED if the given pin does not have analog capability.
 *
 * @code
 * DevicePin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
 * 
 * P0.getAnalogValue(); // P0 is a value in the range of 0 - 1024
 * @endcode
 */
int NRF52Pin::getAnalogValue()
{

    // //check if this pin has an analogue mode...
    if(!(PIN_CAPABILITY_ANALOG & capability))
        return DEVICE_NOT_SUPPORTED;

    // Move into an analogue input state if necessary.
    if (!(status & IO_STATUS_ANALOG_IN))
        disconnect();

    if (adc)
    {
        NRF52ADCChannel *c = adc->getChannel(*this);

        if (c)
        {
            if (obj == NULL || obj->isPinLocked() == false)
                status |= IO_STATUS_ANALOG_IN;

            return c->getSample() / 16;
        }
    }

    return DEVICE_NOT_SUPPORTED;
}

/**
  * Determines if this IO pin is currently configured as an input.
  *
  * @return 1 if pin is an analog or digital input, 0 otherwise.
  */
int NRF52Pin::isInput()
{
    return (status & (IO_STATUS_DIGITAL_IN | IO_STATUS_ANALOG_IN)) == 0 ? 0 : 1;
}

/**
  * Determines if this IO pin is currently configured as an output.
  *
  * @return 1 if pin is an analog or digital output, 0 otherwise.
  */
int NRF52Pin::isOutput()
{
    return (PORT->DIR & (1 << PIN)) != 0 || (status & (IO_STATUS_DIGITAL_OUT | IO_STATUS_ANALOG_OUT)) != 0;
}

/**
  * Determines if this IO pin is currently configured for digital use.
  *
  * @return 1 if pin is digital, 0 otherwise.
  */
int NRF52Pin::isDigital()
{
    return (status & (IO_STATUS_DIGITAL_IN | IO_STATUS_DIGITAL_OUT)) == 0 ? 0 : 1;
}

/**
  * Determines if this IO pin is currently configured for analog use.
  *
  * @return 1 if pin is analog, 0 otherwise.
  */
int NRF52Pin::isAnalog()
{
    return (status & (IO_STATUS_ANALOG_IN | IO_STATUS_ANALOG_OUT)) == 0 ? 0 : 1;
}

/**
 * Configures this IO pin as a "makey makey" style touch sensor (if necessary)
 * and tests its current debounced state.
 *
 * Users can also subscribe to DeviceButton events generated from this pin.
 *
 * @return 1 if pin is touched, 0 if not, or DEVICE_NOT_SUPPORTED if this pin does not support touch capability.
 *
 * @code
 * 
 * DeviceMessageBus bus;
 *
 * DevicePin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_ALL);
 * 
 * if(P0.isTouched())
 * 
 * {
 * 
 *     //do something!
 * 
 * }
 *
 * // subscribe to events generated by this pin!
 * 
 * bus.listen(DEVICE_ID_IO_P0, DEVICE_BUTTON_EVT_CLICK, someFunction);
 * @endcode
 */
int NRF52Pin::isTouched()
{
    // Maintain the last type of sensing used.
    return isTouched(status & IO_STATUS_CAPACITATIVE_TOUCH ? TouchMode::Capacitative : TouchMode::Resistive);
}


/**
 * Configures this IO pin as a "makey makey" style touch sensor (if necessary)
 * and tests its current debounced state.
 *
 * Users can also subscribe to DeviceButton events generated from this pin.
 * @param mode Type of sensing to use (resistive or capacitative)
 * @return 1 if pin is touched, 0 if not, or DEVICE_NOT_SUPPORTED if this pin does not support touch capability.
 *
 * @code
 * 
 * DeviceMessageBus bus;
 *
 * DevicePin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_ALL);
 * 
 * if(P0.isTouched())
 * 
 * {
 * 
 *    //do something!
 * 
 * }
 *
 * // subscribe to events generated by this pin!
 * 
 * bus.listen(DEVICE_ID_IO_P0, DEVICE_BUTTON_EVT_CLICK, someFunction);
 * @endcode
 */
int NRF52Pin::isTouched(TouchMode touchMode)
{
    //check if this pin has a touch mode...
    if(!(PIN_CAPABILITY_DIGITAL & capability))
        return DEVICE_NOT_SUPPORTED;

    // Move into a touch input state if necessary.
    TouchMode currentTouchMode = (status & IO_STATUS_CAPACITATIVE_TOUCH) ? TouchMode::Capacitative : TouchMode::Resistive;

    if ((status & IO_STATUS_TOUCH_IN) == 0 || touchMode != currentTouchMode)
    {
        disconnect();

        if (touchMode == TouchMode::Capacitative)
        {
            connect(*new TouchButton(*this, *touchSensor, CAPTOUCH_DEFAULT_CALIBRATION), true);
            status |= IO_STATUS_CAPACITATIVE_TOUCH;
        }
        else
        {
            // Configure this pin as a digital input.
            getDigitalValue();

            // Connect to a new Button instance.
            connect(*new Button(*this, id, DEVICE_BUTTON_ALL_EVENTS, ACTIVE_LOW, PullMode::None), true);
            status &= ~IO_STATUS_CAPACITATIVE_TOUCH;
        }

        status |= (IO_STATUS_TOUCH_IN | IO_STATUS_DIGITAL_IN);
    }

    if (touchMode == TouchMode::Capacitative)
        return ((TouchButton *)obj)->isPressed();
    
    return ((Button *)obj)->isPressed();
}


/**
 * Configures this pin as a "makey makey" style touch sensor (if required) and tests if at any point the pin has
 * been touched _since the last time_ this was called.
 *
 * Note that holding the pin in the 'touched' state will only generate one event, so this can be viewed as a kind
 * of 'falling edge' detection, where only a not-touched followed by a touched event must occur to increment the count.
 *
 * For this to work, the there must be two sequential `wasTouched` calls with no other pin
 * mode changes in between for this pin, otherwise the touch count will be reset.
 *
 * @return int The number of touch events since the last call.
 */
int NRF52Pin::wasTouched()
{
    // Maintain the last type of sensing used.
    return wasTouched(status & IO_STATUS_CAPACITATIVE_TOUCH ? TouchMode::Capacitative : TouchMode::Resistive);
}


/**
 * Configures this pin as a "makey makey" style touch sensor (if required) and tests if at any point the pin has
 * been touched _since the last time_ this was called.
 *
 * Note that holding the pin in the 'touched' state will only generate one event, so this can be viewed as a kind
 * of 'falling edge' detection, where only a not-touched followed by a touched event must occur to increment the count.
 *
 * For this to work, the there must be two sequential `wasTouched` calls with the same parameter used with no other pin
 * mode changes in between for this pin, otherwise the touch count will be reset.
 *
 * @param touchMode Which touch mode to use this pin in.
 * @return int The number of touch events since the last call.
 */
int NRF52Pin::wasTouched(TouchMode touchMode)
{
    TouchMode currentTouchMode = (status & IO_STATUS_CAPACITATIVE_TOUCH) ? TouchMode::Capacitative : TouchMode::Resistive;
    
    if ((status & IO_STATUS_TOUCH_IN) == 0 || touchMode != currentTouchMode)
        if( this->isTouched( touchMode ) == DEVICE_NOT_SUPPORTED )
            return DEVICE_NOT_SUPPORTED;
    
    if (touchMode == TouchMode::Capacitative)
        return ((TouchButton *)obj)->wasPressed();
    
    return ((Button *)obj)->wasPressed();
}

/**
 * If this pin is configured as a capacitative touch input, perform a calibration on the input.
 */
void NRF52Pin::touchCalibrate()
{
    if ((status & IO_STATUS_TOUCH_IN) && (status & IO_STATUS_CAPACITATIVE_TOUCH))
    {
        ((TouchButton *)obj)->calibrate();
    }
}


/**
  * Configures this IO pin as an analog/pwm output if it isn't already, configures the period to be 20ms,
  * and sets the pulse width, based on the value it is given.
  *
  * @param pulseWidth the desired pulse width in microseconds.
  *
  * @return DEVICE_OK on success, DEVICE_INVALID_PARAMETER if value is out of range, or DEVICE_NOT_SUPPORTED
  *         if the given pin does not have analog capability.
  */
int NRF52Pin::setServoPulseUs(uint32_t pulseWidth)
{
    initialisePWM();

    if (pwm->getPeriodUs() != 20000)
        pwm->setPeriodUs(20000);

    return setAnalogValue((int) (1024.0f * (float) pulseWidth / 20000.0f));
}

/**
  * Configures the PWM period of the analog output to the given value.
  *
  * @param period The new period for the analog output in microseconds.
  *
  * @return DEVICE_OK on success.
  */
int NRF52Pin::setAnalogPeriodUs(uint32_t period)
{
    if (status & IO_STATUS_ANALOG_OUT)
    {
        int oldRange = pwm->getSampleRange();
        pwm->setPeriodUs(period);

        for (int i = 0; i < NRF52PIN_PWM_CHANNEL_MAP_SIZE; i++)
        {
            float v = (float) pwmBuffer[i];
            v = v * (float)pwm->getSampleRange();
            v = v / (float) oldRange;

            pwmBuffer[i] = (uint16_t) v;
        }

        pwmSource->playAsync(pwmBuffer, NRF52PIN_PWM_CHANNEL_MAP_SIZE * sizeof(uint16_t));

        return DEVICE_OK;
    }

    return DEVICE_NOT_SUPPORTED;
}

/**
  * Configures the PWM period of the analog output to the given value.
  *
  * @param period The new period for the analog output in milliseconds.
  *
  * @return DEVICE_OK on success, or DEVICE_NOT_SUPPORTED if the
  *         given pin is not configured as an analog output.
  */
int NRF52Pin::setAnalogPeriod(int period)
{
    return setAnalogPeriodUs(period*1000);
}

/**
  * Obtains the PWM period of the analog output in microseconds.
  *
  * @return the period on success, or DEVICE_NOT_SUPPORTED if the
  *         given pin is not configured as an analog output.
  */
uint32_t NRF52Pin::getAnalogPeriodUs()
{
    if (status & IO_STATUS_ANALOG_OUT)
        return pwm->getPeriodUs();

    return DEVICE_NOT_SUPPORTED;
}

/**
  * Obtains the PWM period of the analog output in milliseconds.
  *
  * @return the period on success, or DEVICE_NOT_SUPPORTED if the
  *         given pin is not configured as an analog output.
  */
int NRF52Pin::getAnalogPeriod()
{
    return getAnalogPeriodUs()/1000;
}

/**
  * Configures the pull of this pin.
  *
  * @param pull one of the pull configurations: PullMode::Up, PullMode::Down, or PullMode::None.
  *
  * @return DEVICE_NOT_SUPPORTED if the current pin configuration is anything other
  *         than a digital input, otherwise DEVICE_OK.
  */
int NRF52Pin::setPull(PullMode pull)
{
    pullMode = pull;

    uint32_t s = PORT->PIN_CNF[PIN] & 0xfffffff3;

    if (pull == PullMode::Down)
        s |= 0x00000004;

    if (pull == PullMode::Up)
        s |= 0x0000000c;


    PORT->PIN_CNF[PIN] = s;

    return DEVICE_OK;
}

/**
  * This member function manages the calculation of the timestamp of a pulse detected
  * on a pin whilst in IO_STATUS_EVENT_PULSE_ON_EDGE or IO_STATUS_EVENT_ON_EDGE modes.
  *
  * @param eventValue the event value to distribute onto the message bus.
  */
void NRF52Pin::pulseWidthEvent(uint16_t eventValue)
{
    Event evt(id, eventValue, CREATE_ONLY);

    // we will overflow for pulses longer than 2^32us (over 1h)
    uint32_t now = (uint32_t)evt.timestamp;

    PulseIn *p = (PulseIn *)obj;

    if (p)
    {
        uint32_t diff = now - p->lastEdge;
        p->lastEdge = now;

        evt.timestamp = diff;
        evt.fire();
    }
}

void NRF52Pin::rise()
{
    if(status & IO_STATUS_EVENT_PULSE_ON_EDGE)
        pulseWidthEvent(DEVICE_PIN_EVT_PULSE_LO);

    if(status & IO_STATUS_EVENT_ON_EDGE)
        Event(id, DEVICE_PIN_EVT_RISE);

    if (status & IO_STATUS_INTERRUPT_ON_EDGE && gpio_irq)
        this->gpio_irq(1);
}

void NRF52Pin::fall()
{
    if(status & IO_STATUS_EVENT_PULSE_ON_EDGE)
        pulseWidthEvent(DEVICE_PIN_EVT_PULSE_HI);

    if(status & IO_STATUS_EVENT_ON_EDGE)
        Event(id, DEVICE_PIN_EVT_FALL);

    if (status & IO_STATUS_INTERRUPT_ON_EDGE && gpio_irq)
        this->gpio_irq(0);
}

/**
  * This member function will construct an TimedInterruptIn instance, and configure
  * interrupts for rise and fall.
  *
  * @param eventType the specific mode used in interrupt context to determine how an
  *                  edge/rise is processed.
  *
  * @return DEVICE_OK on success
  */
int NRF52Pin::enableRiseFallEvents(int eventType)
{
    bool enablePulseIn = false;

    // if we are in neither of the two modes, configure pin as a TimedInterruptIn.
    if (!(status & (IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_INTERRUPT_ON_EDGE)))
    {
        int v = getDigitalValue();

        // PORT->DETECTMODE = 1; // latched-detect

        PORT->PIN_CNF[PIN] &= ~(GPIO_PIN_CNF_SENSE_Msk);
        if (v)
            PORT->PIN_CNF[PIN] |= (GPIO_PIN_CNF_SENSE_Low << GPIO_PIN_CNF_SENSE_Pos);
        else
            PORT->PIN_CNF[PIN] |= (GPIO_PIN_CNF_SENSE_High << GPIO_PIN_CNF_SENSE_Pos);

        PORT->LATCH = 1 << PIN; // clear any pending latch
    }

    // If we are moving into a PULSE_ON_EDGE mode record that we need to start a pulse detector object
    if (!(status & IO_STATUS_EVENT_PULSE_ON_EDGE) && eventType == DEVICE_PIN_EVENT_ON_PULSE)
        enablePulseIn = true;

    // If we're moving out of pulse on edge mode (into plain edge detect mode), turn off the pulse detecor.
    if ((status & IO_STATUS_EVENT_PULSE_ON_EDGE) && eventType != DEVICE_PIN_EVENT_ON_PULSE)
    {
        obj->releasePin(*this);
        obj = NULL;
    }

    // Clear all state related to edge/pulse detection
    status &= ~(IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_INTERRUPT_ON_EDGE);

    // set our status bits accordingly.
    if(eventType == DEVICE_PIN_EVENT_ON_EDGE)
        status |= IO_STATUS_EVENT_ON_EDGE;
    else if(eventType == DEVICE_PIN_EVENT_ON_PULSE)
        status |= IO_STATUS_EVENT_PULSE_ON_EDGE;
    else if(eventType == DEVICE_PIN_INTERRUPT_ON_EDGE)
        status |= IO_STATUS_INTERRUPT_ON_EDGE;

    if (enablePulseIn)
    {
        // Create a new object to track pulse timing data.
        // Set the initial pulse edge to the current time in case the line is currently active.
        PulseIn *p = new PulseIn(*this);
        p->lastEdge = system_timer_current_time_us();
        connect(*p, true);
    }

    return DEVICE_OK;
}

/**
  * If this pin is in a mode where the pin is generating events, it will destruct
  * the current instance attached to this Pin instance.
  *
  * @return DEVICE_OK on success.
  */
int NRF52Pin::disableEvents()
{
    if (status & (IO_STATUS_EVENT_ON_EDGE | IO_STATUS_EVENT_PULSE_ON_EDGE | IO_STATUS_TOUCH_IN | IO_STATUS_INTERRUPT_ON_EDGE))
        disconnect();

    return DEVICE_OK;
}
/**
 * Configures the events generated by this DevicePin instance.
 *
 * DEVICE_PIN_EVENT_ON_EDGE - Configures this pin to a digital input, and generates events whenever a rise/fall is detected on this pin. (DEVICE_PIN_EVT_RISE, DEVICE_PIN_EVT_FALL)
 * 
 * DEVICE_PIN_EVENT_ON_PULSE - Configures this pin to a digital input, and generates events where the timestamp is the duration that this pin was either HI or LO. (DEVICE_PIN_EVT_PULSE_HI, DEVICE_PIN_EVT_PULSE_LO)
 * 
 * DEVICE_PIN_EVENT_ON_TOUCH - Configures this pin as a makey makey style touch sensor, in the form of a DeviceButton. Normal button events will be generated using the ID of this pin.
 * 
 * DEVICE_PIN_EVENT_NONE - Disables events for this pin.
 *
 * @param eventType One of: DEVICE_PIN_EVENT_ON_EDGE, DEVICE_PIN_EVENT_ON_PULSE, DEVICE_PIN_EVENT_ON_TOUCH, DEVICE_PIN_EVENT_NONE
 *
 * @code
 * DeviceMessageBus bus;
 *
 * DevicePin P0(DEVICE_ID_IO_P0, DEVICE_PIN_P0, PIN_CAPABILITY_BOTH);
 * 
 * P0.eventOn(DEVICE_PIN_EVENT_ON_PULSE);
 *
 * void onPulse(Event evt)
 * 
 * {
 * 
 *     int duration = evt.timestamp;
 * 
 * }
 *
 * bus.listen(DEVICE_ID_IO_P0, DEVICE_PIN_EVT_PULSE_HI, onPulse, MESSAGE_BUS_LISTENER_IMMEDIATE)
 * 
 * @endcode
 *
 * @return DEVICE_OK on success, or DEVICE_INVALID_PARAMETER if the given eventype does not match
 *
 * @note In the DEVICE_PIN_EVENT_ON_PULSE mode, the smallest pulse that was reliably detected was 85us, around 5khz. If more precision is required,
 *       please use the InterruptIn class supplied by ARM mbed.
 */
int NRF52Pin::eventOn(int eventType)
{
    switch(eventType)
    {
        case DEVICE_PIN_INTERRUPT_ON_EDGE:
        case DEVICE_PIN_EVENT_ON_EDGE:
        case DEVICE_PIN_EVENT_ON_PULSE:
            enableRiseFallEvents(eventType);
            break;

        case DEVICE_PIN_EVENT_ON_TOUCH:
            isTouched();
            break;

        case DEVICE_PIN_EVENT_NONE:
            disableEvents();
            break;

        default:
            return DEVICE_INVALID_PARAMETER;
    }

    return DEVICE_OK;
}

/**
 * Measures the period of the next digital pulse on this pin.
 * The polarity of the detected pulse is defined by setPolarity().
 * The calling fiber is blocked until a pulse is received or the specified
 * timeout passes.
 *
 * @param timeout The maximum period of time in microseconds to wait for a pulse.
 * @return the period of the pulse in microseconds, or DEVICE_CANCELLED on timeout.
 */
int
NRF52Pin::getPulseUs(int timeout)
{
    PulseIn *p;

    // ensure we're in digital input mode.
    getDigitalValue();

    if (!(status & IO_STATUS_EVENT_PULSE_ON_EDGE))
        eventOn(DEVICE_PIN_EVENT_ON_PULSE);

    p = (PulseIn *)obj;
    return p->awaitPulse(timeout);
}

/**
 * Configures this IO pin drive mode to based on the provided parameter.
 * Valid values are:
 *
 *  0 Standard '0', standard '1'
 *  1 High drive '0', standard '1'
 *  2 Standard '0', high drive '1'
 *  3 High drive '0', high 'drive '1''
 *  4 Disconnect '0' standard '1'
 *  5 Disconnect '0', high drive '1'
 *  6 Standard '0'. disconnect '1'
 *  7 High drive '0', disconnect '1'
 *
 * @param value the value to write to this pin's output drive configuration register
 */
int NRF52Pin::setDriveMode(int value)
{
    if (value < 0 || value > 7)
        return DEVICE_INVALID_PARAMETER;

    uint32_t s = PORT->PIN_CNF[PIN] & 0xfffff8ff;

    s |= (value << 8);

    PORT->PIN_CNF[PIN] = s;

    return DEVICE_OK;
}

/**
 * Configures this IO pin as a high drive pin (capable of sourcing/sinking greater current).
 * By default, pins are STANDARD drive.
 *
 * @param value true to enable HIGH DRIVE on this pin, false otherwise
 */
int NRF52Pin::setHighDrive(bool value)
{
    return setDriveMode(value ? 3 : 0);
}

/**
 * Determines if this IO pin is a high drive pin (capable of sourcing/sinking greater current).
 * By default, pins are STANDARD drive.
 *
 * @return true if HIGH DRIVE is enabled on this pin, false otherwise
 */
bool NRF52Pin::isHighDrive()
{
    uint32_t s = PORT->PIN_CNF[PIN] & 0x00000700;

    return (s == 0x00000300);
}

__attribute__((noinline))
static void get_and_set(NRF_GPIO_Type *port, uint32_t mask) {
    // 0 -> 1, only set when IN==0
    port->DIRSET = ~port->IN & mask;
}

__attribute__((noinline))
static void get_and_clr(NRF_GPIO_Type *port, uint32_t mask) {
    // 1 -> 0, only set when IN==1
    port->DIRSET = port->IN & mask;
}

int NRF52Pin::getAndSetDigitalValue(int value)
{
    uint32_t mask = 1 << PIN;

    if ((PORT->DIR & mask) == 0)
    {
        // set the value
        if (value)
            PORT->OUTSET = mask;
        else
            PORT->OUTCLR = mask;

        // pin in input mode, do the "atomic" set
        if (value)
            get_and_set(PORT, mask);
        else
            get_and_clr(PORT, mask);

        if (PORT->DIR & mask) {
            disconnect();
            setDigitalValue(value); // make sure 'status' is updated
            return 0;
        } else {

            return DEVICE_BUSY;
        }
    }

    return 0;
}

/**
 * Configures the Enables/Disables this pin's DETECT event
 * @param enable The new value of this pin's DETECT sense configuration
 * Valid values are GPIO_PIN_CNF_SENSE_Disabled, GPIO_PIN_CNF_SENSE_High, GPIO_PIN_CNF_SENSE_Low
 */
void NRF52Pin::setDetect(int enable)
{
    PORT->PIN_CNF[PIN] &= ~(GPIO_PIN_CNF_SENSE_Msk);
    PORT->PIN_CNF[PIN] |= (enable << GPIO_PIN_CNF_SENSE_Pos);
}
