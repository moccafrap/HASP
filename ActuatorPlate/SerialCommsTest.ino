#include <avr/wdt.h> // watchdog timer
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>
#include <utility/imumaths.h>
#include <Adafruit_MCP9808.h>
#include <RTClib.h> // RTC and SoftRTC
#include <Adafruit_BME280.h>
#include <SD.h>
#include <Adafruit_ADS1015.h>
#include <CCSDS.h>
#include <ccsds_xbee.h>
#include <ccsds_util.h>
#include <SSC.h>

int COMMAND = 0;
uint8_t ReadData;
int COMMAND_PIN = 0;
#define COMMAND_PIN 6 
#define ACTUATOR_PIN_HBRIDGE_A 3 // yellow
#define ACTUATOR_PIN_HBRIDGE_B 4 // green


/* APIDs */
#define COMMAND_APID 500
#define STATUS_APID 501
#define PACKET_COUNTER_APID 510
#define ENVIRONMENTAL_PACKET_ID 0x16
#define POWER_PACKET_APID 530
#define IMU_PACKET_ID 0x18

long start_millis;
#define CYCLE_DELAY 100 // time between execution cycles [ms]
bool extended = false;
bool armed = true;

//// Enumerations
// logging flag
#define LOG_RCVD 1
#define LOG_SEND 0


/* response codes */
#define INIT_RESPONSE 0xAC
#define RETRACT_RESPONSE 0xE1

// CAMERA FcnCodes
#define COMMAND_NOOP 0
#define REQUEST_ENVIRONMENTAL_DATA 10
#define REQUEST_POWER_DATA 20
#define REQUEST_IMU_DATA 30


//// Declare objects
Adafruit_BNO055 bno = Adafruit_BNO055(-1, 0x29);
Adafruit_MCP9808 tempsensor = Adafruit_MCP9808();
RTC_DS1307 rtc;
RTC_Millis SoftRTC;    // This is the millis()-based software RTC
Adafruit_BME280 bme;
Adafruit_ADS1015 ads(0x4A);
SSC ssc(0x28, 255);

//// Serial object aliases
// so that the user doesn't have to keep track of which is which
#define debug_serial Serial

//// Data Structures
// imu data
struct IMUData_s
{
        uint8_t system_calibration;
        uint8_t accel_calibration;
        uint8_t gyro_calibration;
        uint8_t mag_calibration;
        float accel_x;
        float accel_y;
        float accel_z;
        float gyro_x;
        float gyro_y;
        float gyro_z;
        float mag_x;
        float mag_y;
        float mag_z;
};
// environmental data
struct ENVData_s
{
        float bme_pres;
        float bme_temp;
        float bme_humid;
        float ssc_pres;
        float ssc_temp;
        float bno_temp;
        float mcp_temp;
};
// power data
struct PWRData_s
{
        float batt_volt;
        float i_consump;
};

//// Timing
// timing counters
uint16_t imu_read_ctr = 0;
uint16_t pwr_read_ctr = 0;
uint16_t env_read_ctr = 0;

// rate setting
// sensors will be read every X cycles
uint16_t imu_read_lim = 10;
uint16_t pwr_read_lim = 100;
uint16_t env_read_lim = 100;

// interface counters
uint16_t CmdExeCtr = 0;
uint16_t CmdRejCtr = 0;
uint32_t XbeeRcvdByteCtr = 0;
uint32_t XbeeSentByteCtr = 0;


uint32_t destaddr = 2;
// logging files
File LogFile;
File IMULogFile;
File ENVLogFile;
File PWRLogFile;
File initLogFile;

//// Function prototypes
void command_response(uint8_t data[], uint8_t data_len, struct IMUData_s IMUData, struct ENVData_s ENVData, struct PWRData_s PWRData);

// interface
void send_and_log(uint8_t data[], uint8_t data_len);
void logPkt(File file, uint8_t data[], uint8_t len, uint8_t received_flg);

