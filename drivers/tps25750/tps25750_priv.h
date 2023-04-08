#pragma once

#define TPS25750_REG_MODE_ADDR 0x03
#define TPS25750_REG_MODE_SIZE 4
typedef struct __packed tps25750_mode
{
    char byte_count;
    char mode[TPS25750_REG_MODE_SIZE];
} tps25750_mode_t;
BUILD_ASSERT(sizeof(tps25750_mode_t) == 5);

// Possible values for the "MODE" register
#define TPS25750_REG_MODE_VAL_APP  "APP "
#define TPS25750_REG_MODE_VAL_BOOT "BOOT"
#define TPS25750_REG_MODE_VAL_PTCH "PTCH"

#define TPS25750_REG_TYPE_ADDR 0x04
#define TPS25750_REG_TYPE_SIZE 4

#define TPS25750_REG_CUSTUSE_ADDR 0x06
#define TPS25750_REG_CUSTUSE_SIZE 8

#define TPS25750_REG_CMD1_ADDR 0x08
#define TPS25750_REG_CMD1_SIZE 4

#define TPS25750_REG_CMD1_VAL_PBMS "PBMs"

typedef struct __packed tps25750_cmd1 
{
    uint8_t byte_count;
    char cmd[TPS25750_REG_CMD1_SIZE];
} tps25750_cmd1_t;
BUILD_ASSERT(sizeof(tps25750_cmd1_t) == 5);

// CMD1 register value indicating a command error
#define TPS25750_REG_CMD1_VAL_ERROR "!CMD"

#define TPS25750_REG_DATA1_ADDR 0x09
#define TPS25750_REG_DATA1_SIZE 64

#define TPS25750_REG_DEVICE_CAPABILITIES_ADDR 0x0D
#define TPS25750_REG_DEVICE_CAPABILITIES_SIZE 4

#define TPS25750_REG_VERSION_ADDR 0x0F
#define TPS25750_REG_VERSION_SIZE 4


#define TPS25750_REG_INT_EVENT1_ADDR 0x14
#define TPS25750_REG_INT_MASK1_ADDR 0x16
#define TPS25750_REG_INT_CLEAR1_ADDR 0x18

// All INT_XXXX registers have the same size
#define TPS25750_REG_INT_SIZE 11

// Format:
// #define TPS25750_INT_BIT(_name, _byte, _bit)

