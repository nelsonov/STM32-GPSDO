/*******************************************************************************************************
  GPSDO v0.03a by André Balsa, May 2021
  reuses pieces of the excellent GPS checker code Arduino sketch by Stuart Robinson - 05/04/20
  From version 0.03 includes a command parser, meaning it can receive commands from the USB serial or
  Bluetooth serial interfaces and execute a callback function.

  This program is supplied as is, it is up to the user of the program to decide if the program is
  suitable for the intended purpose and free from errors.
*******************************************************************************************************/

// GPSDO with optional I2C SSD1306 display, STM32 MCU, DFLL in software

/*******************************************************************************************************
  Program Operation -  This Arduino sketch implements a GPSDO with display option. It uses an SSD1306 
  128x64 I2C OLED display. It reads the GPS for 5 seconds and copies the characters from the GPS
  to the serial monitor, this is an example printout from a working GPS that has just been powered on;
   
  GPSDO Starting
  Wait GPS Fix 5 seconds
  Timeout - No GPS Fix 5s
  Wait GPS Fix 5 seconds
  $PGACK,103*40
  $PGACK,105*46
  $PMTK011,MTKGPS*08
  $PMTK010,001*2E
  $PMTK010,00æ*2D
  $GPGGA,235942.800,,,,,0,0,,,M,,M,,*4B
  $GPGSA,A,1,,,,,,,,,,,,,,,*1E
  $GPRMC,235942.800,V,,,,,0.00,0.00,050180,,,N*42
  $GPVTG,0.00,T,,M,0.00,N,0.00,K,N*32
  $GPGSV,1,1,03,30,,,43,07,,,43,05,,,38*70

  Timeout - No GPS Fix 5s
  Wait GPS Fix 5 seconds

  That printout is from a Meadiatek GPS, the Ublox ones are similar. The data from the GPS is also fed into
  the TinyGPS++ library and if there is no fix a message is printed on the serial monitor.

  When the program detects that the GPS has a fix, it prints the Latitude, Longitude, Altitude, Number
  of satellites in use, the HDOP value, time and date to the serial monitor. If the I2C OLED display is
  attached that is updated as well.

  Serial monitor baud rate is set at 115200.
*******************************************************************************************************/
/* Libraries required to compile:
    - TinyGPS++
    - U8g2/u8x8 graphics library
    - Adafruit AHTX0
    - Adafruit BMP280

    For commands parsing, uses SerialCommands library found here:
    https://github.com/ppedro74/Arduino-SerialCommands

   And also requires the installation of support for the STM32 MCUs by installing the STM32duino
   package (STM32 core version 2.0.0 or later).
*******************************************************************************************************/
/* Commands implemented:
    - V : returns program name, version and author

/*******************************************************************************************************
  Program Operation -  This program is a GPSDO with optional OLED display. It uses a small SSD1306
  128x64 I2C OLED display. At startup the program starts checking the data coming from the GPS for a
  valid fix. It reads the GPS NMEA stream for 5 seconds and if there is no fix, prints a message on the
  Arduino IDE serial monitor and updates the seconds without a fix on the display. During this time the
  NMEA stream coming from the GPS is copied to the serial monitor also. The DFLL is active as soon as
  the GPS starts providing a 1PPS pulse. The 10MHz OCXO is controlled by a voltage generated by the
  I2C DAC, which is adjusted once every 429 seconds.
*******************************************************************************************************/

// Enabling the INA219 sensor using the LapINA219 library causes the firmware to lock up after a few minutes
// I have not identified the cause, it could be the library, or a hardware issue, or I have a bad sensor, etc.
// Requires further testing with another INA219 sensor, or another library.
// Note that leaving the INA219 sensor on the I2C bus without otherwise reading from / writing to it, does
// not cause any lock up.

#define Program_Name "GPSDO"
#define Program_Version "v0.03a"
#define Author_Name "André Balsa"

// Define optional modules
// -----------------------
#define GPSDO_AHT10           // I2C temperature and humidity sensor
#define GPSDO_GEN_2kHz        // generate 2kHz square wave test signal on pin PB9 using Timer 4
#define GPSDO_BMP280_SPI      // SPI atmospheric pressure, temperature and altitude sensor
// #define GPSDO_INA219          // INA219 I2C current and voltage sensor
#define GPSDO_BLUETOOTH       // Bluetooth serial (HC-06 module)
#define GPSDO_VCC             // Vcc (nominal 5V) ; reading Vcc requires 1:2 voltage divider to PA0
#define GPSDO_VDD             // Vdd (nominal 3.3V) reads VREF internal ADC channel
// #define GPSDO_VERBOSE_NMEA    // GPS module NMEA stream echoed to USB serial, Bluetooth serial

