/*
    Daemon for communication with SolaX-X1_Mini inverter via RS485
    
    Copyright (C) 2020 - Jens Jordan
    
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*********************************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/*** Defines ********************************************************************************************/

#define SOLARXD_STRING             "SolaXd"
#define VERSION_STRING             "Version 0.2.0 (07-Jan-2021)"

#define DEFAULT_TTY_DEVICE_NAME    "/dev/ttyUSB0"         // serial device
#define DEFAULT_TCP_PORT           6789                   // http-server port
#define DEFAULT_AVERAGE_SAMPLES    10                     // interval use for average calculation (in seconds)
#define DEFAULT_INVERTER_ADDRESS   0x0A                   // must be unique in case of more inverters in the same RS485 bus
#define DEFAULT_LOG_FILE           NULL                   // stderr is used if NULL
#define DEFAULT_LOG_LEVEL          LOG_TRACE              // support for different log levels
#define DEFAULT_TEST_MODE          0                      // enabled / disabled of test & debug code

#define QUALITY_OF_SERVICE_COUNT   100                    // QoS interval (in seconds)
#define TIMEOUT_INVERTER_ONLINE    30                     // in seconds
#define MAX_INDEX_OF_LIVE_DATA     (QUALITY_OF_SERVICE_COUNT - 1)

/*** Macros ********************************************************************************************/

#define lowByte(i)    ( (uint8_t) i )
#define highByte(i)   ( (uint8_t) ( ((int) i) >> 8) )

#define ERROR_MESSAGE(...)   if (arg_LogLevel >= LOG_ERROR ) { log_Message(LOG_ERROR,  fp_log_file, __VA_ARGS__); }
#define NOTICE_MESSAGE(...)  if (arg_LogLevel >= LOG_NOTICE) { log_Message(LOG_NOTICE, fp_log_file, __VA_ARGS__); }
#define INFO_MESSAGE(...)    if (arg_LogLevel >= LOG_INFO  ) { log_Message(LOG_INFO,   fp_log_file, __VA_ARGS__); }
#define DEBUG_MESSAGE(...)   if (arg_LogLevel >= LOG_DEBUG ) { log_Message(LOG_DEBUG,  fp_log_file, __VA_ARGS__); }
#define TRACE_MESSAGE(...)   if (arg_LogLevel >= LOG_TRACE ) { log_Message(LOG_TRACE,  fp_log_file, __VA_ARGS__); }
#define LOG_USE_COLOR        // If the library is compiled with `-DLOG_USE_COLOR` ANSI color escape codes will be used when printing.

/*** Typedefs ******************************************************************************************/

typedef enum
{
    LOG_ERROR = 0,
    LOG_NOTICE,
    LOG_INFO,
    LOG_DEBUG,
    LOG_TRACE
} logLevel_t;

typedef enum
{
    STATE_BROARDCAST,
    STATE_INVERTER_ADDRESS,
    STATE_QUERY_LIVE_DATA
} Solax_StateQuery_t;

typedef enum
{
    ERR_NONE = 0,
    ERR_NO_DATA,
    ERR_INVALID_MSG,
    ERR_CRC_ERROR
} Solax_ErrorQuery_t;

typedef struct
{
    uint8_t Header[2];
    uint8_t Source[2];
    uint8_t Destination[2];
    uint8_t ControlCode;
    uint8_t FunctionCode;
    uint8_t DataLength;
    uint8_t Data[100];
} Solax_Message_t;

typedef struct
{
    bool valid;
    float Temperature;
    float Energy_Today;
    float DC1_Voltage;
    float DC2_Voltage;
    float DC1_Current;
    float DC2_Current;
    float AC_Current;
    float AC_Voltage;
    float Frequency;
    float Power;
    float Energy_Total;
    float Runtime_Total;
    uint8_t Status;
    uint32_t ErrorBits;
} Solax_LiveData_t;

/*** Static Data ******************************************************************************************/

static char*      arg_TTY_Device   = DEFAULT_TTY_DEVICE_NAME;
static int        arg_TCP_Port     = DEFAULT_TCP_PORT;
static int        arg_AV_Samples   = DEFAULT_AVERAGE_SAMPLES;
static int        arg_InverterAddr = DEFAULT_INVERTER_ADDRESS;
static char*      arg_LogFile      = DEFAULT_LOG_FILE;
static logLevel_t arg_LogLevel     = DEFAULT_LOG_LEVEL;
static int        arg_TestMode     = DEFAULT_TEST_MODE;

static int        fd_tty           = -1;    /* File descriptor for serial interface */
static int        fd_sock_server   = -1;    /* File descriptor for network socket */
static FILE*      fp_log_file      = NULL;  /* File pointer for Log-File */

static uint8_t           solax_InverterSerialNumber[15] = "";
static bool              solax_InverterOnline = false;
static float             solax_QualityOfService = 0;
static Solax_LiveData_t  solax_LiveData = {0};

/*** Functions ******************************************************************************************/

void getDateTime(char dateTimeStr[])
{
    struct timespec timeNow;
    struct tm* infoTimeNow;
    int miliSec;

    clock_gettime(CLOCK_REALTIME, &timeNow);
    infoTimeNow = localtime(&timeNow.tv_sec);
    miliSec = timeNow.tv_nsec / 1e6;

    // build time string: "yyyy-MM-DD hh:mm:ss.sss\0"   (len = 24)
    strftime(dateTimeStr, 20, "%Y-%m-%d %H:%M:%S", infoTimeNow);
    sprintf(&dateTimeStr[19], ".%03d", miliSec);
}



