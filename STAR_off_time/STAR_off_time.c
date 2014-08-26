/*
 * NANJG 105C Diagram
 *           ---
 *         -|   |- VCC
 *  Star 4 -|   |- Voltage ADC
 *  Star 3 -|   |- PWM
 *     GND -|   |- Star 2
 *           ---
 
 * CPU speed is 4.8Mhz without the 8x divider when low fuse is 0x75
 *
 * define F_CPU 4800000  CPU: 4.8MHz  PWM: 9.4kHz       ####### use low fuse: 0x75  #######
 *                             /8     PWM: 1.176kHz     ####### use low fuse: 0x65  #######
 * define F_CPU 9600000  CPU: 9.6MHz  PWM: 19kHz        ####### use low fuse: 0x7a  #######
 *                             /8     PWM: 2.4kHz       ####### use low fuse: 0x6a  #######
 * 
 * Above PWM speeds are for phase-correct PWM.  This program uses Fast-PWM, which when the CPU is 4.8MHz will be 18.75 kHz
 *
 * FUSES
 *		I use these fuse settings
 *		Low:  0x75
 *		High: 0xff
 *
 * STARS
 *		Star 2 = Moon if connected
 *		Star 3 = H-L if connected, L-H if not
 *		Star 4 = Capacitor for off-time
 *
 * VOLTAGE
 *		Resistor values for voltage divider (reference BLF-VLD README for more info)
 *		Reference voltage can be anywhere from 1.0 to 1.2, so this cannot be all that accurate
 *
 *           VCC
 *            |
 *           Vd (~.25 v drop from protection diode)
 *            |
 *          1912 (R1 19,100 ohms)
 *            |
 *            |---- PB2 from MCU
 *            |
 *          4701 (R2 4,700 ohms)
 *            |
 *           GND
 *
 *		ADC = ((V_bat - V_diode) * R2   * 255) / ((R1    + R2  ) * V_ref)
 *		125 = ((3.0   - .25    ) * 4700 * 255) / ((19100 + 4700) * 1.1  )
 *		121 = ((2.9   - .25    ) * 4700 * 255) / ((19100 + 4700) * 1.1  )
 *
 *		Well 125 and 121 were too close, so it shut off right after lowering to low mode, so I went with
 *		130 and 120
 *
 *		To find out what value to use, plug in the target voltage (V) to this equation
 *			value = (V * 4700 * 255) / (23800 * 1.1)
 *      
 */
#define F_CPU 4800000UL

/*
 * =========================================================================
 * Settings to modify per driver
 */

#define VOLTAGE_MON			// Comment out to disable

#define MEMORY				// Comment out to disable

// Levels should start around 10 with Fast PWM

#define MODE_MOON			13	// Can comment out to remove mode, but should be set through soldering stars
#define MODE_LOW			20  // Can comment out to remove mode
#define MODE_MED			83	// Can comment out to remove mode
#define MODE_HIGH_W_TURBO	180	// MODE_HIGH value when turbo is enabled
#define MODE_HIGH			255	// Can comment out to remove mode
//#define MODE_TURBO			255	// Can comment out to remove mode
#define TURBO_TIMEOUT		180 // How many WTD ticks before before dropping down (.5 sec each)

#define ADC_LOW				130	// When do we start ramping
#define ADC_CRIT			120 // When do we shut the light off

#define CAP_THRESHOLD		130  // Value between 1 and 255 corresponding with cap voltage (0 - 1.1v) where we consider it a short press to move to the next mode
								 // Not sure the lowest you can go before getting bad readings, but with a value of 70 and a 1uF cap, it seemed to switch sometimes
								 // even when waiting 10 seconds between presses.

/*
 * =========================================================================
 */

#ifdef MODE_TURBO
#undef  MODE_HIGH
#define MODE_HIGH	MODE_HIGH_W_TURBO
#endif

//#include <avr/pgmspace.h>
#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>	
#include <avr/eeprom.h>
#include <avr/sleep.h>
//#include <avr/power.h>

#define STAR2_PIN   PB0
#define STAR3_PIN   PB4
#define CAP_PIN     PB3
#define CAP_CHANNEL 0x03	// MUX 03 corresponds with PB3 (Star 4)
#define CAP_DIDR    ADC3D	// Digital input disable bit corresponding with PB3
#define PWM_PIN     PB1
#define VOLTAGE_PIN PB2
#define ADC_CHANNEL 0x01	// MUX 01 corresponds with PB2
#define ADC_DIDR 	ADC1D	// Digital input disable bit corresponding with PB2
#define ADC_PRSCL   0x06	// clk/64
#define ADC_LOW     130
#define ADC_CRIT	120

#define PWM_LVL		OCR0B	// OCR0B is the output compare register for PB1

/*
 * global variables
 */