/* Defines the bit / byte locations within the INT registers */
#define TPS25750_INT_BIT_LIST \
    /* Byte 10: Patch Status (common to all slave ports) */ \
    /* 7:3 Reserved */ \
    TPS25750_INT_BIT(I2CMasterNACKed, 10, 2) /* A transaction on the I2C master was NACKed. */ \
    TPS25750_INT_BIT(ReadyForPatch, 10, 1) /* Device ready for a patch bundle from the host. */ \
    TPS25750_INT_BIT(PatchLoaded, 10, 0) /* Patch was loaded to the device. */ \
    /* Bytes 8-9: */ \
    /* 15:2 Reserved */ \
    TPS25750_INT_BIT(TXMemBufferEmpty, 8, 1) /* Transmit memory buffer empty. */ \
    /* 0 Reserved */ \
    /* Bytes 6-7: */ \
    /* 15:0 Reserved */ \
    /* Byte 5 */ \
    /* 7 Reserved */ \
    TPS25750_INT_BIT(ErrorUnableToSource, 5, 6) /* The Source was unable to increase the voltage to the negotiated voltage of the contract. */ \
    /* 5:4 Reserved */ \
    TPS25750_INT_BIT(PlugEarlyNotification, 5, 3) /* A connection has been detected but not debounced. */ \
    TPS25750_INT_BIT(SnkTransitionComplete, 5, 2) /* This event only occurs when in source mode (PD_STATUS.PresentPDRole = 1b). It occurs tSrcTransition (ms) after sending an Accept message to a Request message, just before sending the PS_RDY message. */ \
    /* 1 Reserved */ \
    /* 0 Reserved */ \
    /* Byte 4 */  \
    TPS25750_INT_BIT(ErrorMessageData, 4, 7) /* An erroneous message was received. */ \
    TPS25750_INT_BIT(ErrorProtocolError, 4, 6) /* An unexpected message was received from the partner device. */ \
    /* 5 Reserved */ \
    TPS25750_INT_BIT(ErrorMissingGetCapMessage, 4, 4) /* The partner device did not respond to the Get_Sink_Cap or Get_Source_Cap message that was sent. */ \
    TPS25750_INT_BIT(ErrorPowerEventOccurred, 4, 3) /* An OVP, or ILIM event occurred on VBUS. Or a TSD event occurred. */ \
    TPS25750_INT_BIT(ErrorCanProvideVoltageOrCurrentLater, 4, 2) /* The USB PD Source can provide acceptable voltage and current, but not at the present time. A "wait" message was sent or received. */ \
    TPS25750_INT_BIT(ErrorCannotProvideVoltageOrCurrent, 4, 1) /* The USB PD Source cannot provide an acceptable voltage and/or current. A Reject message was sent to the Sink or a Capability Mismatch was received from the Sink. */ \
    TPS25750_INT_BIT(ErrorDeviceIncompatible, 4, 0) /* When set to 1, a USB PD device with an incompatible specification version was connected. Or the partner device is not USB PD capable. */ \
    /* Byte 3 */ \
    /* 7 Reserved */ \
    TPS25750_INT_BIT(CMDComplete, 3, 6) /* Set whenever a non-zero value in CMD register is set to zero or !CMD */ \
    /* 5 Reserved */ \
    /* 4 Reserved */ \
    TPS25750_INT_BIT(PDStatusUpdate, 3, 3) /* Set whenever contents of PD_STATUS register (0x40) change. */ \
    TPS25750_INT_BIT(StatusUpdate, 3, 2) /* Set whenever contents of STATUS register (0x1A) change. */ \
    /* 1 Reserved */ \
    TPS25750_INT_BIT(PowerStatusUpdate, 3, 0) /* Set whenever contents of POWER_STATUS register (0x3F) change. */ \
    /* Byte 2 */ \
    TPS25750_INT_BIT(PPswitchChanged, 2, 7) /* Set whenever contents of POWER_PATH_STATUS register (0x26) changes. */ \
    /* 6 Reserved */ \
    TPS25750_INT_BIT(UsbHostPresentNoLonger, 2, 5) /* Set when STATUS.UsbHostPresent transitions to anything other than 11b. */ \
    TPS25750_INT_BIT(UsbHostPresent, 2, 4) /* Set when STATUS.UsbHostPresent transitions to 11b. */ \
    /* 3 Reserved */ \
    TPS25750_INT_BIT(DRSwapRequested, 2, 2) /* A DR swap was requested by the Port Partner. */ \
    TPS25750_INT_BIT(PRSwapRequested, 2, 1) /* A PR swap was requested by the Port Partner. */ \
    /* 0 Reserved */ \
    /* Byte 1 */ \
    /* 7 Reserved */ \
    TPS25750_INT_BIT(SourceCapMsgRcvd, 1, 6) /* This is asserted when a Source Capabilities message is received from the Port Partner. */ \
    TPS25750_INT_BIT(NewContractAsProv, 1, 5) /* An RDO from the far-end device has been accepted and the PD Controller is a Source. This is asserted after the PS_RDY message has been sent. See ACTIVE_CONTRACT_PDO register (0x34) and ACTIVE_CONTRACT_RDO register (0x35) for details. */ \
    TPS25750_INT_BIT(NewContractAsCons, 1, 4) /* Far-end source has accepted an RDO sent by the PD Controller as a Sink. See ACTIVE_CONTRACT_PDO register (0x34) and ACTIVE_CONTRACT_RDO register (0x35) for details. */ \
    /* 3:0 Reserved */ \
    /* Byte 0*/ \
    /* 7:6 Reserved*/ \
    TPS25750_INT_BIT(DRSwapComplete, 0, 5) /* A Data Role swap has completed. See STATUS register (0x1A) and PD_STATUS register (0x40) for port state.*/ \
    TPS25750_INT_BIT(PRSwapComplete, 0, 4) /* A Power role swap has completed. See STATUS register (0x1A) and PD_STATUS register (0x40) for port state.*/ \
    TPS25750_INT_BIT(PlugInsertOrRemoval, 0, 3) /* USB Plug Status has Changed. See Status register for more plug details.*/ \
    /* 2 Reserved*/ \
    TPS25750_INT_BIT(PDHardReset, 0, 1) /* A PD Hard Reset has been performed. See PD_STATUS.HardResetDetails for more information.*/ \
    /* 0 Reserved*/

