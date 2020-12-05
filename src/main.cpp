//////////////////////////////////////////
// TODO : check button long press processing
// TODO : current loop
// TODO : LCD error indicators
// TODO : mode Z button/settings
// TODO : auto mode shift on low battery
// TODO : original regulator perturbation
// TODO : exponential throttle
// BUG : mode Z / android
// BUG : push button make brake analog read wrong
//////////////////////////////////////////

//////------------------------------------
////// Inludes

#include <Arduino.h>
#include <EEPROM.h>
#include "main.h"
#include "OTA_wifi.h"
#include "MedianFilter.h"
#include "dht_nonblocking.h"
#include "Settings.h"
#include "BluetoothHandler.h"
#include "SharedData.h"
#include "debug.h"
#include "OneButton.h"
#include "EEPROM_storage.h"
#include <Wire.h>
#include <Adafruit_MCP4725.h>

#include "app_version.h"

#include <PID_v1.h>
#include "VescUart.h"
#include "MinimoUart.h"

#include "TFT/tft_main.h"

//////------------------------------------
////// Defines

// SMART CONFIGURATION
#define TFT_ENABLED 1
#define CONTROLLER_MINIMOTORS 1
#define CONTROLLER_VESC 0
#define READ_THROTTLE 0
#define DEBUG_ESP_HTTP_UPDATE 1
#define TEST_ADC_DAC_REFRESH 1

// MINIMO CONFIG
#define ALLOW_LCD_TO_CNTRL_MODIFICATIONS true
#define ALLOW_CNTRL_TO_LCD_MODIFICATIONS true

// PINOUT
#define PIN_SERIAL_ESP_TO_LCD 26
#define PIN_SERIAL_ESP_TO_CNTRL 27
#define PIN_SERIAL_LCD_TO_ESP 25
#define PIN_SERIAL_CNTRL_TO_ESP 14
//#define PIN_OUT_RELAY xx
#define PIN_IN_VOLTAGE 32
#define PIN_IN_CURRENT 35
#define PIN_IN_BUTTON1 22
#define PIN_IN_BUTTON2 15 // PB was TX
#define PIN_OUT_LED_BUTTON1 3
#define PIN_OUT_LED_BUTTON2 21
#define PIN_OUT_BRAKE 13
#define PIN_IN_OUT_DHT 12
#define PIN_IN_ABRAKE 34
#define PIN_IN_THROTTLE 33
#define PIN_OUT_BACKLIGHT 5
#define PIN_I2C_SDA 32
#define PIN_I2C_SCL 33

// I2C
//#define I2C_FREQ 400000
#define I2C_FREQ 1000000

// UART
#define BAUD_RATE_CONSOLE 921600

// ADC
#define ANALOG_TO_VOLTS_A 0.0213
#define ANALOG_TO_VOLTS_B 5.4225
#define ANALOG_TO_CURRENT 35

#define NB_CURRENT_CALIB 200
#define NB_BRAKE_CALIB 100

#define BRAKE_TYPE_ANALOG 1
#if BRAKE_TYPE_ANALOG
#define ANALOG_BRAKE_MIN_ERR_VALUE 500
#define ANALOG_BRAKE_MAX_ERR_VALUE 3500
#else
#define ANALOG_BRAKE_MIN_ERR_VALUE 0
#define ANALOG_BRAKE_MAX_ERR_VALUE 4095
#endif
#define ANALOG_BRAKE_MIN_VALUE 920
#define ANALOG_BRAKE_MIN_OFFSET 100
#define ANALOG_BRAKE_MAX_VALUE 2300

// BUTTONS
#define BUTTON_LONG_PRESS_TICK 300

#define WATCHDOG_TIMEOUT 1000000 //time in ms to trigger the watchdog

//////------------------------------------
////// Variables

// Time
unsigned long timeLoop = 0;

// Watchdog
hw_timer_t *timer = NULL;

// Settings

int begin_soft = 0;
int begin_hard = 0;

char bleLog[50] = "";

HardwareSerial hwSerCntrl(1);
HardwareSerial hwSerLcd(2);

VescUart vescCntrl;
MinimoUart minomoCntrl;

DHT_nonblocking dht_sensor(PIN_IN_OUT_DHT, DHT_TYPE_22);

OneButton button1(PIN_IN_BUTTON1, true, true);
OneButton button2(PIN_IN_BUTTON2, true, true);

TwoWire I2Cone = TwoWire(0);
Adafruit_MCP4725 dac;

SharedData shrd;

int i_loop = 0;

uint32_t iBrakeCalibOrder = 0;

uint16_t voltageStatus = 0;
uint32_t voltageInMilliVolts = 0;

uint16_t brakeAnalogValue = 0;
uint16_t throttleAnalogValue = 0;

MedianFilter voltageFilter(100, 2000);
MedianFilter voltageRawFilter(100, 2000);
MedianFilter currentFilter(200, 1830);
MedianFilter currentFilterInit(NB_CURRENT_CALIB, 1830);
MedianFilter brakeFilter(10 /* 20 */, 900);
MedianFilter brakeFilterInit(NB_BRAKE_CALIB, 900);

Settings settings;

BluetoothHandler blh;

PID pidSpeed(&shrd.pidInput, &shrd.pidOutput, &shrd.pidSetpoint, shrd.speedPidKp, shrd.speedPidKi, shrd.speedPidKd, DIRECT);

//////------------------------------------
////// EEPROM functions

void saveBleLockForced()
{
  EEPROM.writeBytes(EEPROM_ADDRESS_BLE_LOCK_FORCED, &blh.bleLockForced, sizeof(blh.bleLockForced));
  EEPROM.commit();

  Serial.print("save bleLockForced value : ");
  Serial.println(blh.bleLockForced);
}