// Includes
// --------
#if !defined(STM32_CORE_VERSION) || (STM32_CORE_VERSION  < 0x02000000)
#error "Due to API change, this sketch is compatible with STM32_CORE_VERSION  >= 0x02000000"
#endif

#include <SerialCommands.h>                        // Commands parser
// 32 char buffer, listens on USB serial
char serial_command_buffer_[32];
SerialCommands serial_commands_(&Serial, serial_command_buffer_, sizeof(serial_command_buffer_), "\r\n", " ");

#ifdef GPSDO_BLUETOOTH
//              UART    RX   TX
HardwareSerial Serial2(PA3, PA2);                  // Serial connection to HC-06 Bluetooth module
#endif // BLUETOOTH

#include <TinyGPS++.h>                             // get library here > http://arduiniana.org/libraries/tinygpsplus/
TinyGPSPlus gps;                                   // create the TinyGPS++ object

#include <Wire.h>                                  // Hardware I2C library on STM32

#ifdef GPSDO_AHT10
#include <Adafruit_AHTX0.h>                        // Adafruit AHTX0 library
Adafruit_AHTX0 aht;                                // create object aht
#endif // AHT10

#ifdef GPSDO_INA219
#include <LapINA219.h>                             // LapINA219 library library
LapINA219 ina219(0x40);                            // create object ina219 with I2C address 0x40
float ina219volt=0.0, ina219curr=0.0;
#endif // INA219

#include <U8x8lib.h>                                      // get library here >  https://github.com/olikraus/u8g2 
U8X8_SSD1306_128X64_NONAME_HW_I2C disp(U8X8_PIN_NONE);    // use this line for standard 0.96" SSD1306

#include <Adafruit_MCP4725.h>                      // MCP4725 Adafruit library
Adafruit_MCP4725 dac;
const uint16_t default_DAC_output = 2382; // this varies from OCXO to OCXO, and with time and temperature
                                          // Some values I have been using:
                                          // 2603 for an ISOTEMP 143-141, determined empirically
                                          // 2549 for a CTI OSC5A2B02, determined empirically
                                          // 2382 for an NDK ENE3311B, determined empirically
uint16_t adjusted_DAC_output;             // we adjust this value to "close the loop" of the DFLL
volatile bool must_adjust_DAC = false;    // true when there is enough data to adjust Vctl

#define VctlInputPin PB0
int adcVctl = 0;                      // Vctl read by ADC pin PB0

#ifdef GPSDO_VCC
#define VccDiv2InputPin PA0           // Vcc/2 using resistor divider connects to PA0
int adcVcc = 0;
#endif // VCC

#ifdef GPSDO_VDD
int adcVdd = 0;                      // Vdd is read internally as Vref
#endif // VDD

#ifdef GPSDO_BMP280_SPI
// BMP280 - SPI
#include <SPI.h>
#include <Adafruit_BMP280.h>
#define BMP280_CS   (PA4)              // SPI1 uses PA4, PA5, PA6, PA7
Adafruit_BMP280 bmp(BMP280_CS);        // hardware SPI, use PA4 as Chip Select
const uint16_t PressureOffset = 1360;  // that offset must be calculated for your sensor and location
float bmp280temp=0.0, bmp280pres=0.0, bmp280alti=0.0; // read sensor, save here
#endif // BMP280_SPI

// LEDs
// Blue onboard LED blinks to indicate ISR is working
#define blueledpin  PC13    // Blue onboard LED is on PC13 on STM32F411CEU6 Black Pill
// Yellow extra LED is off, on or blinking to indicate some GPSDO status
#define yellowledpin PB1   // Yellow LED on PB1
volatile int yellow_led_state = 2;  // global variable 0=off 1=on 2=1Hz blink

// GPS data
float GPSLat;                                      // Latitude from GPS
float GPSLon;                                      // Longitude from GPS
float GPSAlt;                                      // Altitude from GPS
uint8_t GPSSats;                                   // number of GPS satellites in use
uint32_t GPSHdop;                                  // HDOP from GPS
uint8_t hours, mins, secs, day, month;
uint16_t year;
uint32_t startGetFixmS;
uint32_t endFixmS;

// Uptime data
volatile uint8_t  uphours = 0;
volatile uint8_t  upminutes = 0;
volatile uint8_t  upseconds = 0;
volatile uint16_t updays = 0;
volatile bool halfsecond = false;
char uptimestr[9] = "00:00:00";    // uptime string
char updaysstr[5] = "000d";        // updays string

// OCXO frequency measurement 
volatile uint32_t fcount=0, previousfcount=0, calcfreqint=10000000;