// pkt creation
uint16_t create_HK_pkt(uint8_t HK_Pkt_Buff[]);
uint16_t create_IMU_pkt(uint8_t HK_Pkt_Buff[], struct IMUData_s IMUData);
uint16_t create_PWR_pkt(uint8_t HK_Pkt_Buff[], struct PWRData_s PWRData);
uint16_t create_ENV_pkt(uint8_t HK_Pkt_Buff[], struct ENVData_s ENVData);
uint16_t create_Status_pkt(uint8_t HK_Pkt_Buff[], uint8_t message);
uint16_t _APID2;

// sensor reading
void read_imu(struct IMUData_s *IMUData);
void read_env(struct ENVData_s *ENVData);
void read_pwr(struct PWRData_s *PWRData);

// log data
void log_imu(struct IMUData_s IMUData, File IMULogFile);
void log_env(struct ENVData_s ENVData, File ENVLogFile);
void log_pwr(struct PWRData_s PWRData, File PWRLogFile);

// utility
void print_time(File file);

//// CODE:
void setup(void)
{
    /* setup()
     *
     * Disables watchdog timer (in case its on)
     * Initalizes all the link hardware/software including:
     *   Serial
     *   RTC
     *   SoftRTC
     *   BNO
     *   MCP
     *   BME
     *   SSC
     *   ADS
     *   SD card
     *   Log files
     */
    // disable the watchdog timer immediately in case it was on because of a
    // commanded reboot
    wdt_disable();

    //// Init serial ports:
    /*  aliases defined above are used to reduce confusion about which serial
     *    is connected to what interface
  
     */
    debug_serial.begin(250000);

    debug_serial.println("GoGoGadget Camera payload!");

    //// BNO
    if (!bno.begin())
    {
        debug_serial.println("BNO055 NOT detected.");
    }
    else
    {
        debug_serial.println("BNO055 detected!");
    }
    delay(1000);
    bno.setExtCrystalUse(true);

    //// MCP9808
    if (!tempsensor.begin(0x18))
    {
        debug_serial.println("MCP9808 NOT detected.");
    }
    else
    {
        debug_serial.println("MCP9808 detected!");
    }

    //// RTC
    /* The RTC is used so that the log files contain timestamps. If the RTC
     *  is not running (because no battery is inserted) the RTC will be initalized
     *  to the time that this sketch was compiled at.
     */
    if (!rtc.begin())
    {
        debug_serial.println("DS1308 NOT detected.");
    }
    else
    {
        debug_serial.println("DS1308 detected!");
    }

    if (!rtc.isrunning())
    {
        debug_serial.println("RTC is NOT running!");
        // following line sets the RTC to the date & time this sketch was compiled
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        // This line sets the RTC with an explicit date & time, for example to set
        // January 21, 2014 at 3am you would call:
        // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
    }

    //// SoftRTC (for subsecond precision)
    SoftRTC.begin(rtc.now());    // Initialize SoftRTC to the current time
    start_millis = millis();    // get the current millisecond count

    //// Init BME
    if (!bme.begin(0x76))
    {
        debug_serial.println("BME280 NOT detected.");
    }
    else
    {
        debug_serial.println("BME280 detected!");
    }

    //// Init ADS
    ads.begin();
    ads.setGain(GAIN_ONE);
    debug_serial.println("Initialized ADS1015");

    //// Init SD card
    SPI.begin();
    pinMode(53, OUTPUT);
    if (!SD.begin(53))
    {
        debug_serial.println("SD Card NOT detected.");
    }
    else
    {
        debug_serial.println("SD Card detected!");
    }

    //// Init SSC
    //  set min / max reading and pressure, see datasheet for the values for your
    //  sensor
    ssc.setMinRaw(0);
    ssc.setMaxRaw(16383);
    ssc.setMinPressure(0.0);
    ssc.setMaxPressure(30);

    //  start the sensor
    debug_serial.print("SSC start: ");
    debug_serial.println(ssc.start());

    //MicroSD
    // appends to current file
    // NOTE: Filenames must be shorter than 8 characters
    File IMULogFile = SD.open("IMU_LOG.txt", FILE_WRITE);
    File PWRLogFile = SD.open("PWR_LOG.txt", FILE_WRITE);
    File ENVLogFile = SD.open("ENV_LOG.txt", FILE_WRITE);
    
    pinMode(COMMAND_PIN, INPUT);
    pinMode(ACTUATOR_PIN_HBRIDGE_A, OUTPUT);
    pinMode(ACTUATOR_PIN_HBRIDGE_B, OUTPUT);

    digitalWrite(ACTUATOR_PIN_HBRIDGE_A, LOW);
    digitalWrite(ACTUATOR_PIN_HBRIDGE_B, LOW);

    armed = true;
    delay(10000);
}