void restoreBleLockForced()
{
  EEPROM.readBytes(EEPROM_ADDRESS_BLE_LOCK_FORCED, &blh.bleLockForced, sizeof(blh.bleLockForced));

  Serial.print("restore bleLockForced value : ");
  Serial.println(blh.bleLockForced);
}

void saveBrakeMaxPressure()
{
  EEPROM.writeBytes(EEPROM_ADDRESS_BRAKE_MAX_PRESSURE, &shrd.brakeMaxPressureRaw, sizeof(shrd.brakeMaxPressureRaw));
  EEPROM.commit();

  Serial.print("save saveBrakeMaxPressure value : ");
  Serial.println(shrd.brakeMaxPressureRaw);
}

void restoreBrakeMaxPressure()
{
  EEPROM.readBytes(EEPROM_ADDRESS_BRAKE_MAX_PRESSURE, &shrd.brakeMaxPressureRaw, sizeof(shrd.brakeMaxPressureRaw));

  Serial.print("restore restoreBrakeMaxPressure value : ");
  Serial.println(shrd.brakeMaxPressureRaw);

  if (shrd.brakeMaxPressureRaw == -1)
    shrd.brakeMaxPressureRaw = ANALOG_BRAKE_MAX_VALUE;
}

void saveOdo()
{
  EEPROM.writeBytes(EEPROM_ADDRESS_ODO, &shrd.distanceOdo, sizeof(shrd.distanceOdo));
  EEPROM.commit();

  Serial.print("save saveOdo value : ");
  Serial.println(shrd.distanceOdo);
}

void restoreOdo()
{
  EEPROM.readBytes(EEPROM_ADDRESS_ODO, &shrd.distanceOdo, sizeof(shrd.distanceOdo));

  shrd.distanceOdoInFlash = shrd.distanceOdo;
  shrd.distanceOdoBoot = shrd.distanceOdo;

  Serial.print("restore restoreOdo value : ");
  Serial.println(shrd.distanceOdo);

  if (shrd.distanceOdo == -1)
    shrd.distanceOdo = 0;
}

//////------------------------------------
//////------------------------------------
////// Setups

void setupPins()
{

  pinMode(PIN_IN_OUT_DHT, INPUT_PULLUP);
  pinMode(PIN_IN_BUTTON1, INPUT_PULLUP);
  pinMode(PIN_IN_BUTTON2, INPUT_PULLUP);
  pinMode(PIN_IN_VOLTAGE, INPUT);
  pinMode(PIN_IN_CURRENT, INPUT);
  //pinMode(PIN_IN_DBRAKE, INPUT_PULLUP);
  //  pinMode(PIN_OUT_RELAY, OUTPUT);
  pinMode(PIN_OUT_BRAKE, OUTPUT);
  pinMode(PIN_OUT_LED_BUTTON1, OUTPUT);
  pinMode(PIN_OUT_LED_BUTTON2, OUTPUT);
}

/*
#include "soc/efuse_reg.h"
#include "esp_efuse.h" // for programming eFuse.

void setupEFuse()
{

  // INITIAL SETUP...BURN THE EFUSE IF NECESSARY FOR PROPER OPERATION.
  // Force the FLASH voltage regulator to 3.3v, disabling "MTDI" strapping check at startup
  if ((REG_READ(EFUSE_BLK0_RDATA4_REG) & EFUSE_RD_SDIO_TIEH) == 0)
  {
    esp_efuse_reset();
    REG_WRITE(EFUSE_BLK0_WDATA4_REG, EFUSE_RD_SDIO_TIEH);
    esp_efuse_burn_new_values();
  } //burning SDIO_TIEH -> sets SDIO voltage regulator to pass-thru 3.3v from VDD
  if ((REG_READ(EFUSE_BLK0_RDATA4_REG) & EFUSE_RD_XPD_SDIO_REG) == 0)
  {
    esp_efuse_reset();
    REG_WRITE(EFUSE_BLK0_WDATA4_REG, EFUSE_RD_XPD_SDIO_REG);
    esp_efuse_burn_new_values();
  } //burning SDIO_REG -> enables SDIO voltage regulator (otherwise user must hardwire power to SDIO)
  if ((REG_READ(EFUSE_BLK0_RDATA4_REG) & EFUSE_RD_SDIO_FORCE) == 0)
  {
    esp_efuse_reset();
    REG_WRITE(EFUSE_BLK0_WDATA4_REG, EFUSE_RD_SDIO_FORCE);
    esp_efuse_burn_new_values();
  } //burning SDIO_FORCE -> enables SDIO_REG and SDIO_TIEH
}
*/

void setupI2C()
{

  I2Cone.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ);
}

void setupDac()
{
  // call GENERAL CALL RESET
  // https://www.sparkfun.com/datasheets/BreakoutBoards/MCP4725.pdf
  // page 22

  dac.begin(0x60, &I2Cone);
}

void setupSerial()
{

#if CONTROLLER_MINIMOTORS

  minomoCntrl.setSettings(&settings);
  minomoCntrl.setSharedData(&shrd);
  minomoCntrl.setBluetoothHandler(&blh);

  // minimotor controller
  hwSerCntrl.begin(BAUD_RATE_MINIMOTORS, SERIAL_8N1, PIN_SERIAL_CNTRL_TO_ESP, PIN_SERIAL_ESP_TO_CNTRL);
  minomoCntrl.setControllerSerialPort(&hwSerCntrl);

  // minimotor display
  hwSerLcd.begin(BAUD_RATE_MINIMOTORS, SERIAL_8N1, PIN_SERIAL_LCD_TO_ESP, PIN_SERIAL_ESP_TO_LCD);
  minomoCntrl.setLcdSerialPort(&hwSerLcd);

#endif
#if CONTROLLER_VESC
  hwSerCntrl.begin(BAUD_RATE_VESC, SERIAL_8N1, PIN_SERIAL_CNTRL_TO_ESP, PIN_SERIAL_ESP_TO_CNTRL);
  vescCntrl.setSerialPort(&hwSerCntrl);
  //vescCntrl.setDebugPort(&Serial);
#endif
}