/* Moving average frequency variables
   Basically we store the counter captures for 10 and 100 seconds.
   When the buffers are full, the average frequency is quite simply
   the difference between the oldest and newest data divided by the size
   of the buffer.
   Each second, when the buffers are full, we overwrite the oldest data
   with the newest data and calculate each average frequency.
 */
volatile uint32_t circbuf_ten[11]; // 10+1 seconds circular buffer
volatile uint32_t circbuf_hun[101]; // 100+1 seconds circular buffer

volatile uint32_t cbiten_newest=0; // index to oldest, newest data
volatile uint32_t cbihun_newest=0;

volatile bool cbTen_full=false, cbHun_full=false;  // flag when buffer full
double avgften=0, avgfhun=0; // average frequency calculated once the buffer is full

// SerialCommands callback functions
// This is the default handler, and gets called when no other command matches. 
void cmd_unrecognized(SerialCommands* sender, const char* cmd)
{
  sender->GetSerial()->print("Unrecognized command [");
  sender->GetSerial()->print(cmd);
  sender->GetSerial()->println("]");
}

// called for V (version) command
void cmd_version(SerialCommands* sender)
{
  sender->GetSerial()->println("GPSDO - v0.03a by André Balsa");
}

//Note: Commands are case sensitive
SerialCommand cmd_version_("V", cmd_version);


// Interrupt Service Routine for the 2Hz timer
void Timer_ISR_2Hz(void) // WARNING! Do not attempt I2C communication inside the ISR

{ // Toggle pin. 2hz toogle --> 1Hz pulse, perfect 50% duty cycle
  digitalWrite(blueledpin, !digitalRead(blueledpin));

  halfsecond = !halfsecond; // true @ 1Hz

  // read TIM2->CCR3 twice per second and if it has changed, calculate OCXO frequency

  fcount = TIM2->CCR3;
  
  if ((fcount > 4000000000) && (fcount < 4010000000) && halfsecond) must_adjust_DAC = true; else must_adjust_DAC = false; // once every 429s
  
  if (fcount < 4280000000) { // if we are way below wraparound value (2^32)
    if (fcount > previousfcount) {  // if we have a new count - that happens once per second
      if (((fcount - previousfcount) > 9999800) && ((fcount - previousfcount) < 10000200)) { // if we have a valid fcount, otherwise it's discarded
        logfcount();  // save fcount in the ring buffers
        calcfreqint = fcount - previousfcount; // the difference is exactly the OCXO frequency in Hz
        // previousfcount = fcount;
      }
      previousfcount = fcount;
    }
  } else { // prepare for wraparound every 429 seconds
    TIM2->CCR3 = 0x0; // clear CCR3 (no need to stop counter), perhaps this is not needed
    cbTen_full=false; cbHun_full=false; // we also need to refill the ring buffers
    cbiten_newest=0; cbihun_newest=0;
    previousfcount = 0;
  }                           

  switch (yellow_led_state)
  {
    case 0:
      // turn off led
      digitalWrite(yellowledpin, LOW);
      break;
    case 1:
      // turn on led
      digitalWrite(yellowledpin, HIGH);
      break;
    case 2:
      // blink led
      digitalWrite(yellowledpin, !digitalRead(yellowledpin));
      break;
    default:
      // default is to turn off led
      digitalWrite(yellowledpin, LOW);
      break; 
  }
  
  // Uptime clock - in days, hours, minutes, seconds
  if (halfsecond)
  {
      if (++upseconds > 59)
      {
          upseconds = 0;
          if (++upminutes > 59)
          {
              upminutes = 0;
              if (++uphours > 23)
              {
                  uphours = 0;
                  ++updays;
              }
          }
      }
  }  
}

void logfcount() // called once per second from ISR to update all the ring buffers
{
  // 10 seconds buffer
  circbuf_ten[cbiten_newest]=fcount;
  cbiten_newest++;
  if (cbiten_newest > 10) {
     cbTen_full=true; // that only needs to happen once, when the buffer fills up for the first time
     cbiten_newest = 0;   // (wrap around)
  }
  // 100 seconds buffer
  circbuf_hun[cbihun_newest]=fcount;
  cbihun_newest++;
  if (cbihun_newest > 100) {
     cbHun_full=true; // that only needs to happen once, when the buffer fills up for the first time
     cbihun_newest = 0;   // (wrap around)
  }
}