// Mode storage
uint8_t eepos = 0;
uint8_t eep[32];
uint8_t memory = 0;

// Modes (gets set when the light starts up based on stars)
static uint8_t modes[10];  // Don't need 10, but keeping it high enough to handle all
volatile uint8_t mode_idx = 0;
int     mode_dir = 0; // 1 or -1. Determined when checking stars. Do we increase or decrease the idx when moving up to a higher mode.
uint8_t mode_cnt = 0;

uint8_t lowbatt_cnt = 0;

void store_mode_idx(uint8_t lvl) {  //central method for writing (with wear leveling)
	uint8_t oldpos=eepos;
	eepos=(eepos+1)&31;  //wear leveling, use next cell
	// Write the current mode
	EEARL=eepos;  EEDR=lvl; EECR=32+4; EECR=32+4+2;  //WRITE  //32:write only (no erase)  4:enable  2:go
	while(EECR & 2); //wait for completion
	// Erase the last mode
	EEARL=oldpos;           EECR=16+4; EECR=16+4+2;  //ERASE  //16:erase only (no write)  4:enable  2:go
}
inline void read_mode_idx() {
	eeprom_read_block(&eep, 0, 32);
	while((eep[eepos] == 0xff) && (eepos < 32)) eepos++;
	if (eepos < 32) mode_idx = eep[eepos];//&0x10; What the?
	else eepos=0;
}

inline void next_mode() {
	if (mode_idx == 0 && mode_dir == -1) {
		// Wrap around
		mode_idx = mode_cnt - 1;
	} else {
		mode_idx += mode_dir;
		if (mode_idx > (mode_cnt - 1)) {
			// Wrap around
			mode_idx = 0;
		}
	}
}

inline void check_stars() {
	// Load up the modes based on stars
	// Always load up the modes array in order of lowest to highest mode
	// 0 being low for soldered, 1 for pulled-up for not soldered
	// Moon
#ifdef MODE_MOON
	if ((PINB & (1 << STAR2_PIN)) == 0) {
		modes[mode_cnt++] = MODE_MOON;
	}
#endif
#ifdef MODE_LOW
	modes[mode_cnt++] = MODE_LOW;
#endif
#ifdef MODE_MED
	modes[mode_cnt++] = MODE_MED;
#endif
#ifdef MODE_HIGH
	modes[mode_cnt++] = MODE_HIGH;
#endif
#ifdef MODE_TURBO
	modes[mode_cnt++] = MODE_TURBO;
#endif
	if ((PINB & (1 << STAR3_PIN)) == 0) {
		// High to Low
		mode_dir = -1;
	} else {
		mode_dir = 1;
	}
}

inline void WDT_on() {
	// Setup watchdog timer to only interrupt, not reset, every 500ms.
	cli();							// Disable interrupts
	wdt_reset();					// Reset the WDT
	WDTCR |= (1<<WDCE) | (1<<WDE);  // Start timed sequence
	WDTCR = (1<<WDTIE) | (1<<WDP2) | (1<<WDP0); // Enable interrupt every 500ms
	sei();							// Enable interrupts
}

inline void WDT_off()
{
	cli();							// Disable interrupts
	wdt_reset();					// Reset the WDT
	MCUSR &= ~(1<<WDRF);			// Clear Watchdog reset flag
	WDTCR |= (1<<WDCE) | (1<<WDE);  // Start timed sequence
	WDTCR = 0x00;					// Disable WDT
	sei();							// Enable interrupts
}

inline void ADC_on() {
	DIDR0 |= (1 << ADC_DIDR);							// disable digital input on ADC pin to reduce power consumption
	ADMUX  = (1 << REFS0) | (1 << ADLAR) | ADC_CHANNEL; // 1.1v reference, left-adjust, ADC1/PB2
	ADCSRA = (1 << ADEN ) | (1 << ADSC ) | ADC_PRSCL;   // enable, start, prescale
}

inline void ADC_off() {
	ADCSRA &= ~(1<<7); //ADC off
}

#ifdef VOLTAGE_MON
uint8_t low_voltage(uint8_t voltage_val) {
	// Start conversion
	ADCSRA |= (1 << ADSC);
	// Wait for completion
	while (ADCSRA & (1 << ADSC));
	// See if voltage is lower than what we were looking for
	if (ADCH < voltage_val) {
		// See if it's been low for a while
		if (++lowbatt_cnt > 8) {
			lowbatt_cnt = 0;
			return 1;
		}
	} else {
		lowbatt_cnt = 0;
	}
	return 0;
}
#endif

