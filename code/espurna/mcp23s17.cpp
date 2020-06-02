/*

MCP23S17 MODULE

Copyright (C) 2020 by Eddi De Pieri <eddi at depieri dot com>
Copyright (C) 2016-2017 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2016 Plamen Kovandjiev <p.kovandiev@kmpelectronics.eu> & Dimitar Antonov <d.antonov@kmpelectronics.eu>

*/

#include "mcp23s17.h"

#if MCP23S17_SUPPORT

#define MCP23S17_CS 15

#define READ_CMD  0x41
#define WRITE_CMD 0x40

#define IODIR   0x00
#define IPOL    0x01
#define GPINTEN 0x02
#define DEFVAL  0x03
#define INTCON  0x04
#define IOCON   0x05
#define GPPU    0x06
#define INTF    0x07
#define INTCAP  0x08
#define GPIO    0x09
#define OLAT    0x0A

/**
 * @brief Relay pins.
 */
const uint8_t RELAY_PINS[DUMMY_RELAY_COUNT] =
{ MCP23S17_REL1PIN, MCP23S17_REL2PIN, MCP23S17_REL3PIN, MCP23S17_REL4PIN };

/**
 * @brief Input pins.
 */
const int OPTOIN_PINS[MCP23S17_OPTOIN_COUNT] =
{ MCP23S17_IN1PIN, MCP23S17_IN2PIN, MCP23S17_IN3PIN, MCP23S17_IN4PIN };

uint8_t  _expTxData[16]  __attribute__((aligned(4)));
uint8_t  _expRxData[16]  __attribute__((aligned(4))); 

void MCP23S17Setup()
{
    DEBUG_MSG_P(PSTR("[MCP23S17] Initialize SPI bus\n"));
    // Expander settings.
    SPI.begin();
    SPI.setHwCs(MCP23S17_CS);
    SPI.setFrequency(1000000);
    SPI.setDataMode(SPI_MODE0);

    pinMode(MCP23S17_CS, OUTPUT);
    digitalWrite(MCP23S17_CS, HIGH);
    MCP23S17InitGPIO();
}

/**
 * @brief Set a expander MCP23S17 the pin direction.
 *
 * @param pinNumber Pin number for set.
 * @param mode direction mode. 0 - INPUT, 1 - OUTPUT.
 *
 * @return void
 */
void MCP23S17InitGPIO()
{
    // Relays.
    for (uint8_t i = 0; i < DUMMY_RELAY_COUNT; i++)
    {
        DEBUG_MSG_P(PSTR("[MCP23S17] Initialize GPIO %d\n"), i);
        MCP23S17SetDirection(RELAY_PINS[i], OUTPUT);
    }

    // Opto inputs.
    for (uint8_t i = 0; i < MCP23S17_OPTOIN_COUNT; i++)
    {
        MCP23S17SetDirection(OPTOIN_PINS[i], INPUT);
    }
}

/**
 * @brief Set a expander MCP23S17 the pin direction.
 *
 * @param pinNumber Pin number for set.
 * @param mode direction mode. 0 - INPUT, 1 - OUTPUT.
 *
 * @return void
 */
void MCP23S17SetDirection(uint8_t pinNumber, uint8_t mode)
{
    uint8_t registerData = MCP23S17ReadRegister(IODIR);

    if (INPUT == mode)
    {
        registerData |= (1 << pinNumber);
    }
    else
    {
        registerData &= ~(1 << pinNumber);
    }

    MCP23S17WriteRegister(IODIR, registerData);
}
 
/**
 * @brief Read an expander MCP23S17 a register.
 *
 * @param address A register address.
 *
 * @return The data from the register.
 */
uint8_t MCP23S17ReadRegister(uint8_t address)
{
    _expTxData[0] = READ_CMD;
    _expTxData[1] = address;

    digitalWrite(MCP23S17_CS, LOW);
    SPI.transferBytes(_expTxData, _expRxData, 3);
    digitalWrite(MCP23S17_CS, HIGH);

    return _expRxData[2];
} 

/**
 * @brief Write data in expander MCP23S17 register.
 *
 * @param address A register address.
 * @param data A byte for write.
 *
 * @return void.
 */
void MCP23S17WriteRegister(uint8_t address, uint8_t data)
{
    _expTxData[0] = WRITE_CMD;
    _expTxData[1] = address;
    _expTxData[2] = data;

    digitalWrite(MCP23S17_CS, LOW);
    SPI.transferBytes(_expTxData, _expRxData, 3);
    digitalWrite(MCP23S17_CS, HIGH);
} 

/**
 * @brief Set expander MCP23S17 pin state.
 *
 * @param pinNumber The number of pin to be set.
 * @param state The pin state, true - 1, false - 0.
 *
 * @return void
 */
void MCP23S17SetPin(uint8_t pinNumber, bool state)
{
    uint8_t registerData = MCP23S17ReadRegister(OLAT);

    if (state)
    {
        registerData |= (1 << pinNumber);
    }
    else
    {
        registerData &= ~(1 << pinNumber);
    }

    MCP23S17WriteRegister(OLAT, registerData);
}

/**
 * @brief Get MCP23S17 MCP23S17 pin state.
 *
 * @param pinNumber The number of pin to be get.
 *
 * @return State true - 1, false - 0.
 */
bool MCP23S17GetPin(uint8_t pinNumber)
{
    uint8_t registerData = MCP23S17ReadRegister(GPIO);

    return registerData & (1 << pinNumber);
}
 
/**
 * @brief Set relay new state.
 *
 * @param relayNumber Number of relay from 0 to RELAY_COUNT - 1. 0 - Relay1, 1 - Relay2 ...
 * @param state New state of relay, true - On, false = Off.
 *
 * @return void
 */
void MCP23S17SetRelayState(uint8_t relayNumber, bool state)
{
    // Check if relayNumber is out of range - return.
    if (relayNumber > DUMMY_RELAY_COUNT - 1)
    {
        return;
    }
    
    MCP23S17SetPin(RELAY_PINS[relayNumber], state);
}

/**
 * @brief Get opto in state.
 *
 * @param optoInNumber OptoIn number from 0 to MCP23S17_OPTOIN_COUNT - 1
 *
 * @return bool true - opto in is On, false is Off. If number is out of range - return false.
 */
bool MCP23S17GetOptoInState(uint8_t optoInNumber)
{
    // Check if optoInNumber is out of range - return false.
    if (optoInNumber > MCP23S17_OPTOIN_COUNT - 1)
    {
        return false;
    }

    return !MCP23S17GetPin(OPTOIN_PINS[optoInNumber]);
} 

#endif // MCP23S17_SUPPORT