void setup()
{
  // setup commands parser
  serial_commands_.SetDefaultHandler(cmd_unrecognized);
  serial_commands_.AddCommand(&cmd_version_);
    
  // Setup 2Hz Timer
  HardwareTimer *tim2Hz = new HardwareTimer(TIM9);
  
  // configure blueledpin in output mode
  pinMode(blueledpin, OUTPUT);

  // configure yellow_led_pin in output mode
  pinMode(yellowledpin, OUTPUT);    
  
  tim2Hz->setOverflow(2, HERTZ_FORMAT); // 2 Hz
  tim2Hz->attachInterrupt(Timer_ISR_2Hz);
  tim2Hz->resume();

  // Setup serial interfaces
  Serial1.begin(9600);  // Hardware serial to GPS module
  Serial.begin(115200); // USB serial
  #ifdef GPSDO_BLUETOOTH
  // HC-06 module baud rate factory setting is 9600, 
  // use separate program to set baud rate to 115200
  Serial2.begin(115200);
  #endif // BLUETOOTH

  Serial.println();
  Serial.print(F(__TIME__));
  Serial.print(F(" "));
  Serial.println(F(__DATE__));
  Serial.println(F(Program_Name));
  Serial.println(F(Program_Version));
  Serial.println();

  // Setup OLED I2C display
  // Note that u8x8 library initializes I2C hardware interface
  disp.setBusClock(400000L); // try to avoid display locking up
  disp.begin();
  disp.setFont(u8x8_font_chroma48medium8_r);
  disp.clear();
  disp.setCursor(0, 0);
  disp.print(F("GPSDO - v0.03a"));

  // Initialize I2C again (not sure this is needed, though)
  Wire.begin();
  // try setting a higher I2C clock speed
  Wire.setClock(400000L); 

  #ifdef GPSDO_INA219
  ina219.begin();                           // calibrates ina219 sensor
  #endif // INA219 

  // Setup I2C DAC, read voltage on PB0
  adjusted_DAC_output = default_DAC_output; // initial DAC value
  dac.begin(0x60);
  // Output Vctl to DAC, but do not write to DAC EEPROM 
  dac.setVoltage(adjusted_DAC_output, false); // min=0 max=4096 so 2048 should be 1/2 Vdd = approx. 1.65V
  analogReadResolution(12); // make sure we read 12 bit values when we read from PB0

  #ifdef GPSDO_AHT10
  if (! aht.begin()) {
    Serial.println("Could not find AHT10? Check wiring");
    while (1) delay(10);
  }
  Serial.println("AHT10 found");
  #endif // AHT10

  #ifdef GPSDO_GEN_2kHz
  // generate a test 2kHz square wave on PB9 PWM pin - because we can and Timer 4 is available
  analogWriteFrequency(2000); // default PWM frequency is 1kHz, change it to 2kHz
  analogWrite(PB9, 127); // 127 means 50% duty cycle so a square wave
  #endif // GEN_2kHz

  #ifdef GPSDO_BMP280_SPI
  // Initialize BMP280
  if (!bmp.begin()) {
    Serial.println(F("Could not find a valid BMP280 sensor, check wiring or "
                      "try a different address!"));
    while (1) delay(10);
  }

  // Default settings from datasheet.
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X2,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_500); /* Standby time. */
  #endif // BMP280_SPI
  
  Serial.println(F("GPSDO Starting"));
  Serial.println();

  startGetFixmS = millis();

  // Setup and start Timer 2 which measures OCXO frequency
  
  // setup pin used as ETR (10MHz external clock from OCXO)
  pinMode(PA15, INPUT_PULLUP);    // setup PA15 as input pin
  pinModeAF(PA15, GPIO_AF1_TIM2); // setup PA15 as TIM2 channel 1 / ETR
  
  // setup Timer 2 in input capture mode, active input channel 3
  // to latch counter value on rising edge

  // Instantiate HardwareTimer object. Thanks to 'new' instantiation, HardwareTimer is not destructed when setup() function is finished.
  HardwareTimer *FreqMeasTim = new HardwareTimer(TIM2);
  
  // Configure rising edge detection to measure frequency
  FreqMeasTim->setMode(3, TIMER_INPUT_CAPTURE_RISING, PB10);

  // Configure 32-bit auto-reload register (ARR) with maximum possible value
  TIM2->ARR = 0xffffffff; // count to 2^32, then wraparound (approximately every 429 seconds)

  // select external clock source mode 2 by writing ECE=1 in the TIM2_SMCR register
  TIM2->SMCR |= TIM_SMCR_ECE; // 0x4000
  
  // start the timer
  FreqMeasTim->resume();
}

void pinModeAF(int ulPin, uint32_t Alternate)
{
   int pn = digitalPinToPinName(ulPin);

   if (STM_PIN(pn) < 8) {
      LL_GPIO_SetAFPin_0_7( get_GPIO_Port(STM_PORT(pn)), STM_LL_GPIO_PIN(pn), Alternate);
   } else {
      LL_GPIO_SetAFPin_8_15(get_GPIO_Port(STM_PORT(pn)), STM_LL_GPIO_PIN(pn), Alternate);
   }

   LL_GPIO_SetPinMode(get_GPIO_Port(STM_PORT(pn)), STM_LL_GPIO_PIN(pn), LL_GPIO_MODE_ALTERNATE);
}