void setupEPROMM()
{
  EEPROM.begin(EEPROM_SIZE);
}

void setupPID()
{

  shrd.pidSetpoint = settings.getS1F().Speed_limiter_max_speed;

  //turn the PID on
  pidSpeed.SetMode(AUTOMATIC);
  //myPID.SetSampleTime(10);
  //myPID.SetOutputLimits(0,100);
}

void resetPid()
{

  shrd.pidSetpoint = settings.getS1F().Speed_limiter_max_speed;
  pidSpeed.SetTunings(shrd.speedPidKp, shrd.speedPidKi, shrd.speedPidKd);
  Serial.println("set PID tunings");
}

void initDataWithSettings()
{
  shrd.speedLimiter = (settings.getS1F().Speed_limiter_at_startup == 1);
}

void setupButtons()
{

  button1.attachClick(processButton1Click);
  button1.attachLongPressStart(processButton1LpStart);
  button1.attachDuringLongPress(processButton1LpDuring);
  button1.attachLongPressStop(processButton1LpStop);
  button1.setDebounceTicks(50);
  button1.setPressTicks(BUTTON_LONG_PRESS_TICK);

  button2.attachClick(processButton2Click);
  button2.attachLongPressStart(processButton2LpStart);
  button2.attachDuringLongPress(processButton2LpDuring);
  button2.attachLongPressStop(processButton2LpStop);
  button2.setDebounceTicks(50);
  button2.setPressTicks(BUTTON_LONG_PRESS_TICK);
}

void IRAM_ATTR triggerWatchdog()
{
  ets_printf("watchdog => reboot\n");
  esp_restart();
}

void setupWatchdog()
{
  timer = timerBegin(0, 80, true);                        //timer 0, div 80
  timerAttachInterrupt(timer, &triggerWatchdog, true);    //attach callback
  timerAlarmWrite(timer, WATCHDOG_TIMEOUT * 1000, false); //set time in us
  timerAlarmEnable(timer);                                //enable interrupt
}

void resetWatchdog()
{
  timerWrite(timer, 0); //reset timer (feed watchdog)
}

void disableWatchdog()
{
  timerAlarmDisable(timer);
}

void taskUpdateTFT(void *parameter)
{
  int i = 0;
  for (;;)
  { // infinite loop

    Serial.println(">>>>> update TFT");

    tftUpdateData(i);
    // Pause the task again for 50ms
    //vTaskDelay(10 / portTICK_PERIOD_MS);
    i++;

    if (i >= 20)
      i = 0;
  }
}

////// Setup
void setup()
{

#if TFT_ENABLED
  tftSetupBacklight();
#endif

  // Initialize the Serial (use only in setup codes)
  Serial.begin(BAUD_RATE_CONSOLE);
  Serial.println(PSTR("\n\nsetup --- begin"));

  Serial.print("version : ");
  Serial.println(Version);

  shrd.timeLastNotifyBle = millis();

  Serial.println(PSTR("   serial ..."));
  setupSerial();

  Serial.println(PSTR("   i2c ..."));
  setupI2C();

  Serial.println(PSTR("   dac ..."));
  setupDac();

  Serial.println(PSTR("   eeprom ..."));
  setupEPROMM();
  restoreBleLockForced();
  restoreBrakeMaxPressure();
  restoreOdo();

  Serial.println(PSTR("   settings ..."));
  bool settingsStatusOk = settings.restoreSettings();
  if (!settingsStatusOk)
  {
    settings.initSettings();
  }
  settings.displaySettings();

  Serial.println(PSTR("   BLE ..."));
  //setupBLE();
  blh.init(&settings);
  blh.setSharedData(&shrd);

  Serial.println(PSTR("   pins ..."));
  setupPins();

  Serial.println(PSTR("   buttons ..."));
  setupButtons();

  Serial.println(PSTR("   PID ..."));
  setupPID();

#if TFT_ENABLED
  Serial.println(PSTR("   TFT ..."));
  tftSetup(&shrd, &settings);
#endif

  // force BLE lock mode
  blh.setBleLock(false);

  // setup shared datas
  shrd.currentCalibOrder = NB_CURRENT_CALIB;

  Serial.println(PSTR("   init data with settings ..."));
  initDataWithSettings();

  Serial.println(PSTR("   watchdog ..."));
  setupWatchdog();

  xTaskCreatePinnedToCore(
      taskUpdateTFT,   // Function that should be called
      "taskUpdateTFT", // Name of the task (for debugging)
      10000,           // Stack size (bytes)
      NULL,            // Parameter to pass
      0,               // Task priority
      NULL,            // Task handle,
      1);

  // End off setup
  Serial.println("setup --- end");
}

void notifyBleLogFrame(int mode, char data_buffer[], byte checksum)
{

  char print_buffer[500];

  // for excel
  sprintf(print_buffer, "(%d) %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x / %02x",
          mode,
          data_buffer[0],
          data_buffer[1],
          data_buffer[2],
          data_buffer[3],
          data_buffer[4],
          data_buffer[5],
          data_buffer[6],
          data_buffer[7],
          data_buffer[8],
          data_buffer[9],
          data_buffer[10],
          data_buffer[11],
          data_buffer[12],
          data_buffer[13],
          data_buffer[14],
          checksum);

  blh.notifyBleLogs(print_buffer);
}