void loop(void)
{
    /*  loop()
     *
     *  Reads sensor
     *  Log sensors
     *  Reads from xbee and processes any data
     */


// declare structures to store data
    IMUData_s IMUData;
    PWRData_s PWRData;
    ENVData_s ENVData;

// increment read counters
    imu_read_ctr++;
    pwr_read_ctr++;
    env_read_ctr++;

// read sensors if time between last read
    if (imu_read_ctr > imu_read_lim)
    {
        read_imu(&IMUData);
        log_imu(IMUData, IMULogFile);
        imu_read_ctr = 0;
    }
    if (pwr_read_ctr > pwr_read_lim)
    {
        read_pwr(&PWRData);
        log_pwr(PWRData, PWRLogFile);
        pwr_read_ctr = 0;
    }
    if (env_read_ctr > env_read_lim)
    {
        read_env(&ENVData);
        log_env(ENVData, ENVLogFile);
        env_read_ctr = 0;
    }

// initalize a counter to record how many bytes were read this iteration
    int BytesRead = 0;


        // respond to it
    command_response(IMUData, ENVData, PWRData);

  
    COMMAND = digitalRead(COMMAND_PIN);
    
    
    if (COMMAND == LOW){
      delay(60);
      COMMAND = digitalRead(COMMAND_PIN);
          if (COMMAND == LOW){
          retract(25);
          }
    }


// wait a bit
    delay(10);

}

void command_response(struct IMUData_s IMUData, struct ENVData_s ENVData, struct PWRData_s PWRData)
{
    /*  command_response()
     *
     *  given an array of data (presumably containing a CCSDS packet), check if the
     *  packet is a CAMERA command packet, and if so, process it
     */


    
    uint16_t pktLength = 0;
    uint8_t Pkt_Buff[100];
    
    //case REQUEST_ENVIRONMENTAL_DATA:
    // Requests that an HK packet be sent to the specified xbee address
    /*  Command format:
    *   CCSDS Command Header (8 bytes)
    *   Xbee address (1 byte)
    */
    
    
    // create a HK pkt
    pktLength = create_ENV_pkt(Pkt_Buff, ENVData);
    
    // send the HK packet via xbee and log it
    send_and_log(Pkt_Buff, pktLength);
    
    
    // PWR_Req
    //case REQUEST_POWER_DATA:
    // Requests that an HK packet be sent to the specified xbee address
    /*  Command format:
    *   CCSDS Command Header (8 bytes)
    *   Xbee address (1 byte)
    */
    
    /*
    // create a HK pkt
    pktLength = create_PWR_pkt(Pkt_Buff, PWRData);
    
    // send the HK packet via xbee and log it
    send_and_log(Pkt_Buff, pktLength);
    
    
    
    // IMU_Req
    //case REQUEST_IMU_DATA:
    // Requests that an HK packet be sent to the specified xbee address
    /*  Command format:
    *   CCSDS Command Header (8 bytes)
    *   Xbee address (1 byte)
    */
    
    
    // create a HK pkt
    pktLength = create_IMU_pkt(Pkt_Buff, IMUData);
    
    // send the HK packet via xbee and log it
    send_and_log(Pkt_Buff, pktLength);

}

void log_imu(struct IMUData_s IMUData, File IMULogFile)
{
// print the time to the file
    print_time(IMULogFile);

// print the sensor values
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.system_calibration);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.accel_calibration);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.gyro_calibration);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.mag_calibration);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.accel_x);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.accel_y);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.accel_z);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.gyro_x);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.gyro_y);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.gyro_z);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.mag_x);
    IMULogFile.print(", ");
    IMULogFile.print(IMUData.mag_y);
    IMULogFile.print(", ");
    IMULogFile.println(IMUData.mag_z);

    IMULogFile.flush();
}