void loop()
{
  serial_commands_.ReadSerial();  // process any command from USB serial (Arduino monitor)
  
  if (gpsWaitFix(5)) // wait 5 seconds for fix
  {
    Serial.println();
    Serial.println();
    Serial.print(F("Fix time "));
    Serial.print(endFixmS - startGetFixmS);
    Serial.println(F("mS"));

    GPSLat = gps.location.lat();
    GPSLon = gps.location.lng();
    GPSAlt = gps.altitude.meters();
    GPSSats = gps.satellites.value();
    GPSHdop = gps.hdop.value();

    hours = gps.time.hour();
    mins = gps.time.minute();
    secs = gps.time.second();
    day = gps.date.day();
    month = gps.date.month();
    year = gps.date.year();


    if (must_adjust_DAC) adjustVctlDAC(); // in principle just once every 429 seconds

    adcVctl = analogRead(VctlInputPin);

    #ifdef GPSDO_VCC
    adcVcc = analogRead(VccDiv2InputPin);
    # endif // VCC

    #ifdef GPSDO_VDD
    adcVdd = analogRead(AVREF);                      // Vdd is read internally as Vref
    #endif // VDD    

    #ifdef GPSDO_BMP280_SPI
    bmp280temp = bmp.readTemperature();              // read bmp280 sensor, save values
    bmp280pres = bmp.readPressure();
    bmp280alti = bmp.readAltitude();
    #endif // BMP280_SPI    

    #ifdef GPSDO_INA219
    ina219volt = ina219.busVoltage();                // read ina219 sensor, save values
    ina219curr = ina219.shuntCurrent();
    #endif // INA219 
  
    uptimetostrings();  // get updaysstr and uptimestr
    
    calcavg();          // calculate frequency averages

    #ifdef GPSDO_BLUETOOTH
    btGPSDOstats();
    #endif // BLUETOOTH

    printGPSDOstats();
    displayscreen1();
    startGetFixmS = millis();    //have a fix, next thing that happens is checking for a fix, so restart timer
  }
  else
  {
    uint8_t i;
    for (i=1; i<8; i++) {
      disp.clearLine(i);
    }
    disp.setCursor(0, 1);
    disp.print(F("No GPS Fix "));
    disp.print( (millis() - startGetFixmS) / 1000 );
    Serial.println();
    Serial.println();
    Serial.print(F("Timeout - No GPS Fix "));
    Serial.print( (millis() - startGetFixmS) / 1000 );
    Serial.println(F("s"));
  }
}

void adjustVctlDAC() // slightly more advanced algorithm than previous version
// This should reach a stable DAC output value / a stable 10000000.00 frequency
// 10x faster than before
{
  if (avgfhun >= 10000000.01) {
    if (avgfhun >= 10000000.10) {
     // decrease DAC by ten bits
      adjusted_DAC_output = adjusted_DAC_output - 10;
    } else {
      // decrease DAC by one bit
      adjusted_DAC_output--;
    }
    dac.setVoltage(adjusted_DAC_output, false); // min=0 max=4096
    must_adjust_DAC = false;
  } else if (avgfhun <= 9999999.99) {
    if (avgfhun <= 9999999.90) {
     // increase DAC by ten bits
      adjusted_DAC_output = adjusted_DAC_output + 10;      
    } else {
    // increase DAC by one bit
    adjusted_DAC_output++;
    }
    dac.setVoltage(adjusted_DAC_output, false); // min=0 max=4096
    must_adjust_DAC = false;
  }
}


bool gpsWaitFix(uint16_t waitSecs)
{
  //waits a specified number of seconds for a fix, returns true for good fix

  uint32_t endwaitmS;
  uint8_t GPSchar;

  Serial.print(F("Wait GPS Fix "));
  Serial.print(waitSecs);
  Serial.println(F(" seconds"));

  endwaitmS = millis() + (waitSecs * 1000);

  while (millis() < endwaitmS)
  {
    if (Serial1.available() > 0)
    {
      GPSchar = Serial1.read();
      gps.encode(GPSchar);
      #ifdef GPSDO_VERBOSE_NMEA
      Serial.write(GPSchar);  // echo NMEA stream to USB serial
      #ifdef GPSDO_BLUETOOTH
      Serial2.write(GPSchar); // echo NMEA stream to Bluetooth serial
      #endif // Bluetooth
      #endif // VERBOSE_NMEA
    }

    if (gps.location.isUpdated() && gps.altitude.isUpdated() && gps.date.isUpdated())
    {
      endFixmS = millis();                                //record the time when we got a GPS fix
      return true;
    }
  }

  return false;
}