void log_Message(int level, FILE* fp, const char fmt[], ...)
{
    va_list args;
    char dateTime[24];
    
    static const char *level_names[] = {"ERROR", "NOTE ", "INFO ", "DEBUG", "TRACE"};
    #ifdef LOG_USE_COLOR
    static const char *level_colors[] = {"\x1b[31m", "\x1b[32m", "\x1b[33m", "\x1b[36m", "\x1b[94m"};
    #endif
    
    if (level > 4) return;
    
    if ((level == LOG_ERROR) && (fp != stderr))
    {
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
    
    getDateTime(dateTime);
    #ifdef LOG_USE_COLOR
    fprintf(fp, "%s %s[%s]\x1b[0m ", dateTime, level_colors[level], level_names[level]);
    #else
    fprintf(fp, "%s [%s] ", dateTime, level_names[level]);
    #endif
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    
    fflush(fp);
}



int log_Bin2Hex(char buff[], const uint8_t data[], const uint8_t dataLen)
{
    int x, i;
    int len = 0;
    
    if (dataLen)
    {
        for (i=0; i < dataLen; i++)
        {
            if ((i % 8) == 0)
            {
                len += sprintf(&buff[len], " ");
            }
            x = data[i];
            len += sprintf(&buff[len], "%02X ", x);
        }
    }
    else
    {
        len = sprintf(buff, " No Data");
    }
    return len;
}


uint16_t solax_CalculateCRC(const uint8_t data[], const uint8_t dataLen)
{
    uint8_t i;
    uint16_t chkSum = 0;
    
    for (i = 0; i <= dataLen; i++)
    {
        chkSum = chkSum + data[i];
    }
    return chkSum;
}


Solax_ErrorQuery_t solax_RS485_Send(Solax_Message_t* txMessage)
{
    int txLen;
    uint8_t msgLen;
    uint16_t crc;
    
    txMessage->Header[0] = 0xAA;
    txMessage->Header[1] = 0x55;
    
    msgLen = txMessage->DataLength + 9;
    crc = solax_CalculateCRC((const uint8_t*)txMessage, msgLen - 1);    // calculate out crc bytes
    
    txMessage->Data[txMessage->DataLength + 0] = highByte(crc);
    txMessage->Data[txMessage->DataLength + 1] =  lowByte(crc);
    msgLen += 2;
    
    txLen = write(fd_tty, txMessage, msgLen);
    //tcdrain(fd_tty);    /* delay for output */   // ??????????
    
    if (txLen != msgLen)
    {
        ERROR_MESSAGE("ComTx: Error transmitting data: %s", strerror(errno));
        return -1;
    }
    
    if (arg_LogLevel >= LOG_TRACE)
    {
        char buff[(3 * sizeof(Solax_Message_t)) + (sizeof(Solax_Message_t) / 8) + 3];
        log_Bin2Hex(buff, (const uint8_t*)txMessage, msgLen);
        TRACE_MESSAGE("ComTx:%s", buff);
    }
    
    return ERR_NONE;
}


Solax_ErrorQuery_t solax_RS485_Receive(Solax_Message_t* rxMessage)
{
    int rxLen; // read length
    uint8_t msgLen;
    uint16_t crc;
    
    rxLen = read(fd_tty, rxMessage, sizeof(Solax_Message_t));
    
    // Test-Mode: Simulation of inverter data
    if (arg_TestMode)
    {
        static int x = 0;
        static const uint8_t rx_msg_1[] = {0xAA, 0x55, 0x00, 0xFF, 0x01, 0x00, 0x10, 0x80, 0x0E, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31, 0x05, 0x75};
        static const uint8_t rx_msg_2[] = {0xAA, 0x55, 0x00, 0x0A, 0x00, 0x00, 0x10, 0x81, 0x01, 0x06, 0x01, 0xA1};
        static const uint8_t rx_msg_3[] = {0xAA, 0x55, 0x00, 0x0A, 0x01, 0x00, 0x11, 0x82, 0x32, 0x00, 0x0B, 0x00, 0x01, 0x06, 0xDD, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x15, 0x09, 0x21, 0x13, 0x87, 0x01, 0xE7, 0xFF, 0xFF, 0x00, 0x00, 0x12, 0xD3, 0x00, 0x00, 0x0A, 0x0F, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x9C};
        static const uint8_t rx_msg_4[] = {0xAA, 0x55, 0x00, 0x0A, 0x01, 0x00, 0x11, 0x82, 0x32, 0x00, 0x0B, 0x00, 0x01, 0x06, 0xCB, 0x00, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x14, 0x09, 0x22, 0x13, 0x89, 0x01, 0xD7, 0xFF, 0xFF, 0x00, 0x00, 0x12, 0xD3, 0x00, 0x00, 0x0A, 0x0F, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x7B};
        if (x == 1) {rxLen = sizeof(rx_msg_1); memcpy(rxMessage, rx_msg_1, rxLen);}
        if (x == 2) {rxLen = sizeof(rx_msg_2); memcpy(rxMessage, rx_msg_2, rxLen);}
        if (x == 3) {rxLen = sizeof(rx_msg_3); memcpy(rxMessage, rx_msg_3, rxLen);}
        if (x == 4) {rxLen = sizeof(rx_msg_4); memcpy(rxMessage, rx_msg_4, rxLen);}
        x++; if (x > 4) {x = 3;}
    }
    
    if (rxLen < 0)
    {
        ERROR_MESSAGE("ComRx: Error receiving data: %s", strerror(errno));
        return -1;
    }
    
    if (arg_LogLevel >= LOG_TRACE)
    {
        char buff[(3 * sizeof(Solax_Message_t)) + (sizeof(Solax_Message_t) / 8) + 3];
        log_Bin2Hex(buff, (const uint8_t*)rxMessage, rxLen);
        TRACE_MESSAGE("ComRx:%s", buff);
    }
    
    if (rxLen == 0)
    {
        return ERR_NO_DATA;
    }
    
    if ((rxLen < 11) || (rxMessage->Header[0] != 0xAA) || (rxMessage->Header[1] != 0x55))
    {
        TRACE_MESSAGE("ComRx: Header fail");
        return ERR_INVALID_MSG;
    }
    
    msgLen = rxMessage->DataLength + 9;
    
    if ((msgLen + 2) > rxLen)
    {
        TRACE_MESSAGE("ComRx: Length fail");
        return ERR_INVALID_MSG;
    }
    
    crc = solax_CalculateCRC((const uint8_t*)rxMessage, msgLen - 1);  // calculate out crc bytes
    
    // check calc crc with received crc
    if (rxMessage->Data[rxMessage->DataLength + 0] != highByte(crc) ||
        rxMessage->Data[rxMessage->DataLength + 1] != lowByte(crc))
    {
        return ERR_CRC_ERROR;
    }

    return ERR_NONE;
}


Solax_ErrorQuery_t solax_Send_Broardcast()
{
    static Solax_Message_t txMessage;
    
    txMessage.Source[0] = 0x01;
    txMessage.Source[1] = 0x00;
    txMessage.Destination[0] = 0x00;
    txMessage.Destination[1] = 0x00;
    txMessage.ControlCode = 0x10;
    txMessage.FunctionCode = 0x00;
    txMessage.DataLength = 0x00;
    
    return solax_RS485_Send(&txMessage);
}


Solax_ErrorQuery_t solax_Send_InverterAddress()
{
    static Solax_Message_t txMessage;
    
    txMessage.Source[0] = 0x00;
    txMessage.Source[1] = 0x00;
    txMessage.Destination[0] = 0x00;
    txMessage.Destination[1] = 0x00;
    txMessage.ControlCode = 0x10;
    txMessage.FunctionCode = 0x01;
    txMessage.DataLength = 0x0F;
    memcpy(txMessage.Data, solax_InverterSerialNumber, 14);
    txMessage.Data[14] = arg_InverterAddr;
    
    return solax_RS485_Send(&txMessage);
}


Solax_ErrorQuery_t solax_Send_QueryLiveData()
{
    static Solax_Message_t txMessage;
    
    txMessage.Source[0] = 0x01;
    txMessage.Source[1] = 0x00;
    txMessage.Destination[0] = 0x00;
    txMessage.Destination[1] = arg_InverterAddr;
    txMessage.ControlCode = 0x11;
    txMessage.FunctionCode = 0x02;
    txMessage.DataLength = 0x00;
    
    return solax_RS485_Send(&txMessage);
}


Solax_ErrorQuery_t solax_SendQuery(const Solax_StateQuery_t stateQuery)
{
    Solax_ErrorQuery_t error;
    
    switch (stateQuery)
    {
        case STATE_BROARDCAST:
        {
            error = solax_Send_Broardcast();
            break;
        }
        
        case STATE_INVERTER_ADDRESS:
        {
            error = solax_Send_InverterAddress();
            break;
        }
        
        case STATE_QUERY_LIVE_DATA:
        {
            error = solax_Send_QueryLiveData();
            break;
        }
    }
    return error;
}


Solax_ErrorQuery_t solax_ReceiveQuery(const Solax_StateQuery_t stateQuery, Solax_LiveData_t* liveData)
{
    static Solax_Message_t rxMessage;
    
    uint32_t value;
    Solax_ErrorQuery_t error;
        
    error = solax_RS485_Receive(&rxMessage);
    if (error == -1) return -1;
    
    *liveData = (Solax_LiveData_t) {0};
        
    switch (stateQuery)
    {
        case STATE_BROARDCAST:
        {
            if (error == ERR_NO_DATA)
            {
                // Maybe the Solax-X1 inverter is offline (e.g. no sunlight);
                DEBUG_MESSAGE("Solax: No broadcast response");
            }
            else if (error == ERR_CRC_ERROR)
            {
                DEBUG_MESSAGE("Solax: Broadcast response CRC error");
            }
            else if ((error == ERR_INVALID_MSG) || (rxMessage.ControlCode != 0x10) || (rxMessage.FunctionCode != 0x80))
            {
                DEBUG_MESSAGE("Solax: Invalid broadcast response message");
            }
            else
            {
                // Serial number from query response
                memcpy(solax_InverterSerialNumber, rxMessage.Data, 14);
                solax_InverterSerialNumber[14] = '\0';
                DEBUG_MESSAGE("Solax: Serial number: %s", solax_InverterSerialNumber);
            }
            break;
        }
        
        case STATE_INVERTER_ADDRESS:
        {
            if (error == ERR_NO_DATA)
            {
                DEBUG_MESSAGE("Solax: No address confirmation response");
            }
            else if (error == ERR_CRC_ERROR)
            {
                DEBUG_MESSAGE("Solax: Address confirmation response CRC error");
            }
            else if ((error == ERR_INVALID_MSG) || (rxMessage.ControlCode != 0x10) || (rxMessage.FunctionCode != 0x81) || (rxMessage.Data[0] != 0x06))
            {
                DEBUG_MESSAGE("Solax: Invalid address confirmation message");
            }
            else
            {
                DEBUG_MESSAGE("Solax: Inverter Bus-Address 0x%02X confirmed", arg_InverterAddr);
            }
            break;
        }
        
        case STATE_QUERY_LIVE_DATA:
        {
            if (error == ERR_NO_DATA)
            {
                DEBUG_MESSAGE("Solax: No live data response");
            }
            else if (error == ERR_CRC_ERROR)
            {
                DEBUG_MESSAGE("Solax: Data response CRC error");
            }
            else if ((error == ERR_INVALID_MSG) || (rxMessage.ControlCode != 0x11) || (rxMessage.FunctionCode != 0x82))
            {
                DEBUG_MESSAGE("Solax: Invalid live data message");
            }
            else
            {
                value = (rxMessage.Data[0] << 8) | rxMessage.Data[1]; // Temperature [°C]
                liveData->Temperature = value;
                DEBUG_MESSAGE("Solax: LiveData.Temperature: %.0f C", liveData->Temperature);
                
                value = (rxMessage.Data[2] << 8) | rxMessage.Data[3]; // Energy Today [kWh]
                liveData->Energy_Today = value * 0.1f;
                DEBUG_MESSAGE("Solax: LiveData.Energy_Today: %.1f kWh", liveData->Energy_Today);
                
                value = (rxMessage.Data[4] << 8) | rxMessage.Data[5]; // PV1 Voltage [V]
                liveData->DC1_Voltage = value * 0.1f;
                DEBUG_MESSAGE("Solax: LiveData.DC1_Voltage: %.1f V", liveData->DC1_Voltage);
                
                value = (rxMessage.Data[6] << 8) | rxMessage.Data[7]; // PV2 Voltage [V]
                liveData->DC2_Voltage = value * 0.1f;
                DEBUG_MESSAGE("Solax: LiveData.DC2_Voltage: %.1f V", liveData->DC2_Voltage);
                
                value = (rxMessage.Data[8] << 8) | rxMessage.Data[9]; // PV1 Current [A]
                liveData->DC1_Current = value * 0.1f;
                DEBUG_MESSAGE("Solax: LiveData.DC1_Current: %.1f A", liveData->DC1_Current);
                
                value = (rxMessage.Data[10] << 8) | rxMessage.Data[11]; // PV2 Current [A]
                liveData->DC2_Current = value * 0.1f;
                DEBUG_MESSAGE("Solax: LiveData.DC2_Current: %.1f A", liveData->DC2_Current);
                
                value = (rxMessage.Data[12] << 8) | rxMessage.Data[13]; // AC Current [A]
                liveData->AC_Current = value * 0.1f;
                DEBUG_MESSAGE("Solax: LiveData.AC_Current: %.1f A", liveData->AC_Current);
                
                value = (rxMessage.Data[14] << 8) | rxMessage.Data[15]; // AC Voltage [V]
                liveData->AC_Voltage = value * 0.1f;
                DEBUG_MESSAGE("Solax: LiveData.AC_Voltage: %.1f V", liveData->AC_Voltage);
                
                value = (rxMessage.Data[16] << 8) | rxMessage.Data[17]; // AC Frequency [Hz]
                liveData->Frequency = value * 0.01f;
                DEBUG_MESSAGE("Solax: LiveData.Frequency: %.2f Hz", liveData->Frequency);
                
                value = (rxMessage.Data[18] << 8) | rxMessage.Data[19]; // AC Power [W]
                liveData->Power = value;
                DEBUG_MESSAGE("Solax: LiveData.Power: %.0f W", liveData->Power);
                
                //value = (rxMessage.Data[20] << 8) | rxMessage.Data[21]; // Not Used
                
                value = (rxMessage.Data[22] << 24) | (rxMessage.Data[23] << 16) | (rxMessage.Data[24] << 8) | rxMessage.Data[25]; // Energy Total [kWh]
                if (value) {liveData->Energy_Total = value * 0.1f;}
                DEBUG_MESSAGE("Solax: LiveData.Energy_Total: %.1f kWh", liveData->Energy_Total);
                
                value = (rxMessage.Data[26] << 24) | (rxMessage.Data[27] << 16) | (rxMessage.Data[28] << 8) | rxMessage.Data[29]; // Work Time Total [hour]
                if (value) {liveData->Runtime_Total = value;}
                DEBUG_MESSAGE("Solax: LiveData.Runtime_Total: %.0f h", liveData->Runtime_Total);
                
                value = (rxMessage.Data[30] << 8) | rxMessage.Data[31]; // Work mode [???]
                liveData->Status = (uint8_t)value;
                DEBUG_MESSAGE("Solax: LiveData.Status: %d", liveData->Status);
                
                //value = (rxMessage.Data[32] << 8) | rxMessage.Data[33]; // Grid voltage fault in 0.1V
                //value = (rxMessage.Data[34] << 8) | rxMessage.Data[35]; // Gird frequency fault in 0.01Hz
                //value = (rxMessage.Data[36] << 8) | rxMessage.Data[37]; // DC injection fault in 1mA
                //value = (rxMessage.Data[38] << 8) | rxMessage.Data[39]; // Temperature fault in °C
                //value = (rxMessage.Data[40] << 8) | rxMessage.Data[41]; // Pv1 voltage fault in 0.1V
                //value = (rxMessage.Data[42] << 8) | rxMessage.Data[43]; // Pv2 voltage fault in 0.1V
                //value = (rxMessage.Data[44] << 8) | rxMessage.Data[45]; // GFC fault
                
                value = (rxMessage.Data[49] << 24) | (rxMessage.Data[48] << 16) | (rxMessage.Data[47] << 8) | rxMessage.Data[46]; // Error Code
                liveData->ErrorBits = (uint32_t)value;
                DEBUG_MESSAGE("Solax: LiveData.ErrorBits: 0x%08X", liveData->ErrorBits);
                
                liveData->valid = true;
            }
            break;
        }
    }
    return error;
}


/* --- State Machine Communication with Solax-X1_Mini --- */
int solax_QueryHandle(Solax_LiveData_t* liveData)
{
    static Solax_StateQuery_t stateQuery = STATE_QUERY_LIVE_DATA;
    static int timeoutInverterOnline = TIMEOUT_INVERTER_ONLINE;
    static int countError = 0;
    
    Solax_ErrorQuery_t errorRx;
    Solax_ErrorQuery_t errorTx;
        
    errorRx = solax_ReceiveQuery(stateQuery, liveData);
    if (errorRx == -1) return -1;
    
    switch (stateQuery)
    {
        case STATE_BROARDCAST:
        {
            if (errorRx)
            {
                countError++;
                if (countError >= 10)
                {
                    countError = 0;
                    stateQuery = STATE_QUERY_LIVE_DATA;
                }
            }
            else
            {
                countError = 0;
                stateQuery = STATE_INVERTER_ADDRESS;
            }
            break;
        }
        
        case STATE_INVERTER_ADDRESS:
        {
            if (errorRx)
            {
                countError++;
                if (countError >= 3)
                {
                    countError = 0;
                    stateQuery = STATE_BROARDCAST;
                }
            }
            else
            {
                countError = 0;
                stateQuery = STATE_QUERY_LIVE_DATA;
            }
            break;
        }
        
        case STATE_QUERY_LIVE_DATA:
        {
            if (errorRx)
            {
                countError++;
                if (countError >= 3)
                {
                    countError = 0;
                    stateQuery = STATE_BROARDCAST;
                }
            }
            else
            {
                timeoutInverterOnline = 0;
            }
            break;
        }
    }
    
    if (solax_InverterOnline == true)
    {
        timeoutInverterOnline++;
        if (timeoutInverterOnline >= TIMEOUT_INVERTER_ONLINE)
        {
            solax_InverterOnline = false;
            NOTICE_MESSAGE("Solax: Inverter offline");
        }
    }
    else  // (solax_InverterOnline == false)
    {
        if (timeoutInverterOnline == 0)
        {
            solax_InverterOnline = true;
            NOTICE_MESSAGE("Solax: Live data received");
        }
    }

    
    errorTx = solax_SendQuery(stateQuery);
    if (errorTx == -1) return -1;
    
    return errorRx;
}



void solax_LiveData_Average(const Solax_LiveData_t* liveData, uint8_t idx, uint8_t max_idx)
{
    uint8_t i, cnt = 0, qos = 0;

    solax_LiveData = (Solax_LiveData_t) {0};

    for (i = 0; i <= arg_AV_Samples; i++)
    {
        if (liveData[idx].valid != true) continue;
        solax_LiveData.Temperature  += liveData[idx].Temperature;
        solax_LiveData.DC1_Voltage  += liveData[idx].DC1_Voltage;
        solax_LiveData.DC2_Voltage  += liveData[idx].DC2_Voltage;
        solax_LiveData.DC1_Current  += liveData[idx].DC1_Current;
        solax_LiveData.DC2_Current  += liveData[idx].DC2_Current;
        solax_LiveData.AC_Current   += liveData[idx].AC_Current;
        solax_LiveData.AC_Voltage   += liveData[idx].AC_Voltage;
        solax_LiveData.Frequency    += liveData[idx].Frequency;
        solax_LiveData.Power        += liveData[idx].Power;
        solax_LiveData.ErrorBits    |= liveData[idx].ErrorBits;
        if (solax_LiveData.Energy_Today  < liveData[idx].Energy_Today ) solax_LiveData.Energy_Today  = liveData[idx].Energy_Today;
        if (solax_LiveData.Energy_Total  < liveData[idx].Energy_Total ) solax_LiveData.Energy_Total  = liveData[idx].Energy_Total;
        if (solax_LiveData.Runtime_Total < liveData[idx].Runtime_Total) solax_LiveData.Runtime_Total = liveData[idx].Runtime_Total;
        if (solax_LiveData.Status        < liveData[idx].Status       ) solax_LiveData.Status        = liveData[idx].Status;
        
        if (idx)
        {
            idx--;
        }
        else
        {
            idx = max_idx;
        }
        cnt++;
    }
    
    if (cnt)
    {
        solax_LiveData.Temperature /= cnt;
        solax_LiveData.DC1_Voltage /= cnt;
        solax_LiveData.DC2_Voltage /= cnt;
        solax_LiveData.DC1_Current /= cnt;
        solax_LiveData.DC2_Current /= cnt;
        solax_LiveData.AC_Current  /= cnt;
        solax_LiveData.AC_Voltage  /= cnt;
        solax_LiveData.Frequency   /= cnt;
        solax_LiveData.Power       /= cnt;
    }
    

    for (idx = 0; idx <= max_idx; idx++)
    {
        qos += liveData[idx].valid;
    }
    
    solax_QualityOfService = (float)qos / (max_idx + 1);
    
    /*
    INFO_MESSAGE("TEST: solax_LiveData.QualityOfService: %.2f", solax_QualityOfService);
    INFO_MESSAGE("TEST: solax_LiveData.Temperature: %.0f C",    solax_LiveData.Temperature);
    INFO_MESSAGE("TEST: solax_LiveData.DC1_Voltage: %.1f V",    solax_LiveData.DC1_Voltage);
    INFO_MESSAGE("TEST: solax_LiveData.DC2_Voltage: %.1f V",    solax_LiveData.DC2_Voltage);
    INFO_MESSAGE("TEST: solax_LiveData.DC1_Current: %.1f A",    solax_LiveData.DC1_Current);
    INFO_MESSAGE("TEST: solax_LiveData.DC2_Current: %.1f A",    solax_LiveData.DC2_Current);
    INFO_MESSAGE("TEST: solax_LiveData.AC_Current: %.1f A",     solax_LiveData.AC_Current);
    INFO_MESSAGE("TEST: solax_LiveData.AC_Voltage: %.1f V",     solax_LiveData.AC_Voltage);
    INFO_MESSAGE("TEST: solax_LiveData.Frequency: %.2f Hz",     solax_LiveData.Frequency);
    INFO_MESSAGE("TEST: solax_LiveData.Power: %.0f W",          solax_LiveData.Power);
    INFO_MESSAGE("TEST: solax_LiveData.Energy_Today: %.1f kWh", solax_LiveData.Energy_Today);
    INFO_MESSAGE("TEST: solax_LiveData.Energy_Total: %.1f kWh", solax_LiveData.Energy_Total);
    INFO_MESSAGE("TEST: solax_LiveData.Runtime_Total: %.0f h",  solax_LiveData.Runtime_Total);
    INFO_MESSAGE("TEST: solax_LiveData.Status: %d",             solax_LiveData.Status);
    INFO_MESSAGE("TEST: solax_LiveData.ErrorBits: 0x%08X",      solax_LiveData.ErrorBits);
    INFO_MESSAGE("TEST: Counter: %d", cnt);
    */
}


int solax_JsonPath(char buffer[], Solax_LiveData_t* liveData)
{
    static const char* solax_ErrorText[32] =
    {
        "Tz Protection Fault",           // Byte 0.0
        "Mains Lost Fault",              // Byte 0.1
        "Grid Voltage Fault",            // Byte 0.2
        "Grid Frequency Fault",          // Byte 0.3
        "PLL Lost Fault",                // Byte 0.4
        "Bus Voltage Fault",             // Byte 0.5
        "Error Bit 06",                  // Byte 0.6
        "Oscillator Fault",              // Byte 0.7
        
        "DCI OCP Fault",                 // Byte 1.0
        "Residual Current Fault",        // Byte 1.1
        "PV Voltage Fault",              // Byte 1.2
        "Ac10Mins Voltage Fault",        // Byte 1.3
        "Isolation Fault",               // Byte 1.4
        "Over Temperature Fault",        // Byte 1.5
        "Ventilator Fault",              // Byte 1.6
        "Error Bit 15",                  // Byte 1.7
        
        "SPI Communication Fault",       // Byte 2.0
        "SCI Communication Fault",       // Byte 2.1
        "Error Bit 18",                  // Byte 2.2
        "Input Configuration Fault",     // Byte 2.3
        "EEPROM Fault",                  // Byte 2.4
        "Relay Fault",                   // Byte 2.5
        "Sample Consistence Fault",      // Byte 2.6
        "Residual-Current Device Fault", // Byte 2.7
        
        "Error Bit 24",                  // Byte 3.0
        "Error Bit 25",                  // Byte 3.1
        "Error Bit 26",                  // Byte 3.2
        "Error Bit 27",                  // Byte 3.3
        "Error Bit 28",                  // Byte 3.4
        "DCI Device Fault",              // Byte 3.5
        "Other Device Fault",            // Byte 3.6
        "Error Bit 31",                  // Byte 3.7
    };
    
    INFO_MESSAGE("JsonP: Inverter.Address: %d",            arg_InverterAddr);
    INFO_MESSAGE("JsonP: Inverter.Online: %d",             solax_InverterOnline);
    INFO_MESSAGE("JsonP: Inverter.QualityOfService: %.2f", solax_QualityOfService);
    INFO_MESSAGE("JsonP: LiveData.DC1_Voltage: %.1f V",    liveData->DC1_Voltage);
    INFO_MESSAGE("JsonP: LiveData.DC2_Voltage: %.1f V",    liveData->DC2_Voltage);
    INFO_MESSAGE("JsonP: LiveData.DC1_Current: %.1f A",    liveData->DC1_Current);
    INFO_MESSAGE("JsonP: LiveData.DC2_Current: %.1f A",    liveData->DC2_Current);
    INFO_MESSAGE("JsonP: LiveData.AC_Current: %.1f A",     liveData->AC_Current);
    INFO_MESSAGE("JsonP: LiveData.AC_Voltage: %.1f V",     liveData->AC_Voltage);
    INFO_MESSAGE("JsonP: LiveData.Frequency: %.2f Hz",     liveData->Frequency);
    INFO_MESSAGE("JsonP: LiveData.Power: %.0f W",          liveData->Power);
    INFO_MESSAGE("JsonP: LiveData.Energy_Today: %.1f kWh", liveData->Energy_Today);
    INFO_MESSAGE("JsonP: LiveData.Energy_Total: %.1f kWh", liveData->Energy_Total);
    INFO_MESSAGE("JsonP: LiveData.Runtime_Total: %.0f h",  liveData->Runtime_Total);
    INFO_MESSAGE("JsonP: LiveData.Status: %d",             liveData->Status);
    INFO_MESSAGE("JsonP: LiveData.ErrorBits: 0x%08X",      liveData->ErrorBits);
    
    int len = 0;
    len += sprintf(&buffer[len], "{\r\n");                                                          //
    len += sprintf(&buffer[len], "  \"inverter\":\r\n");                                            //
    len += sprintf(&buffer[len], "  {\r\n");                                                        //
    len += sprintf(&buffer[len], "    \"address\": %d,\r\n",              arg_InverterAddr);        //
    len += sprintf(&buffer[len], "    \"online\": %d,\r\n",               solax_InverterOnline);    //
    len += sprintf(&buffer[len], "    \"quality_of_service\": %.2f,\r\n", solax_QualityOfService);  //
    len += sprintf(&buffer[len], "    \"live_data\":\r\n");                                         //
    len += sprintf(&buffer[len], "    {\r\n");                                                      //
    len += sprintf(&buffer[len], "      \"temperature\": %.0f,\r\n",      liveData->Temperature);   //
    len += sprintf(&buffer[len], "      \"dc1_voltage\": %.1f,\r\n",      liveData->DC1_Voltage);   //
    len += sprintf(&buffer[len], "      \"dc1_current\": %.1f,\r\n",      liveData->DC1_Current);   //
    len += sprintf(&buffer[len], "      \"dc2_voltage\": %.1f,\r\n",      liveData->DC2_Voltage);   //
    len += sprintf(&buffer[len], "      \"dc2_current\": %.1f,\r\n",      liveData->DC2_Current);   //
    len += sprintf(&buffer[len], "      \"ac_voltage\": %.1f,\r\n",       liveData->AC_Voltage);    //
    len += sprintf(&buffer[len], "      \"ac_current\": %.1f,\r\n",       liveData->AC_Current);    //
    len += sprintf(&buffer[len], "      \"frequency\": %.2f,\r\n",        liveData->Frequency);     //
    len += sprintf(&buffer[len], "      \"power\": %.0f,\r\n",            liveData->Power);         //
    len += sprintf(&buffer[len], "      \"energy_today\": %.1f,\r\n",     liveData->Energy_Today);  //
    len += sprintf(&buffer[len], "      \"energy_total\": %.1f,\r\n",     liveData->Energy_Total);  //
    len += sprintf(&buffer[len], "      \"runtime_total\": %.0f,\r\n",    liveData->Runtime_Total); //
    len += sprintf(&buffer[len], "      \"status\": %d,\r\n",             liveData->Status);        //
    len += sprintf(&buffer[len], "      \"error_bits\": %d\r\n",          liveData->ErrorBits);     //
    len += sprintf(&buffer[len], "    }\r\n");                                                      //
    len += sprintf(&buffer[len], "  }\r\n");                                                        //
    len += sprintf(&buffer[len], "}\r\n");                                                          //
    
    return len;
}



int poll_HTTP_Server(void)
{
    int fd_sock_client;
    struct sockaddr_in addr_client;
    char response[1000];
    int len = 0;

    fd_sock_client = accept(fd_sock_server, NULL, NULL);
 
    if ((fd_sock_client == -1) && (errno != EWOULDBLOCK))
    {
        ERROR_MESSAGE("Http: Error when accepting HTTP-Client connection: %s", strerror(errno));
        return -1;
    }
    
    if (fd_sock_client >= 0)  
    {
        DEBUG_MESSAGE("HTTP: Got a connection");
                
        /*
        len += sprintf(&response[len], "HTTP/1.0 200 OK\r\n");
        len += sprintf(&response[len], "Connection: close\r\n");
        len += sprintf(&response[len], "Content-Type: text/html; charset=utf-8\r\n");
        len += sprintf(&response[len], "\r\n");
        len += sprintf(&response[len], "<html><head><title>SolaXd</title></head>\r\n");
        len += sprintf(&response[len], "<body>\r\n");
        len += solax_JsonPath(&response[len], &solax_LiveData);
        len += sprintf(&response[len], "</body></html>\r\n");
        */
        
        len += sprintf(&response[len], "HTTP/1.0 200 OK\r\n");
        len += sprintf(&response[len], "Connection: close\r\n");
        len += sprintf(&response[len], "Content-Type: application/json\r\n");
        len += sprintf(&response[len], "\r\n");
        len += solax_JsonPath(&response[len], &solax_LiveData);

        write(fd_sock_client, response, len);
        close(fd_sock_client);
    }
    return 0;
}


int init_HTTP_Server(int port)
{
    int error;
    int flags;
    int enable = 1;
    struct sockaddr_in addr_server;

    fd_sock_server = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_sock_server == -1) { ERROR_MESSAGE("Init: Error opening socket for HTTP-Server at port '%d': %s", port, strerror(errno)); return -1; }
    
    flags = fcntl(fd_sock_server, F_GETFL);
    flags = fcntl(fd_sock_server, F_SETFL, flags | O_NONBLOCK);
    
    setsockopt(fd_sock_server, SOL_SOCKET, SO_REUSEADDR, &enable , sizeof(int));
    
    // filling server information
    addr_server.sin_family = AF_INET;
    addr_server.sin_addr.s_addr = INADDR_ANY;
    addr_server.sin_port = htons(port);

    error = bind(fd_sock_server, (struct sockaddr *) &addr_server, sizeof(addr_server));
    if (error == -1) { ERROR_MESSAGE("Init: Error binding socket for HTTP-Server at port '%d': %s", port, strerror(errno)); return -1; }

    error = listen(fd_sock_server, 10);
    if (error == -1) { ERROR_MESSAGE("Init: Error listening socket for HTTP-Server at port '%d': %s", port, strerror(errno)); return -1; }
    
    NOTICE_MESSAGE("Init: HTTP-Server at port '%d' created successfully", port);
    return 0;
}