void displaySpeed()
{

  Serial.print("speedCurrent : ");
  Serial.print(shrd.speedCurrent);
  Serial.print(" / speedOld : ");
  Serial.print(shrd.speedOld);
  Serial.println("");
}

void displayBrake()
{
  Serial.print("Brake : ");
  Serial.println(shrd.brakeStatus);
}

void displayButton1()
{
  Serial.print("Button1 : ");
  Serial.println(shrd.button1ClickStatus);
}

void displayButton2()
{
  Serial.print("Button2 : ");
  Serial.println(shrd.button1ClickStatus);
}

void getBrakeFromAnalog()
{

  brakeAnalogValue = analogRead(PIN_IN_ABRAKE);
  shrd.brakeAnalogValue = brakeAnalogValue;

  if (settings.getS2F().Electric_brake_type == settings.LIST_Electric_brake_type_analog)
  {

    int brakeFilterMean = brakeFilter.getMean();
    int brakeFilterMeanErr = brakeFilter.getMeanWithoutExtremes(1);

    // ignore out of range datas ... and notify
    if (brakeAnalogValue < ANALOG_BRAKE_MIN_ERR_VALUE)
    {
#if DEBUG_DISPLAY_ANALOG_BRAKE
      Serial.println("brake ANALOG_BRAKE_MIN_ERR_VALUE");
#endif

      /*
      char print_buffer[500];
      sprintf(print_buffer, "brake ANALOG_BRAKE_MIN_ERR_VALUE / f1 : %d / f2 : %d / raw : %d / sentOrder : %d / sentOrderOld : %d / status : %d / init : %d",
              brakeFilterMean,
              brakeFilterMeanErr,
              brakeAnalogValue,
              shrd.brakeSentOrder,
              shrd.brakeSentOrderOld,
              shrd.brakeStatus,
              brakeFilterInit.getMean());
      blh.notifyBleLogs(print_buffer);
      Serial.println(print_buffer);
*/
      return;
    }

    // ignore out of range datas ... and notify
    if (brakeAnalogValue > ANALOG_BRAKE_MAX_ERR_VALUE)
    {
#if DEBUG_DISPLAY_ANALOG_BRAKE
      Serial.println("brake ANALOG_BRAKE_MAX_ERR_VALUE");
      char print_buffer[500];
      sprintf(print_buffer, "brake ANALOG_BRAKE_MAX_ERR_VALUE / f1 : %d / f2 : %d / raw : %d / sentOrder : %d / sentOrderOld : %d / status : %d / init : %d",
              brakeFilterMean,
              brakeFilterMeanErr,
              brakeAnalogValue,
              shrd.brakeSentOrder,
              shrd.brakeSentOrderOld,
              shrd.brakeStatus,
              brakeFilterInit.getMean());
      blh.notifyBleLogs(print_buffer);
      Serial.println(print_buffer);
#endif
      return;
    }

    if (brakeAnalogValue > shrd.brakeMaxPressureRaw)
      brakeAnalogValue = shrd.brakeMaxPressureRaw;

    brakeFilter.in(brakeAnalogValue);

    if ((brakeAnalogValue < 1000) && (shrd.currentCalibOrder == 1))

      brakeFilterInit.in(brakeAnalogValue);

    iBrakeCalibOrder++;
    if (iBrakeCalibOrder > NB_BRAKE_CALIB)
    {
      iBrakeCalibOrder = 0;
      shrd.brakeCalibOrder = 0;
    }

    if (settings.getS1F().Electric_brake_progressive_mode == 1)
    {
      brakeFilterMeanErr = brakeFilter.getMeanWithoutExtremes(1);
      brakeFilterMean = brakeFilter.getMean();

      shrd.brakeFordidenHighVoltage = isElectricBrakeForbiden();

      // alarm controler from braking
      if ((brakeFilterMeanErr > brakeFilterInit.getMean() + ANALOG_BRAKE_MIN_OFFSET) && (!shrd.brakeFordidenHighVoltage))
      {
        digitalWrite(PIN_OUT_BRAKE, 1);

        if (shrd.brakeStatus == 0)
        {
          char print_buffer[500];
          sprintf(print_buffer, ">>>> brake IO ON");

          Serial.println(print_buffer);

          blh.notifyBleLogs(print_buffer);
        }

        shrd.brakeStatus = 1;
      }
      else
      {
        digitalWrite(PIN_OUT_BRAKE, 0);

        if (shrd.brakeStatus == 1)
        {
          char print_buffer[500];
          sprintf(print_buffer, ">>>> brake IO OFF");

          Serial.println(print_buffer);

          blh.notifyBleLogs(print_buffer);
        }

        shrd.brakeStatus = 0;
      }

      // notify brake LCD value
      if ((shrd.brakeSentOrder != shrd.brakeSentOrderOld) || (shrd.brakeStatus != shrd.brakeStatusOld))
      {
        blh.notifyBreakeSentOrder(shrd.brakeSentOrder, shrd.brakeStatus, shrd.brakeFordidenHighVoltage);

#if DEBUG_DISPLAY_ANALOG_BRAKE
        Serial.print("brake notify : ");
        Serial.println(shrd.brakeSentOrder);
#endif

        char print_buffer[500];
        sprintf(print_buffer, ">> brakeNotify = f1 : %d / f2 : %d / raw : %d / sentOrder : %d / sentOrderOld : %d / status : %d / init : %d / forbid : %d",
                brakeFilterMean,
                brakeFilterMeanErr,
                brakeAnalogValue,
                shrd.brakeSentOrder,
                shrd.brakeSentOrderOld,
                shrd.brakeStatus,
                brakeFilterInit.getMean(),
                shrd.brakeFordidenHighVoltage);
        blh.notifyBleLogs(print_buffer);
      }

      shrd.brakeStatusOld = shrd.brakeStatus;
      shrd.brakeSentOrderOld = shrd.brakeSentOrder;

#if DEBUG_BLE_DISPLAY_ANALOG_BRAKE

      if ((brakeFilterMeanErr > brakeFilterInit.getMean() + ANALOG_BRAKE_MIN_OFFSET))
      {

        char print_buffer[500];
        sprintf(print_buffer, "brake = f1 : %d / f2 : %d / raw : %d / sentOrder : %d / sentOrderOld : %d / status : %d / init : %d / forbid : %d",
                brakeFilterMean,
                brakeFilterMeanErr,
                brakeAnalogValue,
                shrd.brakeSentOrder,
                shrd.brakeSentOrderOld,
                shrd.brakeStatus,
                brakeFilterInit.getMean(),
                shrd.brakeFordidenHighVoltage);

        Serial.println(print_buffer);

        blh.notifyBleLogs(print_buffer);
      }

#endif
    }
  }
}