void log_env(struct ENVData_s ENVData, File ENVLogFile)
{
// print the time to the file
    print_time(ENVLogFile);

// print the sensor values
    ENVLogFile.print(", ");
    ENVLogFile.print(ENVData.bme_pres);
    ENVLogFile.print(", ");
    ENVLogFile.print(ENVData.bme_temp);
    ENVLogFile.print(", ");
    ENVLogFile.print(ENVData.bme_humid);
    ENVLogFile.print(", ");
    ENVLogFile.print(ENVData.ssc_pres);
    ENVLogFile.print(", ");
    ENVLogFile.print(ENVData.ssc_temp);
    ENVLogFile.print(", ");
    ENVLogFile.print(ENVData.bno_temp);
    ENVLogFile.print(", ");
    ENVLogFile.println(ENVData.mcp_temp);

    ENVLogFile.flush();
}

void log_pwr(struct PWRData_s PWRData, File PWRLogFile)
{
// print the time to the file
    print_time(PWRLogFile);

// print the sensor values
    PWRLogFile.print(", ");
    PWRLogFile.print(PWRData.batt_volt);
    PWRLogFile.print(", ");
    PWRLogFile.println(PWRData.i_consump);

    PWRLogFile.flush();
}

void read_env(struct ENVData_s *ENVData)
{
//BME280
    ENVData->bme_pres = bme.readPressure() / 100.0F;    // hPa
    ENVData->bme_temp = bme.readTemperature();    // degC
    ENVData->bme_humid = bme.readHumidity();    // %

//  SSC
    /*
     ssc.update();
     ENVData->ssc_pres = ssc.pressure(); // PSI
     ENVData->ssc_temp = ssc.temperature(); // degC
     */
// BNO
    ENVData->bno_temp = bno.getTemp();

//MCP9808
    ENVData->mcp_temp = tempsensor.readTempC();    // degC
}

void read_pwr(struct PWRData_s *PWRData)
{

    PWRData->batt_volt = ((float) ads.readADC_SingleEnded(2)) * 0.002 * 3.0606;    // V
    PWRData->i_consump = (((float) ads.readADC_SingleEnded(3)) * 0.002 - 2.5) * 10;
}

void read_imu(struct IMUData_s *IMUData)
{

    uint8_t system_cal, gyro_cal, accel_cal, mag_cal = 0;
    bno.getCalibration(&system_cal, &gyro_cal, &accel_cal, &mag_cal);

// get measurements
    imu::Vector<3> mag = bno.getVector(Adafruit_BNO055::VECTOR_MAGNETOMETER);    // (values in uT, micro Teslas)
    imu::Vector<3> gyro = bno.getVector(Adafruit_BNO055::VECTOR_GYROSCOPE);    // (values in rps, radians per second)
    imu::Vector<3> accel = bno.getVector(Adafruit_BNO055::VECTOR_ACCELEROMETER);    // (values in m/s^2)

// assign them into structure fields
    IMUData->system_calibration = system_cal;
    IMUData->accel_calibration = accel_cal;
    IMUData->gyro_calibration = gyro_cal;
    IMUData->mag_calibration = mag_cal;
    IMUData->accel_x = accel.x();
    IMUData->accel_y = accel.y();
    IMUData->accel_z = accel.z();
    IMUData->gyro_x = gyro.x();
    IMUData->gyro_y = gyro.y();
    IMUData->gyro_z = gyro.z();
    IMUData->mag_x = mag.x();
    IMUData->mag_y = mag.y();
    IMUData->mag_z = mag.z();

}

