// Wire protocol between the host (libpinedio-wsbridge) and the Waveshare USB-TO-LoRa
// bridge firmware. The firmware is a dumb SPI/GPIO bridge to the SX1262; RadioLib on the
// host does all radio work, exactly as it does through a CH341 on a MeshStick.
//
// Every frame, both directions:  SYNC  CMD  LEN_LO  LEN_HI  PAYLOAD[LEN]
// Host requests carry CMD < 0x80. A reply carries CMD | 0x80. EVT_IRQ is unsolicited
// and is only ever sent between replies, never inside one.
#ifndef WSBRIDGE_PROTOCOL_H
#define WSBRIDGE_PROTOCOL_H

#define WSB_SYNC 0xA5
#define WSB_BAUD 2000000
#define WSB_MAX_PAYLOAD 512
#define WSB_VERSION 1

// Requests
#define WSB_CMD_HELLO 0x01    // -> "WSBR", version, tcxo flag
#define WSB_CMD_XFER 0x02     // payload: SPI bytes; -> bytes clocked in. CS untouched
#define WSB_CMD_XFER_CS 0x03  // as XFER, with CS asserted around the transfer
#define WSB_CMD_WRITE 0x04    // payload: pin, level; no reply
#define WSB_CMD_READ 0x05     // payload: pin; -> level
#define WSB_CMD_MODE 0x06     // payload: pin, 1=output 0=input; no reply

#define WSB_REPLY(cmd) ((cmd) | 0x80)
#define WSB_EVT_IRQ 0xE1      // DIO1 rose

// Pin numbers, chosen to match Meshtastic's MeshStick (CH341) yaml
#define WSB_PIN_CS 0    // PB12
#define WSB_PIN_RXEN 1  // PB4, RF switch: high = RX path, low = TX path
#define WSB_PIN_RESET 2 // PA4, SX1262 NRESET
#define WSB_PIN_BUSY 4  // PB1
#define WSB_PIN_IRQ 6   // PB0, SX1262 DIO1

#endif