bool isElectricBrakeForbiden()
{
  if (settings.getS1F().Electric_brake_disabled_on_high_voltage == 0)
  {
#if DEBUG_BRAKE_FORBIDEN
    Serial.println("electric brake not disabled on battery high voltage");
#endif
    return false;
  }

  float voltage = shrd.voltageFilterMean / 1000.0;
  float bat_min = settings.getS3F().Battery_min_voltage / 10.0;
  float bat_max = settings.getS3F().Battery_max_voltage / 10.0;
  float maxVoltage = bat_min + (settings.getS1F().Electric_brake_disabled_percent_limit * (bat_max - bat_min) / 100.0);

#if DEBUG_BRAKE_FORBIDEN
  Serial.print("bat_min ");
  Serial.print(bat_min);
  Serial.print(" / bat_max ");
  Serial.print(bat_max);
  Serial.print(" / voltage ");
  Serial.print(voltage);
  Serial.print(" / maxVoltage ");
  Serial.print(maxVoltage);
  Serial.print(" / settings.getS1F().Electric_brake_disabled_percent_limit ");
  Serial.println(settings.getS1F().Electric_brake_disabled_percent_limit);
#endif

  return (voltage > maxVoltage);
}

void processVescSerial()
{

  String command;

  if (vescCntrl.readVescValues())
  {
    /*
    Serial.print("rpm : ");
    Serial.print(vescCntrl.data.rpm);
    Serial.print(" / tachometerAbs : ");
    Serial.print(vescCntrl.data.tachometerAbs);
    Serial.print(" / tachometer : ");
    Serial.print(vescCntrl.data.tachometer);
    Serial.print(" / ampHours : ");
    Serial.print(vescCntrl.data.ampHours);
    Serial.print(" / avgInputCurrent : ");
    Serial.print(vescCntrl.data.avgInputCurrent);
    */

    float speedCompute = vescCntrl.data.rpm * (settings.getS1F().Wheel_size / 10.0) / settings.getS1F().Motor_pole_number / 120.0;
    if (speedCompute < 0)
      speedCompute = 0;
    if (speedCompute > 999)
      speedCompute = 999;

    if (speedCompute > shrd.speedMax)
      shrd.speedMax = speedCompute;

    shrd.speedCurrent = speedCompute;

    /*
    Serial.print(" / speedCompute : ");
    Serial.println(speedCompute);
    Serial.println(vescCntrl.data.inpVoltage);
    Serial.println(vescCntrl.data.ampHours);
    */

    shrd.voltageFilterMean = vescCntrl.data.inpVoltage * 1000;
    shrd.currentFilterMean = vescCntrl.data.avgInputCurrent * 1000;
  }

  /*
  Serial.print("throttleAnalogValue : ");
  Serial.println(throttleAnalogValue);
*/

  if (throttleAnalogValue < 900)
    throttleAnalogValue = 0;

  float duty = (throttleAnalogValue - 900) / 2000.0;

  if (duty > 1)
    duty = 1.0;
  if (duty < 0)
    duty = 0.0;
  /*
  Serial.print("duty : ");
  Serial.println(duty);
*/
  vescCntrl.setDuty(duty);
}

uint8_t modifyBrakeFromAnalog(char var, char data_buffer[])
{

  //*********************************
  // shrd.brakeSentOrder = var;
  // BUG TO FIX ???

  shrd.brakeSentOrder = settings.getS1F().Electric_brake_min_value;

  if (settings.getS1F().Electric_brake_progressive_mode == 1)
  {

    uint32_t step = 0;
    uint32_t diff = 0;
    uint32_t diffStep = 0;

    if (settings.getS1F().Electric_brake_max_value - settings.getS1F().Electric_brake_min_value > 0)
    {
      step = (shrd.brakeMaxPressureRaw - ANALOG_BRAKE_MIN_VALUE) / (settings.getS1F().Electric_brake_max_value - settings.getS1F().Electric_brake_min_value);

      int brakeFilterMeanErr = brakeFilter.getMeanWithoutExtremes(1);
      if (brakeFilterMeanErr > ANALOG_BRAKE_MIN_VALUE)
      {

        diff = brakeFilterMeanErr - ANALOG_BRAKE_MIN_VALUE;
        diffStep = diff / step;
        shrd.brakeSentOrder = diffStep + settings.getS1F().Electric_brake_min_value;
      }
    }

#if DEBUG_DISPLAY_ANALOG_BRAKE

    char print_buffer[500];
    sprintf(print_buffer, "brakeFilter : %d / brakeAnalogValue : %d / brakeSentOrder : %d  / brakeSentOrderOld : %d / shrd.brakeStatus : %d / brakeFilterInit : %d ",
            brakeFilter.getMean(),
            brakeAnalogValue,
            shrd.brakeSentOrder,
            shrd.brakeSentOrderOld,
            shrd.brakeStatus,
            brakeFilterInit.getMean());
    blh.notifyBleLogs(print_buffer);

    Serial.println(print_buffer);

#endif
  }

  return shrd.brakeSentOrder;
}

