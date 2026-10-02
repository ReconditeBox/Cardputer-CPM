#ifndef CARDPUTER_ADV_KEYBOARD_H
#define CARDPUTER_ADV_KEYBOARD_H

#include <M5Cardputer.h>
#include <algorithm>
#include <memory>

#include "utility/Keyboard/KeyboardReader/KeyboardReader.h"
#include "utility/Adafruit_TCA8418/Adafruit_TCA8418.h"
#include "utility/Adafruit_TCA8418/Adafruit_TCA8418_registers.h"

/*
 * Cardputer ADV keyboard reader.
 *
 * M5Stack's current ADV reader consumes only one TCA8418 FIFO event
 * per update() call and relies on an interrupt flag. Under sustained
 * multi-key use this can allow the 10-event FIFO to fall behind or
 * overflow, leaving the software key list stale.
 *
 * This reader polls the chip directly, drains every queued event on
 * each poll, and recovers from FIFO overflow by clearing the software
 * key list and hardware FIFO.
 */
class CardputerAdvPollingKeyboardReader : public KeyboardReader
{
public:
    CardputerAdvPollingKeyboardReader()
        : _ready(false),
          _lastPollMicros(0)
    {
    }

    void begin() override
    {
        _tca8418 =
            std::make_unique<Adafruit_TCA8418>();

        if (!_tca8418->begin())
        {
            _ready = false;
            return;
        }

        if (!_tca8418->matrix(7, 8))
        {
            _ready = false;
            return;
        }

        /*
         * We do not use the TCA8418 interrupt pin in this reader.
         * Polling the event counter avoids dependence on a missed or
         * stale ISR flag.
         */
        _tca8418->disableInterrupts();

        _tca8418->flush();

        /*
         * Clear all relevant interrupt status bits, including overflow.
         */
        _tca8418->writeRegister8(
            TCA8418_REG_INT_STAT,
            0x0F
        );

        _key_list.clear();

        _lastPollMicros = micros();
        _ready = true;
    }

    void update() override
    {
        if (!_ready)
        {
            return;
        }

        /*
         * 1 kHz polling is far faster than human key activity while
         * avoiding an I2C transaction on every RunCPM console poll.
         */
        uint32_t now = micros();

        if (
            (uint32_t)(
                now - _lastPollMicros
            ) < 1000
        )
        {
            return;
        }

        _lastPollMicros = now;

        uint8_t intStat =
            _tca8418->readRegister8(
                TCA8418_REG_INT_STAT
            );

        if (
            intStat &
            TCA8418_REG_STAT_OVR_FLOW_INT
        )
        {
            recoverFromOverflow();
            return;
        }

        /*
         * Drain the complete hardware FIFO, not just one event.
         * The TCA8418 can hold at most 10 events.
         */
        uint8_t available =
            _tca8418->available();

        while (available > 0)
        {
            uint8_t raw =
                _tca8418->getEvent();

            if (raw == 0)
            {
                break;
            }

            processEvent(raw);

            available--;
        }

        /*
         * Clear the key-event interrupt status after draining.
         * This is harmless even though the physical INT pin is unused.
         */
        _tca8418->writeRegister8(
            TCA8418_REG_INT_STAT,
            0x01
        );

        /*
         * An overflow could have happened while the FIFO was being
         * drained. Check once more before returning.
         */
        intStat =
            _tca8418->readRegister8(
                TCA8418_REG_INT_STAT
            );

        if (
            intStat &
            TCA8418_REG_STAT_OVR_FLOW_INT
        )
        {
            recoverFromOverflow();
        }
    }

private:
    std::unique_ptr<Adafruit_TCA8418> _tca8418;
    bool _ready;
    uint32_t _lastPollMicros;

    void recoverFromOverflow()
    {
        /*
         * Once FIFO events have been lost, the software key list can no
         * longer be trusted. Clear both sides so the next fresh key press
         * starts from a known state.
         */
        _key_list.clear();

        _tca8418->flush();

        _tca8418->writeRegister8(
            TCA8418_REG_INT_STAT,
            0x0F
        );
    }

    void processEvent(uint8_t raw)
    {
        /*
         * TCA8418 KEY_EVENT bit 7:
         *   1 = press
         *   0 = release
         */
        bool pressed =
            (raw & 0x80) != 0;

        uint8_t keyNumber =
            raw & 0x7F;

        if (keyNumber == 0)
        {
            return;
        }

        uint8_t zeroBased =
            keyNumber - 1;

        uint8_t matrixRow =
            zeroBased / 10;

        uint8_t matrixCol =
            zeroBased % 10;

        /*
         * Same physical remap used by M5Stack's Cardputer ADV reader.
         */
        Point2D_t point;

        point.x =
            matrixRow * 2;

        if (matrixCol > 3)
        {
            point.x++;
        }

        point.y =
            (matrixCol + 4) % 4;

        if (
            point.x < 0 ||
            point.x >= 14 ||
            point.y < 0 ||
            point.y >= 4
        )
        {
            return;
        }

        if (pressed)
        {
            auto it =
                std::find(
                    _key_list.begin(),
                    _key_list.end(),
                    point
                );

            if (it == _key_list.end())
            {
                _key_list.push_back(point);
            }
        }
        else
        {
            auto it =
                std::find(
                    _key_list.begin(),
                    _key_list.end(),
                    point
                );

            if (it != _key_list.end())
            {
                _key_list.erase(it);
            }
        }
    }
};

#endif