void printGPSDOstats() 
{
  float tempfloat;
  
  Serial.print(F("Uptime "));
  Serial.print(updaysstr);
  Serial.print(F(" "));
  Serial.println(uptimestr);
  
  Serial.print(F("New GPS Fix "));

  tempfloat = ( (float) GPSHdop / 100);

  Serial.print(F("Lat,"));
  Serial.print(GPSLat, 6);
  Serial.print(F(",Lon,"));
  Serial.print(GPSLon, 6);
  Serial.print(F(",Alt,"));
  Serial.print(GPSAlt, 1);
  Serial.print(F("m,Sats,"));
  Serial.print(GPSSats);
  Serial.print(F(",HDOP,"));
  Serial.print(tempfloat, 2);
  Serial.print(F(",Time,"));

  if (hours < 10)
  {
    Serial.print(F("0"));
  }

  Serial.print(hours);
  Serial.print(F(":"));

  if (mins < 10)
  {
    Serial.print(F("0"));
  }

  Serial.print(mins);
  Serial.print(F(":"));

  if (secs < 10)
  {
    Serial.print(F("0"));
  }

  Serial.print(secs);
  Serial.print(F(",Date,"));

  Serial.print(day);
  Serial.print(F("/"));
  Serial.print(month);
  Serial.print(F("/"));
  Serial.println(year);

  Serial.println();
  float Vctl = (float(adcVctl)/4096) * 3.3;
  Serial.print("Vctl: ");
  Serial.print(Vctl);
  Serial.print("  DAC: ");
  Serial.println(adjusted_DAC_output);

  #ifdef GPSDO_VCC
  // Vcc/2 is provided on pin PA0
  float Vcc = (float(adcVcc)/4096) * 3.3 * 2.0;
  Serial.print("Vcc: ");
  Serial.println(Vcc);
  #endif // VCC

  #ifdef GPSDO_VDD
  // internal sensor Vref
  float Vdd = (1.21 * 4096) / float(adcVdd); // from STM32F411CEU6 datasheet
  Serial.print("Vdd: ");                     // Vdd = Vref on Black Pill
  Serial.println(Vdd);
  #endif // VDD

  #ifdef GPSDO_INA219
  // current sensor for the OCXO
  Serial.print(F("OCXO voltage: "));
  Serial.print(ina219volt, 2);
  Serial.println(F("V"));
  Serial.print(F("OCXO current: "));
  Serial.print(ina219curr, 0);
  Serial.println(F("mA"));
  #endif // INA219 
      
  // OCXO frequency measurements
  Serial.println();
  Serial.print(F("Counter: "));
  Serial.print(TIM2->CCR3);
  Serial.print(F(" Frequency: "));
  Serial.print(calcfreqint);
  Serial.print(F(" Hz"));
  Serial.println();
  Serial.print("10s Frequency Avg: ");
  Serial.print(avgften,1);
  Serial.print(F(" Hz"));
  Serial.println();
  Serial.print("100s Frequency Avg: ");
  Serial.print(avgfhun,2);
  Serial.print(F(" Hz"));
  Serial.println(); 

  #ifdef GPSDO_BMP280_SPI
  // BMP280 measurements
  Serial.print(F("BMP280 Temperature = "));
  Serial.print(bmp280temp, 1);
  Serial.println(" *C");
  Serial.print(F("Pressure = "));
  Serial.print((bmp280pres+PressureOffset)/100, 1);
  Serial.println(" hPa");
  Serial.print(F("Approx altitude = "));
  Serial.print(bmp280alti, 1); /* Adjusted to local forecast! */
  Serial.println(" m");
  #endif // BMP280_SPI

  #ifdef GPSDO_AHT10
  // AHT10 measurements
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);// populate temp and humidity objects with fresh data
  Serial.print("AHT10 Temperature: ");
  Serial.print(temp.temperature);
  Serial.println(" *C");
  Serial.print("Humidity: ");
  Serial.print(humidity.relative_humidity);
  Serial.println("% rH");
  #endif // AHT10
  
  Serial.println();
  Serial.println();
}