ISR(WDT_vect) {
	static uint8_t ticks = 0;
	if (ticks < 255) ticks++;
	
#ifdef MODE_TURBO	
	//if (ticks == TURBO_TIMEOUT && modes[mode_idx] == MODE_TURBO) { // Doesn't make any sense why this doesn't work
	if (ticks == TURBO_TIMEOUT && mode_idx == (mode_cnt - 1)) {
		// Turbo mode is always at end
		PWM_LVL = modes[--mode_idx];
		store_mode_idx(mode_idx);
	}
#endif

}

int main(void)
{	
	// All ports default to input, but turn pull-up resistors on for the stars (not the ADC input!  Made that mistake already)
	PORTB = (1 << STAR2_PIN) | (1 << STAR3_PIN);
	
	// Determine what mode we should fire up
	// Read the last mode that was saved
	read_mode_idx();
	
	check_stars(); // Moving down here as it might take a bit for the pull-up to turn on?
	
	// Start up ADC for capacitor pin
	DIDR0 |= (1 << CAP_DIDR);							// disable digital input on ADC pin to reduce power consumption
	ADMUX  = (1 << REFS0) | (1 << ADLAR) | CAP_CHANNEL; // 1.1v reference, left-adjust, ADC3/PB3
	ADCSRA = (1 << ADEN ) | (1 << ADSC ) | ADC_PRSCL;   // enable, start, prescale
	
	// Wait for completion
	while (ADCSRA & (1 << ADSC));
	// Start again as datasheet says first result is unreliable
	ADCSRA |= (1 << ADSC);
	// Wait for completion
	while (ADCSRA & (1 << ADSC));
	if (ADCH > CAP_THRESHOLD) {
		// Indicates they did a short press, go to the next mode
		next_mode(); // Will handle wrap arounds
		store_mode_idx(mode_idx);
	} else {
		// Didn't have a short press, keep the same mode
	#ifndef MEMORY
		// Reset to the first mode
		mode_idx = ((mode_dir == 1) ? 0 : (mode_cnt - 1));
		store_mode_idx(mode_idx);
	#endif
	}
	// Turn off ADC
	ADC_off();
	
	// Charge up the capacitor by setting CAP_PIN to output
	DDRB  |= (1 << CAP_PIN);	// Output
    PORTB |= (1 << CAP_PIN);	// High
	
    // Set PWM pin to output
    DDRB |= (1 << PWM_PIN);

    // Set timer to do PWM for correct output pin and set prescaler timing
    TCCR0A = 0x23; // phase corrected PWM is 0x21 for PB1, fast-PWM is 0x23
    TCCR0B = 0x01; // pre-scaler for timer (1 => 1, 2 => 8, 3 => 64...)
	
	// Turn features on or off as needed
	#ifdef VOLTAGE_MON
	ADC_on();
	#else
	ADC_off();
	#endif
	ACSR   |=  (1<<7); //AC off
	
	// Enable sleep mode set to Idle that will be triggered by the sleep_mode() command.
	// Will allow us to go idle between WDT interrupts
	set_sleep_mode(SLEEP_MODE_IDLE);
	
	uint8_t prev_mode_idx = mode_idx;
	
	WDT_on();
	
	// Now just fire up the mode
	PWM_LVL = modes[mode_idx];
	
	uint8_t i = 0;
	uint8_t hold_pwm;
	while(1) {
	#ifdef VOLTAGE_MON
		if (low_voltage(ADC_LOW)) {
			// We need to go to a lower level
			if (mode_idx == 0 && PWM_LVL <= modes[mode_idx]) {
				// Can't go any lower than the lowest mode
				// Wait until we hit the critical level before flashing 10 times and turning off
				while (!low_voltage(ADC_CRIT));
				i = 0;
				while (i++<10) {
					PWM_LVL = 0;
					_delay_ms(250);
					PWM_LVL = modes[0];
					_delay_ms(500);
				}
				// Turn off the light
				PWM_LVL = 0;
				// Disable WDT so it doesn't wake us up
				WDT_off();
				// Power down as many components as possible
				set_sleep_mode(SLEEP_MODE_PWR_DOWN);
				sleep_mode();
			} else {
				// Flash 3 times before lowering
				hold_pwm = PWM_LVL;
				i = 0;
				while (i++<3) {
					PWM_LVL = 0;
					_delay_ms(250);
					PWM_LVL = hold_pwm;
					_delay_ms(500);
				}
				// Lower the mode by half, but don't go below lowest level
				if ((PWM_LVL >> 1) < modes[0]) {
					PWM_LVL = modes[0];
					mode_idx = 0;
				} else {					
					PWM_LVL = (PWM_LVL >> 1);
				}					
				// See if we should change the current mode level if we've gone under the current mode.
				if (PWM_LVL < modes[mode_idx]) {
					// Lower our recorded mode
					mode_idx--;
				}
			}
			// Wait 3 seconds before lowering the level again
			_delay_ms(3000);
		}
	#endif
		sleep_mode();
	}

    return 0; // Standard Return Code
}