/*uint16_t create_HK_pkt(uint8_t HK_Pkt_Buff[])
{
    /*  create_HK_pkt()
     *
     *  Creates an HK packet containing the values of all the interface counters.
     *  Packet data is filled into the memory passed in as the argument
     *
     */
     /*
// get the current time from the RTC
    DateTime now = rtc.now();

// initalize counter to record length of packet
    uint16_t payloadSize = 0;

// add length of primary header
    payloadSize += sizeof(CCSDS_PriHdr_t);

// Populate primary header fields:
    setAPID(HK_Pkt_Buff, PACKET_COUNTER_APID);
    setSecHdrFlg(HK_Pkt_Buff, 1);
    setPacketType(HK_Pkt_Buff, 0);
    setVer(HK_Pkt_Buff, 0);
    setSeqCtr(HK_Pkt_Buff, 0);
    setSeqFlg(HK_Pkt_Buff, 0);

// add length of secondary header
    payloadSize += sizeof(CCSDS_TlmSecHdr_t);

// Populate the secondary header fields:
    setTlmTimeSec(HK_Pkt_Buff, now.unixtime() / 1000L);
    setTlmTimeSubSec(HK_Pkt_Buff, now.unixtime() % 1000L);

// Add counter values to the pkt
    payloadSize = addIntToTlm(CmdExeCtr, HK_Pkt_Buff, payloadSize);    // Add counter of sent packets to message
    payloadSize = addIntToTlm(CmdRejCtr, HK_Pkt_Buff, payloadSize);    // Add counter of sent packets to message
    payloadSize = addIntToTlm(XbeeRcvdByteCtr, HK_Pkt_Buff, payloadSize);    // Add counter of sent packets to message
    payloadSize = addIntToTlm(XbeeSentByteCtr, HK_Pkt_Buff, payloadSize);    // Add counter of sent packets to message
    payloadSize = addIntToTlm(millis() / 1000L, HK_Pkt_Buff, payloadSize);    // Timer

// fill the length field
    setPacketLength(HK_Pkt_Buff, payloadSize);

    return payloadSize;

}*/

uint16_t create_ENV_pkt(uint8_t HK_Pkt_Buff[], struct ENVData_s ENVData)
{
    /*  create_ENV_pkt()
     *
     *  Creates an HK packet containing the values of all the interface counters.
     *  Packet data is filled into the memory passed in as the argument
     *
     */
// get the current time from the RTC
    DateTime now = rtc.now();

// initalize counter to record length of packet
    uint16_t payloadSize = 0;

// add length of primary header
    payloadSize += sizeof(CCSDS_PriHdr_t);

// Populate primary header fields:
    setAPID(HK_Pkt_Buff, ENVIRONMENTAL_PACKET_ID);
    setSecHdrFlg(HK_Pkt_Buff, 1);
    setPacketType(HK_Pkt_Buff, 0);
    setVer(HK_Pkt_Buff, 0);
    setSeqCtr(HK_Pkt_Buff, 0);
    setSeqFlg(HK_Pkt_Buff, 0);

// add length of secondary header
    payloadSize += sizeof(CCSDS_TlmSecHdr_t);

// Populate the secondary header fields:
    setTlmTimeSec(HK_Pkt_Buff, now.unixtime() / 1000L);
    setTlmTimeSubSec(HK_Pkt_Buff, now.unixtime() % 1000L);

// Add counter values to the pkt
    payloadSize = addFloatToTlm(ENVData.bme_pres, HK_Pkt_Buff, payloadSize);    // Add bme pressure to message [Float]
    payloadSize = addFloatToTlm(ENVData.bme_temp, HK_Pkt_Buff, payloadSize);    // Add bme temperature to message [Float]
    payloadSize = addFloatToTlm(ENVData.bme_humid, HK_Pkt_Buff, payloadSize);    // Add bme humidity to message [Float]
    payloadSize = addFloatToTlm(ENVData.ssc_pres, HK_Pkt_Buff, payloadSize);    // Add ssc pressure to message [Float]
    payloadSize = addFloatToTlm(ENVData.ssc_temp, HK_Pkt_Buff, payloadSize);    // Add ssc temperature to messsage [Float]
    payloadSize = addFloatToTlm(ENVData.bno_temp, HK_Pkt_Buff, payloadSize);    // Add bno temperature to message [Float]
    payloadSize = addFloatToTlm(ENVData.mcp_temp, HK_Pkt_Buff, payloadSize);    // Add mcp temperature to message [Float]

// fill the length field
    setPacketLength(HK_Pkt_Buff, payloadSize);

    return payloadSize;

}