int init_Serial_Interface(char device[])
{
    struct termios tty;
    
    fd_tty = open(device, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd_tty < 0)
    {
        ERROR_MESSAGE("Init: Error opening '%s': %s", device, strerror(errno));
        return -1;
    }
    
    if (tcgetattr(fd_tty, &tty) < 0)
    {
        ERROR_MESSAGE("Init: Error opening '%s': %s", device, strerror(errno));
        return -1;
    }
    
    // raw mode
    tty.c_cflag = CLOCAL | CREAD;  // ignore modem controls
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    
    // baudrate 9600, 8 bits, no parity, 1 stop bit
    cfsetospeed(&tty, (speed_t)B9600);
    cfsetispeed(&tty, (speed_t)B9600);    
    tty.c_cflag |= CS8;             // 8-bit characters
    tty.c_cflag &= ~PARENB;         // no parity bit
    tty.c_cflag &= ~CSTOPB;         // 1 stop bit
    tty.c_cflag &= ~CRTSCTS;        // no hardware flowcontrol
    
    /* completely non-blocking read */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    
    if (tcsetattr(fd_tty, TCSANOW, &tty) != 0)
    {
        ERROR_MESSAGE("Init: Error opening '%s': %s", device, strerror(errno));
        return -1;
    }
    
    NOTICE_MESSAGE("Init: Device '%s' opened successfully", device);
    return 0;
}