void processButton1Click()
{
  if (shrd.button1ClickStatus == ACTION_OFF)
  {
    shrd.button1ClickStatus = ACTION_ON;
  }
  else
  {
    shrd.button1ClickStatus = ACTION_OFF;
  }

  processAuxEvent(1, false);
  processSpeedLimiterEvent(1, false);
  processLockEvent(1, false);

  Serial.print("processButton1Click : ");
  Serial.println(shrd.button1ClickStatus);

  char print_buffer[500];
  sprintf(print_buffer, "processButton1Click : %d", shrd.button1ClickStatus);
  blh.notifyBleLogs(print_buffer);
}

void processButton1LpStart()
{
  shrd.button1LpDuration = button1.getPressedTicks();
  Serial.print("processButton1LpStart : ");
  Serial.println(shrd.button1LpDuration);

  char print_buffer[500];
  sprintf(print_buffer, "processButton1LpStart : %d", shrd.button1LpDuration);
  blh.notifyBleLogs(print_buffer);
}

void processButton1LpDuring()
{
  shrd.button1LpDuration = button1.getPressedTicks();

  if ((shrd.button1LpDuration > settings.getS3F().Button_long_press_duration * 1000) && (!shrd.button1LpProcessed))
  {

    char print_buffer[500];
    sprintf(print_buffer, "processButton1LpDuring : %d =>> process", shrd.button1LpDuration);
    blh.notifyBleLogs(print_buffer);

    processAuxEvent(1, true);
    processSpeedLimiterEvent(1, true);
    processLockEvent(1, true);
    shrd.button1LpProcessed = true;
  }
}

void processButton1LpStop()
{
  Serial.print("processButton1LpStop : ");
  Serial.println(shrd.button1LpDuration);

  char print_buffer[500];
  sprintf(print_buffer, "processButton1LpStop : %d", shrd.button1LpDuration);
  blh.notifyBleLogs(print_buffer);

  shrd.button1LpProcessed = false;
  shrd.button1LpDuration = 0;
}

void processButton1()
{
  if (shrd.button1ClickStatus == ACTION_ON)
  {
    digitalWrite(PIN_OUT_LED_BUTTON1, HIGH);
  }
  else if (shrd.button1ClickStatus == ACTION_OFF)
  {
    digitalWrite(PIN_OUT_LED_BUTTON1, LOW);
  }
}

////////////////////////////////////////////

void processButton2Click()
{
  if (shrd.button2ClickStatus == ACTION_OFF)
  {
    shrd.button2ClickStatus = ACTION_ON;
  }
  else
  {
    shrd.button2ClickStatus = ACTION_OFF;
  }

  processAuxEvent(2, false);
  processSpeedLimiterEvent(2, false);
  processLockEvent(2, false);

  Serial.print("processButton2Click : ");
  Serial.println(shrd.button2ClickStatus);

  char print_buffer[500];
  sprintf(print_buffer, "processButton2Click : %d", shrd.button2ClickStatus);
  blh.notifyBleLogs(print_buffer);
}

void processButton2LpStart()
{
  shrd.button2LpDuration = button2.getPressedTicks();
  Serial.print("processButton2LpStart : ");
  Serial.println(shrd.button2LpDuration);

  char print_buffer[500];
  sprintf(print_buffer, "processButton2LpStart : %d", shrd.button2LpDuration);
  blh.notifyBleLogs(print_buffer);
}

void processButton2LpDuring()
{
  shrd.button2LpDuration = button2.getPressedTicks();

  if ((shrd.button2LpDuration > settings.getS3F().Button_long_press_duration * 1000) && (!shrd.button2LpProcessed))
  {

    char print_buffer[500];
    sprintf(print_buffer, "processButton2LpDuring : %d ==> process", shrd.button2LpDuration);
    blh.notifyBleLogs(print_buffer);

    processAuxEvent(2, true);
    processSpeedLimiterEvent(2, true);
    processLockEvent(2, true);
    shrd.button2LpProcessed = true;
  }
}

void processButton2LpStop()
{
  Serial.print("processButton2LpStop : ");
  Serial.println(shrd.button2LpDuration);

  char print_buffer[500];
  sprintf(print_buffer, "processButton2LpStop : %d", shrd.button2LpDuration);
  blh.notifyBleLogs(print_buffer);

  shrd.button2LpProcessed = false;
  shrd.button2LpDuration = 0;
}

void processButton2()
{
  if (shrd.button2ClickStatus == ACTION_ON)
  {
    digitalWrite(PIN_OUT_LED_BUTTON2, HIGH);
  }
  else if (shrd.button2ClickStatus == ACTION_OFF)
  {
    digitalWrite(PIN_OUT_LED_BUTTON2, LOW);
  }
}
//////////////////////////