// Define a bunch of functions for accessing bits in the byte array
#define TPS25750_INT_BIT(_name, _byte, _bit) \
    inline bool tps25750_int_bit_##_name(const uint8_t* bytes) { \
        return ((bytes[_byte] & (1 << _bit)) == (1 << _bit)); \
    }

TPS25750_INT_BIT_LIST
#undef TPS25750_INT_BIT

// All INT_XXXX registers use the same data format
typedef struct tps25750_int
{
#define TPS25750_INT_BIT(_name, _byte, _bit) \
    bool _name;

    TPS25750_INT_BIT_LIST
#undef TPS25750_INT_BIT
} tps25750_int_t;


#define TPS25750_REG_STATUS_ADDR 0x1A
#define TPS25750_REG_STATUS_SIZE 5

#define TPS25750_REG_POWER_PATH_STATUS_ADDR 0x26
#define TPS25750_REG_POWER_PATH_STATUS_SIZE 5

#define TPS25750_REG_PORT_CONTROL_ADDR 0x29
#define TPS25750_REG_PORT_CONTROL_SIZE 4

#define TPS25750_REG_BOOT_STATUS_ADDR 0x2D
#define TPS25750_REG_BOOT_STATUS_SIZE 5

#define TPS25750_REG_BUILD_DESCRIPTION_ADDR 0x2E
#define TPS25750_REG_BUILD_DESCRIPTION_SIZE 49

#define TPS25750_REG_DEVICE_INFO_ADDR 0x2F
#define TPS25750_REG_DEVICE_INFO_SIZE 40

#define TPS25750_REG_RX_SOURCE_CAPS_ADDR 0x30
#define TPS25750_REG_RX_SOURCE_CAPS_SIZE 29

#define TPS25750_REG_RX_SINK_CAPS_ADDR 0x31
#define TPS25750_REG_RX_SINK_CAPS_SIZE 29

#define TPS25750_REG_TX_SOURCE_CAPS_ADDR 0x32
#define TPS25750_REG_TX_SOURCE_CAPS_SIZE 31

#define TPS25750_REG_TX_SINK_CAPS_ADDR 0x33
#define TPS25750_REG_TX_SINK_CAPS_SIZE 29

#define TPS25750_REG_ACTIVE_CONTRACT_PDO_ADDR 0x34
#define TPS25750_REG_ACTIVE_CONTRACT_PDO_SIZE 6

#define TPS25750_REG_ACTIVE_CONTRACT_RDO_ADDR 0x35
#define TPS25750_REG_ACTIVE_CONTRACT_RDO_SIZE 4

#define TPS25750_REG_POWER_STATUS_ADDR 0x3F
#define TPS25750_REG_POWER_STATUS_SIZE 2

#define TPS25750_REG_PD_STATUS_ADDR 0x40
#define TPS25750_REG_PD_STATUS_SIZE 4

#define TPS25750_REG_TYPEC_STATE_ADDR 0x69
#define TPS25750_REG_TYPEC_STATE_SIZE 4

#define TPS25750_REG_GPIO_STATUS_ADDR 0x72
#define TPS25750_REG_GPIO_STATUS_SIZE 8

typedef enum tps25750_std_task_result
{
    SUCCESS = 0x0,
    TIMEOUT_OR_ABORT = 0x1,
    RESERVED = 0x2,
    REJECTED = 0x3,
    REJECTED_RX_LOCKED = 0x4,
} tps25750_std_task_result_t;

typedef struct tps25750_std_task_response
{
    uint8_t result;
} tps25750_std_task_response_t;

typedef struct __packed tps25750_pbms_data_in {
    uint8_t byte_count;
    uint8_t payload[]
} tps25750_pbms_data_in_t;

typedef struct __packed tps25750_data1 {
    uint8_t byte_count;
    uint8_t data[TPS25750_REG_DATA1_SIZE];
} tps25750_data1_t;

struct tps25750_dev_data
{
};

struct tps25750_dev_config
{
    struct i2c_dt_spec i2c;
};