void btGPSDOstats() 
{
  float tempfloat;
  
  Serial2.print(F("Uptime "));
  Serial2.print(updaysstr);
  Serial2.print(F(" "));
  Serial2.println(uptimestr);

  Serial2.print(F("New GPS Fix "));

  tempfloat = ( (float) GPSHdop / 100);

  Serial2.print(F("Lat,"));
  Serial2.print(GPSLat, 6);
  Serial2.print(F(",Lon,"));
  Serial2.print(GPSLon, 6);
  Serial2.print(F(",Alt,"));
  Serial2.print(GPSAlt, 1);
  Serial2.print(F("m,Sats,"));
  Serial2.print(GPSSats);
  Serial2.print(F(",HDOP,"));
  Serial2.print(tempfloat, 2);
  Serial2.print(F(",Time,"));

  if (hours < 10)
  {
    Serial2.print(F("0"));
  }

  Serial2.print(hours);
  Serial2.print(F(":"));

  if (mins < 10)
  {
    Serial2.print(F("0"));
  }

  Serial2.print(mins);
  Serial2.print(F(":"));

  if (secs < 10)
  {
    Serial2.print(F("0"));
  }

  Serial2.print(secs);
  Serial2.print(F(",Date,"));

  Serial2.print(day);
  Serial2.print(F("/"));
  Serial2.print(month);
  Serial2.print(F("/"));
  Serial2.println(year);

  Serial2.println();
  float Vctl = (float(adcVctl)/4096) * 3.3;
  Serial2.print("Vctl: ");
  Serial2.print(Vctl);
  Serial2.print("  DAC: ");
  Serial2.println(adjusted_DAC_output);

  #ifdef GPSDO_VCC
  // Vcc/2 is provided on pin PA0
  float Vcc = (float(adcVcc)/4096) * 3.3 * 2.0;
  Serial2.print("Vcc: ");
  Serial2.println(Vcc);
  #endif // VCC

  #ifdef GPSDO_VDD
  // internal sensor Vref
  float Vdd = (1.21 * 4096) / float(adcVdd); // from STM32F411CEU6 datasheet
  Serial2.print("Vdd: ");                     // Vdd = Vref on Black Pill
  Serial2.println(Vdd);
  #endif // VDD
  
  // OCXO frequency measurements
  Serial2.println();
  Serial2.print(F("Counter: "));
  Serial2.print(TIM2->CCR3);
  Serial2.print(F(" Frequency: "));
  Serial2.print(calcfreqint);
  Serial2.print(F(" Hz"));
  Serial2.println();
  Serial2.print("10s Frequency Avg: ");
  Serial2.print(avgften,1);
  Serial2.print(F(" Hz"));
  Serial2.println();
  Serial2.print("100s Frequency Avg: ");
  Serial2.print(avgfhun,2);
  Serial2.print(F(" Hz"));
  Serial2.println(); 

  #ifdef GPSDO_BMP280_SPI
  // BMP280 measurements
  Serial2.print(F("BMP280 Temperature = "));
  Serial2.print(bmp280temp, 1);
  Serial2.println(" *C");
  Serial2.print(F("Pressure = "));
  Serial2.print((bmp280pres+PressureOffset)/100, 1);
  Serial2.println(" hPa");
  Serial2.print(F("Approx altitude = "));
  Serial2.print(bmp280alti, 1); /* Adjusted to local forecast! */
  Serial2.println(" m");
  #endif // BMP280_SPI

  #ifdef GPSDO_AHT10
  // AHT10 measurements
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);// populate temp and humidity objects with fresh data
  Serial2.print("AHT10 Temperature: ");
  Serial2.print(temp.temperature);
  Serial2.println(" *C");
  Serial2.print("Humidity: ");
  Serial2.print(humidity.relative_humidity);
  Serial2.println("% rH");
  #endif // AHT10
  
  Serial2.println();
  Serial2.println();
}