void processAuxEvent(uint8_t buttonId, bool isLongPress)
{

  // process AUX order -- button 1
  if (((buttonId == 1) && (!isLongPress) && (settings.getS3F().Button_1_short_press_action == settings.LIST_Button_press_action_Aux_on_off)) ||
      ((buttonId == 1) && (isLongPress) && (settings.getS3F().Button_1_long_press_action == settings.LIST_Button_press_action_Aux_on_off)) ||
      ((buttonId == 2) && (!isLongPress) && (settings.getS3F().Button_2_short_press_action == settings.LIST_Button_press_action_Aux_on_off)) ||
      ((buttonId == 2) && (isLongPress) && (settings.getS3F().Button_2_long_press_action == settings.LIST_Button_press_action_Aux_on_off)))
  {
    if (shrd.auxOrder == 0)
    {
      shrd.auxOrder = 1;
    }
    else
    {
      shrd.auxOrder = 0;
    }
    blh.notifyAuxOrder(shrd.auxOrder);

    Serial.print("processAuxEvent => ok / ");
    Serial.println(shrd.auxOrder);

    char print_buffer[500];
    sprintf(print_buffer, "processAuxEvent : %d", shrd.auxOrder);
    blh.notifyBleLogs(print_buffer);
  }
}

void processSpeedLimiterEvent(uint8_t buttonId, bool isLongPress)
{

  // process SpeedLimiter
  if (((buttonId == 1) && (!isLongPress) && (settings.getS3F().Button_1_short_press_action == settings.LIST_Button_press_action_Startup_speed_limitation_on_off)) ||
      ((buttonId == 1) && (isLongPress) && (settings.getS3F().Button_1_long_press_action == settings.LIST_Button_press_action_Startup_speed_limitation_on_off)) ||
      ((buttonId == 2) && (!isLongPress) && (settings.getS3F().Button_2_short_press_action == settings.LIST_Button_press_action_Startup_speed_limitation_on_off)) ||
      ((buttonId == 2) && (isLongPress) && (settings.getS3F().Button_2_long_press_action == settings.LIST_Button_press_action_Startup_speed_limitation_on_off)))
  {
    if (shrd.speedLimiter == 0)
    {
      shrd.speedLimiter = 1;
    }
    else
    {
      shrd.speedLimiter = 0;
    }
    blh.notifySpeedLimiterStatus(shrd.speedLimiter);

    Serial.print("notifySpeedLimiterStatus => ok / ");
    Serial.println(shrd.speedLimiter);

    char print_buffer[500];
    sprintf(print_buffer, "notifySpeedLimiterStatus : %d", shrd.speedLimiter);
    blh.notifyBleLogs(print_buffer);
  }
}

void processLockEvent(uint8_t buttonId, bool isLongPress)
{

  // process SpeedLimiter
  if (((buttonId == 1) && (!isLongPress) && (settings.getS3F().Button_1_short_press_action == settings.LIST_Button_press_action_Anti_theft_manual_lock_on)) ||
      ((buttonId == 1) && (isLongPress) && (settings.getS3F().Button_1_long_press_action == settings.LIST_Button_press_action_Anti_theft_manual_lock_on)) ||
      ((buttonId == 2) && (!isLongPress) && (settings.getS3F().Button_2_short_press_action == settings.LIST_Button_press_action_Anti_theft_manual_lock_on)) ||
      ((buttonId == 2) && (isLongPress) && (settings.getS3F().Button_2_long_press_action == settings.LIST_Button_press_action_Anti_theft_manual_lock_on)))
  {
    blh.setBleLock(true);
    blh.notifyBleLock();

    Serial.println("processLockEvent => ok / ON");

    char print_buffer[500];
    sprintf(print_buffer, "processLockEvent");
    blh.notifyBleLogs(print_buffer);
  }
}

void processAux()
{
  /*
  if (shrd.auxOrder == 1)
  {
    digitalWrite(PIN_OUT_RELAY, 1);
  }
  else
  {
    digitalWrite(PIN_OUT_RELAY, 0);
  }
  */
}

void processDHT()
{
  static unsigned long measurement_timestamp = millis();

  /* Measure once every four seconds. */
  if (millis() - measurement_timestamp > 5000ul)
  {

    float temperature;
    float humidity;

    if (dht_sensor.measure(&temperature, &humidity) == true)
    {
      measurement_timestamp = millis();

#if DEBUG_DISPLAY_DHT
      Serial.print("T = ");
      Serial.print(temperature, 1);
      Serial.print(" deg. C, H = ");
      Serial.print(humidity, 1);
      Serial.println("%");
#endif

      shrd.currentTemperature = temperature;
      shrd.currentHumidity = humidity;
    }
  }
}