uint16_t create_PWR_pkt(uint8_t HK_Pkt_Buff[], struct PWRData_s PWRData)
{
    /*  create_ENV_pkt()
     *
     *  Creates an HK packet containing the values of all the interface counters.
     *  Packet data is filled into the memory passed in as the argument
     *
     */
// get the current time from the RTC
    DateTime now = rtc.now();

// initalize counter to record length of packet
    uint16_t payloadSize = 0;

// add length of primary header
    payloadSize += sizeof(CCSDS_PriHdr_t);

// Populate primary header fields:
    setAPID(HK_Pkt_Buff, POWER_PACKET_APID);
    setSecHdrFlg(HK_Pkt_Buff, 1);
    setPacketType(HK_Pkt_Buff, 0);
    setVer(HK_Pkt_Buff, 0);
    setSeqCtr(HK_Pkt_Buff, 0);
    setSeqFlg(HK_Pkt_Buff, 0);

// add length of secondary header
    payloadSize += sizeof(CCSDS_TlmSecHdr_t);

// Populate the secondary header fields:
    setTlmTimeSec(HK_Pkt_Buff, now.unixtime() / 1000L);
    setTlmTimeSubSec(HK_Pkt_Buff, now.unixtime() % 1000L);

// Add counter values to the pkt
    payloadSize = addFloatToTlm(PWRData.batt_volt, HK_Pkt_Buff, payloadSize);    // Add battery voltage to message [Float]
    payloadSize = addFloatToTlm(PWRData.i_consump, HK_Pkt_Buff, payloadSize);    // Add current consumption to message [Float]

// fill the length field
    setPacketLength(HK_Pkt_Buff, payloadSize);

    return payloadSize;

}

uint16_t create_IMU_pkt(uint8_t HK_Pkt_Buff[], struct IMUData_s IMUData)
{
    /*  create_IMU_pkt()
     *
     *  Creates an HK packet containing the values of all the interface counters.
     *  Packet data is filled into the memory passed in as the argument
     *
     */
// get the current time from the RTC
    DateTime now = rtc.now();

// initalize counter to record length of packet
    uint16_t payloadSize = 0;

// add length of primary header
    payloadSize += sizeof(CCSDS_PriHdr_t);

// Populate primary header fields:
    setAPID(HK_Pkt_Buff, IMU_PACKET_ID);
    setSecHdrFlg(HK_Pkt_Buff, 1);
    setPacketType(HK_Pkt_Buff, 0);
    setVer(HK_Pkt_Buff, 0);
    setSeqCtr(HK_Pkt_Buff, 0);
    setSeqFlg(HK_Pkt_Buff, 0);

// add length of secondary header
    payloadSize += sizeof(CCSDS_TlmSecHdr_t);

// Populate the secondary header fields:
    setTlmTimeSec(HK_Pkt_Buff, now.unixtime() / 1000L);
    setTlmTimeSubSec(HK_Pkt_Buff, now.unixtime() % 1000L);

// Add counter values to the pkt
    payloadSize = addIntToTlm(IMUData.system_calibration, HK_Pkt_Buff, payloadSize);    // Add system cal status to message [uint8_t]
    payloadSize = addIntToTlm(IMUData.accel_calibration, HK_Pkt_Buff, payloadSize);    // Add accelerometer cal status to message [uint8_t]
    payloadSize = addIntToTlm(IMUData.gyro_calibration, HK_Pkt_Buff, payloadSize);    // Add gyro cal status to message [uint8_t]
    payloadSize = addIntToTlm(IMUData.mag_calibration, HK_Pkt_Buff, payloadSize);    // Add mnagnetomter cal status to message [uint8_t]
    payloadSize = addFloatToTlm(IMUData.accel_x, HK_Pkt_Buff, payloadSize);    // Add battery accelerometer x to message [Float]
    payloadSize = addFloatToTlm(IMUData.accel_y, HK_Pkt_Buff, payloadSize);    // Add battery accelerometer y to message [Float]
    payloadSize = addFloatToTlm(IMUData.accel_z, HK_Pkt_Buff, payloadSize);    // Add battery accelerometer z to message [Float]
    payloadSize = addFloatToTlm(IMUData.gyro_x, HK_Pkt_Buff, payloadSize);    // Add battery accelerometer x to message [Float]
    payloadSize = addFloatToTlm(IMUData.gyro_y, HK_Pkt_Buff, payloadSize);    // Add battery accelerometer y to message [Float]
    payloadSize = addFloatToTlm(IMUData.gyro_z, HK_Pkt_Buff, payloadSize);    // Add battery accelerometer z to message [Float]
    payloadSize = addFloatToTlm(IMUData.mag_x, HK_Pkt_Buff, payloadSize);    // Add battery accelerometer x to message [Float]
    payloadSize = addFloatToTlm(IMUData.mag_y, HK_Pkt_Buff, payloadSize);    // Add battery accelerometer y to message [Float]
    payloadSize = addFloatToTlm(IMUData.mag_z, HK_Pkt_Buff, payloadSize);    // Add battery accelerometer z to message [Float]

// fill the length field
    setPacketLength(HK_Pkt_Buff, payloadSize);

    return payloadSize;

}