void displayscreen1()
{
  //show GPSDO data on OLED display
  float tempfloat;

  // OCXO frequency
  disp.setCursor(0, 1);
  disp.print(F("F "));
  // display 1s, 10s or 100s value depending on whether data is available
  if (cbTen_full) {
    if (cbHun_full) { // if we have data over 100 seconds
      if (avgfhun < 10000000) {
        disp.setCursor(2, 1); disp.print(" ");
      }
      else disp.setCursor(2, 1);
      disp.print(avgfhun, 2); // to 2 decimal places
      disp.print("Hz ");
    }
    else { // nope, only 10 seconds
      if (avgften < 10000000) {
        disp.setCursor(2, 1); disp.print(" ");
      }
      else disp.setCursor(2, 1);
      disp.print(avgften, 1); // to 1 decimal place
      disp.print("Hz  ");
    }
  }
  else { // we don't have any averages
    if (calcfreqint < 10000000) {
      disp.setCursor(2, 1); disp.print(" ");
    }
    else disp.setCursor(2, 1);
    disp.print(calcfreqint); // integer
    disp.print("Hz    ");
  }

  // Latitude
  //disp.clearLine(2);
  disp.setCursor(0, 2);
  disp.print(GPSLat, 6);
  // Longitude
  //disp.clearLine(3);
  disp.setCursor(0, 3);
  disp.print(GPSLon, 6);
  // Altitude and Satellites
  //disp.clearLine(4);
  disp.setCursor(0, 4);
  disp.print(GPSAlt);
  disp.print(F("m  "));
  disp.setCursor(9, 4);
  disp.print(F("Sats "));
  disp.print(GPSSats);
  if (GPSSats < 10) disp.print(F(" ")); // clear possible digit when sats >= 10
  // HDOP
  //disp.clearLine(5);
  disp.setCursor(0, 5);
  // choose HDOP or uptime
  //disp.print(F("HDOP "));
  //tempfloat = ((float) GPSHdop / 100);
  //disp.print(tempfloat);
  disp.print(F("Up "));  
  disp.print(updaysstr);
  disp.print(F(" "));
  disp.print(uptimestr);

  // Time
  //disp.clearLine(6);
  disp.setCursor(0, 6);

  if (hours < 10)
  {
    disp.print(F("0"));
  }

  disp.print(hours);
  disp.print(F(":"));

  if (mins < 10)
  {
    disp.print(F("0"));
  }

  disp.print(mins);
  disp.print(F(":"));

  if (secs < 10)
  {
    disp.print(F("0"));
  }

  disp.print(secs);
  disp.print(F("  "));

  // Date
  //disp.clearLine(7);
  disp.setCursor(0, 7);

  disp.print(day);
  disp.print(F("/"));
  disp.print(month);
  disp.print(F("/"));
  disp.print(year);

  #ifdef GPSDO_BMP280_SPI
  // BMP280 temperature
  disp.setCursor(10, 6);
  disp.print(bmp280temp, 1);
  disp.print(F("C"));
  #endif // BMP280_SPI

  #ifdef GPSDO_VCC
  disp.setCursor(11, 2);
  // Vcc/2 is provided on pin PA0
  float Vcc = (float(adcVcc)/4096) * 3.3 * 2.0;
  disp.print(Vcc);
  disp.print(F("V"));
  #endif // VCC

  #ifdef GPSDO_VDD
  // internal sensor Vref
  disp.setCursor(11, 3);
  float Vdd = (1.21 * 4096) / float(adcVdd); // from STM32F411CEU6 datasheet                 
  disp.print(Vdd);                           // Vdd = Vref on Black Pill
  disp.print(F("V"));
  #endif // VDD

  disp.setCursor(11, 7); // display DAC value
  disp.print(adjusted_DAC_output);  
}

void calcavg() {
  // Calculate the OCXO frequency to 1 or 2 decimal places only when the respective buffers are full
  
  if (cbTen_full) { // we want (latest fcount - oldest fcount) / 10
    
    uint32_t latfcount, oldfcount; // latest fcount, oldest fcount stored in ring buffer

    // latest fcount is always circbuf_ten[cbiten_newest-1]
    // except when cbiten_newest is zero
    // oldest fcount is always circbuf_ten[cbiten_newest] when buffer is full

    if (cbiten_newest == 0) latfcount = circbuf_ten[10];
    else latfcount = circbuf_ten[cbiten_newest-1];
    oldfcount = circbuf_ten[cbiten_newest];
    
    avgften = double(latfcount - oldfcount)/10.0;
    // oldest fcount is always circbuf_ten[cbiten_newest-2]
    // except when cbiten_newest is <2 (zero or 1)
    
  }
   
  if (cbHun_full) { // we want (latest fcount - oldest fcount) / 100
    
    uint32_t latfcount, oldfcount;

    // latest fcount is always circbuf_hun[cbihun_newest-1]
    // except when cbihun_newest is zero
    // oldest fcount is always circbuf_hun[cbihun_newest] when buffer is full

    if (cbihun_newest == 0) latfcount = circbuf_hun[100];
    else latfcount = circbuf_hun[cbihun_newest-1];
    oldfcount = circbuf_hun[cbihun_newest];
    
    avgfhun = double(latfcount - oldfcount)/100.0;
    // oldest fcount is always circbuf_ten[cbiten_newest-2]
    // except when cbiten_newest is <2 (zero or 1)
  } 
}

void uptimetostrings() {
  // translate uptime variables to strings
  uptimestr[0] = '0' + uphours / 10;
  uptimestr[1] = '0' + uphours % 10;
  uptimestr[3] = '0' + upminutes / 10;
  uptimestr[4] = '0' + upminutes % 10;
  uptimestr[6] = '0' + upseconds / 10;
  uptimestr[7] = '0' + upseconds % 10;
 
  if (updays > 99) { // 100 days or more
    updaysstr[0] = '0' + updays / 100;
    updaysstr[1] = '0' + (updays % 100) / 10;
    updaysstr[2] = '0' + (updays % 100) % 10;
  }
  else { // less than 100 days
    updaysstr[0] = '0';
    updaysstr[1] = '0' + updays / 10;
    updaysstr[2] = '0' + updays % 10;
  }
}