void processVoltage()
{

  voltageStatus = analogRead(PIN_IN_VOLTAGE);

  // eject false reading
  if (voltageStatus == 4095)
  {
    /*
    Serial.print("Voltage read : ");
    Serial.print(voltageStatus);
    Serial.println(" => eject ");
*/

    char print_buffer[500];
    sprintf(print_buffer, "Voltage read 4095 ==> eject");
    blh.notifyBleLogs(print_buffer);

    return;
  }
  if (voltageStatus < 900)
  {
    /*
    Serial.print("Voltage read : ");
    Serial.print(voltageStatus);
    Serial.println(" => eject ");
    */

    char print_buffer[500];
    sprintf(print_buffer, "Voltage read <900 ==> eject");
    blh.notifyBleLogs(print_buffer);

    return;
  }

  voltageInMilliVolts = ((voltageStatus * ANALOG_TO_VOLTS_A) + ANALOG_TO_VOLTS_B) * 1000;

  //double correctedValue = -0.000000000000016 * pow(voltageStatus, 4) + 0.000000000118171 * pow(voltageStatus, 3) - 0.000000301211691 * pow(voltageStatus, 2) + 0.001109019271794 * voltageStatus + 0.034143524634089;
  //voltageInMilliVolts = correctedValue * 25.27 * 1000;

  voltageFilter.in(voltageInMilliVolts);
  voltageRawFilter.in(voltageStatus);
  shrd.voltageFilterMean = voltageFilter.getMean();

#if DEBUG_DISPLAY_VOLTAGE
  Serial.print("Voltage read : ");
  Serial.print(voltageStatus);
  Serial.print(" / in voltage mean : ");
  Serial.print(voltageRawFilter.getMean());
  Serial.print(" / in volts : ");
  Serial.print(voltageInMilliVolts / 1000.0);
  Serial.print(" / in volts mean : ");
  Serial.print(voltageFilter.getMean() / 1000.0);
  Serial.print(" / iloop : ");
  Serial.println(i_loop);

  /*
  Serial.print(" / in correctedValue : ");
  Serial.print(correctedValue); 
  Serial.print(" / in volts2 : ");
  Serial.println(correctedValue * 25.27); 
  */
#endif

#if DEBUG_BLE_DISPLAY_VOLTAGE
  char print_buffer[500];
  sprintf(print_buffer, "Voltage / read : %d / mean : %d / mean volts : %0.1f ", voltageStatus, voltageRawFilter.getMean(), voltageFilter.getMean() / 1000.0);
  blh.notifyBleLogs(print_buffer);
#endif
}

void processCurrent()
{
  int curerntRead = analogRead(PIN_IN_CURRENT);
  int currentInMillamps = (curerntRead - currentFilterInit.getMean()) * (1000.0 / ANALOG_TO_CURRENT);

  // current rest value
  currentFilter.in(currentInMillamps);
  shrd.currentFilterMean = currentFilter.getMeanWithoutExtremes(10);

  if ((shrd.speedCurrent == 0) && (shrd.currentCalibOrder > 0))
  {

    shrd.currentCalibOrder--;
    currentFilterInit.in(curerntRead);

#if DEBUG_DISPLAY_CURRENT
    if (shrd.currentCalibOrder == 1)
      Serial.println("Current calibration end ... ");
#endif
  }

  /*
#if DEBUG_DISPLAY_CURRENT
  Serial.print("Current read : ");
  Serial.print(curerntRead);
  Serial.print(" / currentFilterInit mean : ");
  Serial.print(currentFilterInit.getMean());
  Serial.print(" / in amperes : ");
  Serial.println(currentInMillamps / 1000.0);
#endif
*/
}

//////------------------------------------
//////------------------------------------
////// Main loop

void loop()
{

  // handle Wifi OTA
  if (shrd.inOtaMode)
  {
    blh.deinit();
    OTA_loop();
    return;
  }

#if CONTROLLER_MINIMOTORS
  minomoCntrl.processMinimotorsSerial();
#endif
#if CONTROLLER_VESC
  {

    if (i_loop % 10 == 1)
    {
      //Serial.println(">>>>>>>>>>> readVescValues");

      vescCntrl.requestVescValues();
    }

    if (i_loop % 10 == 9)
    {
      //Serial.println(">>>>>>>>>>> processVescSerial");
      processVescSerial();
    }
  }
#endif
  blh.processBLE();

  button1.tick();
  button2.tick();
  processButton1();
#if DEBUG_DISPLAY_BUTTON1
  displayButton1();
#endif

  processButton2();
#if DEBUG_DISPLAY_BUTTON2
  displayButton2();
#endif

  processAux();

  if (i_loop % 10 == 0)
  {
    processVoltage();
  }

  if ((i_loop % 10 == 2) || (i_loop % 10 == 7))
  {
    //modifyBrakeFromLCD();
    //displayBrake();
    getBrakeFromAnalog();
  }

#if CONTROLLER_MINIMOTORS
  if (i_loop % 10 == 4)
  {
    processCurrent();
  }
#endif

  // keep it fast (/100 not working)
  if (i_loop % 10 == 6)
  {
    processDHT();
  }

#if TEST_ADC_DAC_REFRESH

  if ((i_loop % 10 == 3) || (i_loop % 10 == 9))
  {
    uint32_t dacOutput = shrd.brakeAnalogValue * 1.2;
    if (dacOutput > 4095)
      dacOutput = 4095;

    //dacOutput = (i_loop / 10) % 4096;
    dac.setVoltage(dacOutput /*i_loop % 4096*/, false);

    char print_buffer[500];
    sprintf(print_buffer, "brake raw : %d / dacOutput : %d",
            brakeAnalogValue,
            dacOutput);
    Serial.println(print_buffer);
  }
#endif

#if READ_THROTTLE
  throttleAnalogValue = analogRead(PIN_IN_THROTTLE);
#endif

#if TFT_ENABLED
  //tftUpdateData(i_loop);
#endif

  // Give a time for ESP
  delay(1);
  //delayMicroseconds(1000);
  //yield();

#if DEBUG_TIMELOOP_NS
  Serial.print("> ");
  Serial.print(micros() - timeLoop - 1000);
  Serial.print(" / i_loop : ");
  Serial.println(i_loop);
  timeLoop = micros();
#endif

#if DEBUG_TIMELOOP_MS
  Serial.print("> ");
  Serial.print(millis() - timeLoop - 1);
  Serial.print(" / i_loop : ");
  Serial.println(i_loop);
  timeLoop = millis();
#endif

  i_loop++;

  resetWatchdog();
}

/////////// End