void send_and_log(uint8_t data[], uint8_t data_len)
{
    /*  xbee_send_and_log()
     *
     *  Sends the given data out over the xbee and adds an entry to the xbee log file.
     *  Also updates the radio sent counter.
     */

// send the data via xbee
    Serial.write(data, data_len);

// log the sent data
    //logPkt(LogFile, data, sizeof(data), 0);

// update the xbee send ctr
    XbeeSentByteCtr += data_len;
}

//void logPkt(File file, uint8_t data[], uint8_t len, uint8_t received_flg)
//{
    /*  logPkt()
     *
     *  Prints an entry in the given log file containing the given data. Will prepend an
     *  'S' if the data was sent or an 'R' is the data was received based on the value
     *  of the received_flg.
     */

// if the file is open
/*    if (file)
    {

        // prepend an indicator of if the data was received or sent
        // R indicates this was received data
        if (received_flg)
        {
            file.print("R ");
        }
        else
        {
            file.print("S ");
        }

        // Print a timestamp
        print_time(file);

        char buf[50];

        // print the data in hex
        file.print(": ");
        for (int i = 0; i < len; i++)
        {
            sprintf(buf, "%02x, ", data[i]);
            file.print(buf);
        }
        file.println();

        // ensure the data gets written to the file
        file.flush();
    }
}
*/
void print_time(File file)
{
    /*  print_time()
     *
     *  Prints the current time to the given log file
     */

// get the current time from the RTC
    DateTime now = rtc.now();

// print a datestamp to the file
    char buf[50];
    sprintf(buf, "%02d/%02d/%02d %02d:%02d:%02d", now.day(), now.month(), now.year(), now.hour(), now.minute(), now.second());
    file.print(buf);
}

void extend(int pulse_seconds)
{
    controlActuator(1, pulse_seconds);
    extended = true;
}

void retract(int pulse_seconds)
{
    controlActuator(-1, pulse_seconds);
    extended = false;
}

void controlActuator(int direction, int pulse_seconds)
{

// actuator without built-in polarity switch
    if (direction < 0)
    {
        digitalWrite(ACTUATOR_PIN_HBRIDGE_A, LOW);
        digitalWrite(ACTUATOR_PIN_HBRIDGE_B, HIGH);
    }
    else if (direction > 0)
    {
        digitalWrite(ACTUATOR_PIN_HBRIDGE_A, HIGH);
        digitalWrite(ACTUATOR_PIN_HBRIDGE_B, LOW);
    }

    delay(pulse_seconds * 1000);

    digitalWrite(ACTUATOR_PIN_HBRIDGE_A, LOW);
    digitalWrite(ACTUATOR_PIN_HBRIDGE_B, LOW);

}