/*** MAIN ******************************************************************************************/

int main(int argc, char* argv[])
{
    int opt;
    int error;
    uint8_t index = 0;
    Solax_LiveData_t liveData[MAX_INDEX_OF_LIVE_DATA + 1] = {0};

    if ((argc == 2) && (strcmp(argv[1], "--version") == 0))
    {
        printf("%s, %s\n", SOLARXD_STRING, VERSION_STRING);
        printf("Copyright (C) 2021 Jens Jordan\n");
        printf("License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\n");
        printf("There is NO WARRANTY, to the extent permitted by law.\n");
        printf("This is free software; you are free to change and redistribute it.\n");
        return 0;
    }
    
    if ((argc == 2) && (strcmp(argv[1], "--help") == 0))
    {
        printf("Usage: %s [OPTION] ...\n", SOLARXD_STRING);
        printf("Daemon for communication with SolaX-X1_Mini inverter via RS485.\n");
        printf("    Options:      The default value for each option is shown in square brackets.\n");
        printf("      -d <DEV>    Use DEV as SolaXd serial port device  [%s]\n", DEFAULT_TTY_DEVICE_NAME);
        printf("      -p <PORT>   Port of HTTP-Server  [%d]\n", DEFAULT_TCP_PORT);
        printf("      -s <SAMPLE> Samples used for average calculation  [%d]\n", DEFAULT_AVERAGE_SAMPLES);
        printf("      -a <ADDR>   Use ADDR as inverter bus address  [%d]\n", DEFAULT_INVERTER_ADDRESS);
        printf("      -l <FILE>   Write log to FILE, instead to stderr\n");
        printf("      -L <LEVEL>  LEVEL: 0=error/1=notice/2=info/3=debug/4=trace  [%d]\n", DEFAULT_LOG_LEVEL);
        printf("      -x          Enable test mode with simulated inverter data\n");
        printf("      --help      Display this help and exit\n");
        printf("      --version   Output version information and exit\n");
        return 0;
    }
    
    while ((opt = getopt (argc, argv, ":d:p:s:a:l:L:x")) != -1)
    {
        switch (opt)
        {
            case 'd':
                arg_TTY_Device = optarg;
                break;
            case 'p':
                arg_TCP_Port = atoi(optarg);
                break;
            case 's':
                arg_AV_Samples = atoi(optarg);
                break;
            case 'a':
                arg_InverterAddr = atoi(optarg);
                break;
            case 'l':
                arg_LogFile = optarg;
                break;
            case 'L':
                arg_LogLevel = atoi(optarg);
                break;
            case 'x':
                arg_TestMode = 1;
                break;
            case ':':
                fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                return -1;
            case '?':
                if (isprint (optopt))
                    fprintf (stderr, "Unknown option '-%c'.\n", optopt);
                else
                    fprintf (stderr, "Unknown option character '0x%x'.\n", optopt);
                return -1;
            default:
                abort();
        }
    }
    
    if (arg_LogFile == NULL)
    {
        fp_log_file = stderr;
    }
    else
    {
        fp_log_file = fopen(arg_LogFile, "a");    // open Log-File
        if (fp_log_file == NULL) { printf("Main: Error opening Log-File '%s': %s\n", arg_LogFile, strerror(errno)); return errno; }
    }
    
    NOTICE_MESSAGE("Main: %s started", SOLARXD_STRING);
    
    INFO_MESSAGE("Main: TTY_Device   : %s", arg_TTY_Device  );
    INFO_MESSAGE("Main: TCP_Port     : %d", arg_TCP_Port    );
    INFO_MESSAGE("Main: AV_Samples   : %d", arg_AV_Samples  );
    INFO_MESSAGE("Main: InverterAddr : %d", arg_InverterAddr);
    INFO_MESSAGE("Main: LogFile      : %s", arg_LogFile     );
    INFO_MESSAGE("Main: LogLevel     : %d", arg_LogLevel    );
    INFO_MESSAGE("Main: TestMode     : %d", arg_TestMode    );
    
    error = init_Serial_Interface(arg_TTY_Device);  // open COM-Port
    if (error == -1) return errno;
    
    error = init_HTTP_Server(arg_TCP_Port);         // open TCP-Listener
    if (error == -1) return errno;
    
    while (1)
    {
        error = solax_QueryHandle(&liveData[index]);
        if (error == -1) return errno;
        
        solax_LiveData_Average(liveData, index, MAX_INDEX_OF_LIVE_DATA);
        
        index++;
        if (index > MAX_INDEX_OF_LIVE_DATA) index = 0;
        
        error = poll_HTTP_Server();
        if (error == -1) return errno;
        
        sleep(1);
    }
    return errno;
}
