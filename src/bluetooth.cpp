#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>

#include <bluetooth/services/nsms.h>

#include <errno.h>

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_DBG);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* Implementation of two status characteristics */
BT_NSMS_DEF(nsms_btn1, "Button 1", false, "Unknown", 20);
BT_NSMS_DEF(nsms_btn2, "Button 2", IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED), "Unknown", 20);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NSMS_VAL),
};

// We cannot simply use BT_LE_ADV_CONN directly here, because C++ and C have slightly different semantics about
// taking the address of a temporary.
// We will reconstruct the same values as BT_LE_ADV_CONN and use this const instead
static const struct bt_le_adv_param adv_param =
    BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_CONNECTABLE,
        BT_GAP_ADV_FAST_INT_MIN_2,
        BT_GAP_ADV_FAST_INT_MAX_2,
        NULL);

enum class BtThreadState
{
    IDLE,
    ADVERTISING,
    CONNECTING,
    CONNECTED,
};

enum class BtThreadEvent
{
    NEW_CONNECTION,
    PAIRING_NEEDED,
    SECURITY_CHANGED,
    DISCONNECTION,
};

#define BT_THREAD_MSGQ_ALIGNMENT 4
#define BT_THREAD_MSGQ_MAX_DEPTH 10

#define REQUIRED_BT_SECURITY_LEVEL BT_SECURITY_L4

struct BtThreadCommand
{
    BtThreadEvent event; // Event that happened
    struct bt_conn* conn; // Connection associated with the event
    bt_security_t level; // Security level, if event == SECURITY_CHANGED
    uint8_t reason; // Disconnection reason
};
static_assert(sizeof(BtThreadCommand) % BT_THREAD_MSGQ_ALIGNMENT == 0, "BtThreadCommand must be a multiple of BT_THREAD_MSGQ_ALIGNMENT");

struct BtThreadContext
{
    BtThreadState state; // Current state
    struct bt_conn* conn; // Current connection info, reference counted with bt_conn_ref/bt_conn_unref
};

K_MSGQ_DEFINE(
    bt_thread_command_msgq,  /* name */
    sizeof(BtThreadCommand), /* message size */
    BT_THREAD_MSGQ_MAX_DEPTH /* max msgs */,
    BT_THREAD_MSGQ_ALIGNMENT /* alignment */);

void bt_thread_func(void* a, void* b, void* c);

K_THREAD_DEFINE(
    bt_thread, 
    8096,
    bt_thread_func,
    NULL,
    NULL,
    NULL,
    6,
    0,
    0
);

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err)
    {
        LOG_ERR("Connection failed (err %u)", err);
        return;
    }

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Connected to %s", addr);

    // Send an event to the BT thread
    BtThreadCommand cmd;
    cmd.event = BtThreadEvent::NEW_CONNECTION;

    // We are about to take a long-term reference to this BT connection, so we must increase
    // the refcount to ensure it stays valid.
    // We do this before sending to the thread to ensure the connection object remains valid 
    // while it's sitting in the msgq

    cmd.conn = bt_conn_ref(conn);

    if (cmd.conn == NULL) {
        LOG_ERR("Failed to refcount the Bluetooth connection!");
        return;
    }

    int ret = k_msgq_put(&bt_thread_command_msgq, &cmd, K_NO_WAIT);
    if (ret) {
        LOG_ERR("Failed to put Bluetooth connection event on thread msgq!");
        // Need to un-ref the BT Conn to avoid leaks
        bt_conn_unref(cmd.conn);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected (reason %u)", reason);
    
    BtThreadCommand cmd;
    cmd.event = BtThreadEvent::DISCONNECTION;

    // Do not decref the conn yet, we will do that in the BT thread. We want the conn object
    // to remain valid while this event sits in the msgq
    cmd.conn = conn;
    cmd.reason = reason;

    int ret = k_msgq_put(&bt_thread_command_msgq, &cmd, K_NO_WAIT);
    if (ret) {
        LOG_ERR("Failed to put Bluetooth disconnection event on thread msgq!");
    }
}

#if IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED)
static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err)
    {
        LOG_INF("Security changed: %s level %u", addr, level);
    }
    else
    {
        LOG_ERR("Security failed: %s level %u err %d", addr, level,
                err);
    }

    BtThreadCommand cmd;
    cmd.event = BtThreadEvent::SECURITY_CHANGED;

    // Do not decref the conn yet, we will do that in the BT thread. We want the conn object
    // to remain valid while this event sits in the msgq
    cmd.conn = conn;
    cmd.level = level;

    int ret = k_msgq_put(&bt_thread_command_msgq, &cmd, K_NO_WAIT);
    if (ret) {
        LOG_ERR("Failed to put Bluetooth security change event on thread msgq!");
    }
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
#if IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED)
    .security_changed = security_changed,
#endif
};

#if IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Passkey for %s: %06u", addr, passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Pairing cancelled: %s", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Pairing failed conn: %s, reason %d", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .passkey_display = auth_passkey_display,
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed};
#else
static struct bt_conn_auth_cb conn_auth_callbacks;
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;
#endif /* IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED) */

/*
static void button_changed(uint32_t button_state, uint32_t has_changed)
{
    if (has_changed & STATUS1_BUTTON) {
        bt_nsms_set_status(&nsms_btn1,
                   (button_state & STATUS1_BUTTON) ? "Pressed" : "Released");
    }
    if (has_changed & STATUS2_BUTTON) {
        bt_nsms_set_status(&nsms_btn2,
                   (button_state & STATUS2_BUTTON) ? "Pressed" : "Released");
    }
}*/

/*
static int init_button(void)
{
    int err;

    err = dk_buttons_init(button_changed);
    if (err) {
        printk("Cannot init buttons (err: %d)\n", err);
    }

    return err;
}*/

// Helper function to start BT advertising
int bt_start_advertising()
{
    return bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
}

int bt_stop_advertising()
{
    return bt_le_adv_stop();
}

const char *bt_state_to_string(const BtThreadState &state)
{
    switch (state)
    {
    case BtThreadState::IDLE:
        return "IDLE";

    case BtThreadState::ADVERTISING:
        return "ADVERTISING";

    case BtThreadState::CONNECTING:
        return "CONNECTING";

    case BtThreadState::CONNECTED:
        return "CONNECTED";
    }

    return "UNKNOWN";
}

int bt_state_change_to(BtThreadContext *ctx, const BtThreadState &targetState)
{
    int err = 0;

    // Perform the onExit action for the current state
    // If error occurrs leaving the current state, set err to a non-zero value and
    // entry to the next state will be aborted
    LOG_DBG("%s: onExit", bt_state_to_string(ctx->state));

    switch (ctx->state)
    {
    case BtThreadState::IDLE:
        break;

    case BtThreadState::ADVERTISING:
        // When exiting the advertising state, stop advertising
        err = bt_stop_advertising();
        break;

    case BtThreadState::CONNECTING:
        break;

    case BtThreadState::CONNECTED:
        break;
    }

    if (err)
    {
        LOG_ERR("Failed to exit %s: err %d", bt_state_to_string(ctx->state), err);
        return err;
    }

    // Perform onEntry action for the targetState
    LOG_DBG("%s: onEntry", bt_state_to_string(targetState));
    switch (targetState)
    {
    case BtThreadState::IDLE:
        break;

    case BtThreadState::ADVERTISING:
        // On entry to Advertising state, start advertising
        err = bt_start_advertising();
        break;

    case BtThreadState::CONNECTING:
        break;

    case BtThreadState::CONNECTED:
        break;
    }

    if (err)
    {
        LOG_ERR("Failed to enter %s: err %d", bt_state_to_string(targetState), err);
        return err;
    }

    LOG_DBG("State changed from %s -> %s", bt_state_to_string(ctx->state), bt_state_to_string(targetState));
    ctx->state = targetState;

    return 0;
}

void bt_state_idle_handle_command(BtThreadContext *ctx, const BtThreadCommand *command)
{
    switch (command->event)
    {
    case BtThreadEvent::NEW_CONNECTION:
        LOG_ERR("Got NEW_CONNECTION in the IDLE state!");
        break;

    case BtThreadEvent::DISCONNECTION:
        LOG_ERR("Got DISCONNECTION in the IDLE state!");
        break;

    case BtThreadEvent::PAIRING_NEEDED:
        LOG_ERR("Got PAIRING_NEEDED in the IDLE state!");
        break;

    case BtThreadEvent::SECURITY_CHANGED:
        LOG_ERR("Got SECURITY_CHANGED in the IDLE state!");
        break;

        // No default here: we want the compiler kicking and screaming if we forget one of these
    }
}

void bt_state_advertising_handle_command(BtThreadContext *ctx, const BtThreadCommand *command)
{
    char addr[BT_ADDR_LE_STR_LEN];

    switch (command->event)
    {
    case BtThreadEvent::NEW_CONNECTION:
        // Record the address of this connection in our thread context
        ctx->conn = command->conn;

        // Indicate that this connection requires L4 security
        bt_conn_set_security(ctx->conn, REQUIRED_BT_SECURITY_LEVEL);
        
        // Change to the connecting state
        bt_state_change_to(ctx, BtThreadState::CONNECTING);
        break;

    case BtThreadEvent::DISCONNECTION:
        // Generally shouldn't be possible since we try hard to only be connected to a single peer
        // Not critical? Lets log a warning and continue advertising
        bt_addr_le_to_str(bt_conn_get_dst(command->conn), addr, sizeof(addr));
        LOG_WRN("Unexpected BT disconnection during advertising: %s", addr);
        break;

    case BtThreadEvent::SECURITY_CHANGED:
        // Generally shouldn't be possible since we try hard to only be connected to a single peer
        // Not critical? Lets log a warning and continue advertising
        bt_addr_le_to_str(bt_conn_get_dst(command->conn), addr, sizeof(addr));
        LOG_WRN("Unexpected security change during advertising: %s", addr);
        break;

    case BtThreadEvent::PAIRING_NEEDED:
        // Generally shouldn't be possible since we try hard to only be connected to a single peer
        // Not critical? Lets log a warning and continue advertising
        bt_addr_le_to_str(bt_conn_get_dst(command->conn), addr, sizeof(addr));
        LOG_WRN("Unexpected pairing event during advertising: %s", addr);
        break;

        // No default here: we want the compiler kicking and screaming if we forget one of these
    }
}

void bt_state_connecting_handle_command(BtThreadContext *ctx, const BtThreadCommand *cmd)
{
    char addrConnecting[BT_ADDR_LE_STR_LEN];
    char addrNew[BT_ADDR_LE_STR_LEN];

    if (ctx->conn != cmd->conn) {
        bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
        bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
        LOG_ERR("Got a command for a different connection: in-progress connection with '%s', new connection to '%s'", addrConnecting, addrNew);
        return;
    }

    switch (cmd->event)
    {
    case BtThreadEvent::NEW_CONNECTION:
        // Generally shouldn't be possible since we try hard to only be connected to a single peer
        // Not critical? Lets log a warning and continue advertising
        bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
        bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
        LOG_WRN("Unexpected new connection from '%s' while attempting to connect with '%s'", addrNew, addrConnecting);
        LOG_WRN("New connection rejected");
        break;

    case BtThreadEvent::DISCONNECTION:
        LOG_WRN("Peer left before we could complete a connection. Return to advertising");
        bt_conn_unref(ctx->conn); // Decref the conn so we don't have a leak
        ctx->conn = NULL;
        bt_state_change_to(ctx, BtThreadState::ADVERTISING);
        break;

    case BtThreadEvent::SECURITY_CHANGED:
        // What level of security did the peer upgrade to?
        LOG_INF("Peer security level changed to %d", cmd->level);

        if (cmd->level == REQUIRED_BT_SECURITY_LEVEL) {
            LOG_DBG("Required security level achieved");
            bt_state_change_to(ctx, BtThreadState::CONNECTED);
        } else {
            LOG_ERR("Failed to reach required security level %d, got %d instead", REQUIRED_BT_SECURITY_LEVEL, cmd->level);
            // Disconnect from the peer
            int ret = bt_conn_disconnect(ctx->conn, BT_HCI_ERR_AUTH_FAIL);
            if (ret) {
                LOG_ERR("Failed to disconnect remote connection! %d", ret);
            }
        }
        break;

    case BtThreadEvent::PAIRING_NEEDED:
        // Peer needs to enter a pin code to pair with us
        LOG_INF("Peer needs to enter a pin code to pair");
        break;

        // No default here: we want the compiler kicking and screaming if we forget one of these
    }
}

void bt_state_connected_handle_command(BtThreadContext *ctx, const BtThreadCommand *cmd)
{
    char addrConnecting[BT_ADDR_LE_STR_LEN];
    char addrNew[BT_ADDR_LE_STR_LEN];

    if (ctx->conn != cmd->conn) {
        bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
        bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
        LOG_ERR("Got a command for a different connection: in-progress connection with '%s', new connection to '%s'", addrConnecting, addrNew);
        return;
    }

    switch (cmd->event)
    {
    case BtThreadEvent::NEW_CONNECTION:
        // Generally shouldn't be possible since we try hard to only be connected to a single peer
        // Not critical? Lets log a warning and continue advertising
        bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
        bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
        LOG_WRN("Unexpected new connection from '%s' while attempting to connect with '%s'", addrNew, addrConnecting);
        LOG_WRN("New connection rejected");
        break;

    case BtThreadEvent::DISCONNECTION:
        LOG_WRN("Peer left before we could complete a connection. Return to advertising");
        bt_conn_unref(ctx->conn); // Decref the conn so we don't have a leak
        ctx->conn = NULL;
        bt_state_change_to(ctx, BtThreadState::ADVERTISING);
        break;

    case BtThreadEvent::SECURITY_CHANGED:
        // What level of security did the peer upgrade to?
        LOG_INF("Peer security level changed to %d", cmd->level);

        if (cmd->level == REQUIRED_BT_SECURITY_LEVEL) {
            LOG_DBG("Required security level achieved");
            bt_state_change_to(ctx, BtThreadState::CONNECTED);
        } else {
            LOG_ERR("Failed to reach required security level %d, got %d instead", REQUIRED_BT_SECURITY_LEVEL, cmd->level);
            // Disconnect from the peer
            int ret = bt_conn_disconnect(ctx->conn, BT_HCI_ERR_AUTH_FAIL);
            if (ret) {
                LOG_ERR("Failed to disconnect remote connection! %d", ret);
            }
        }
        break;

    case BtThreadEvent::PAIRING_NEEDED:
        // Generally shouldn't be possible since we try hard to only be connected to a single peer
        // Not critical? Lets log a warning and continue advertising
        bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
        bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
        LOG_WRN("Unexpected pairing needed '%s' while attempting to connect with '%s'", addrNew, addrConnecting);
        LOG_WRN("Rejecting command");
        break;

        // No default here: we want the compiler kicking and screaming if we forget one of these
    }
}

void bt_thread_func(void *a, void *b, void *c)
{
    // We want to build a state machine based on Bluetooth events
    // Basic flow we want is:
    // - Entry -> Idle
    // - Idle -> Advertising (LED indicates advertising)
    // - Advertising -> Connecting (LED fast BT flashing, stop BLE advertising, request L4 security)
    // - Optional: Connecting -> Connecting (L4 security key sharing needed)
    // - Connecting -> Connected (L4 security complete)
    // - Connected -> Advertising (Disconnect event)

    BtThreadContext ctx;
    ctx.state = BtThreadState::IDLE;
    ctx.conn = NULL;

    int err = bt_state_change_to(&ctx, BtThreadState::ADVERTISING);

    if (err) {
        LOG_ERR("Failed to initially start bluetooth advertising! %d", err);
        return;
    }

    while (true)
    {
        // Get a message off the queue
        BtThreadCommand command;
        int ret = k_msgq_get(&bt_thread_command_msgq, &command, K_FOREVER);

        if (ret != 0) {
            LOG_ERR("Unexpected return code from k_msgq_get(): %d", ret);
            continue;
        }

        // Decide what to do with the message based on the current state
        switch (ctx.state)
        {
        case BtThreadState::IDLE:
            bt_state_idle_handle_command(&ctx, &command);
            break;

        case BtThreadState::ADVERTISING:
            bt_state_advertising_handle_command(&ctx, &command);
            break;

        case BtThreadState::CONNECTING:
            bt_state_connecting_handle_command(&ctx, &command);
            break;

        case BtThreadState::CONNECTED:
            bt_state_connected_handle_command(&ctx, &command);
            break;

            // No default: we want the compiler to kick and scream if we miss one of these
        }
    }
}



static int bluetooth_init(const struct device *dev)
{
    int err = 0;
    if (IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED))
    {
        err = bt_conn_auth_cb_register(&conn_auth_callbacks);
        if (err)
        {
            LOG_ERR("Failed to register authorization callbacks.");
            return -EFAULT;
        }

        err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
        if (err)
        {
            LOG_ERR("Failed to register authorization info callbacks.");
            return -EFAULT;
        }
    }

    err = bt_enable(NULL);
    if (err)
    {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return -EFAULT;
    }

    LOG_INF("Bluetooth initialized");

    // Settings must be loaded after bt_enable() because BT uses settings to store bonding information
    if (IS_ENABLED(CONFIG_SETTINGS))
    {
        settings_load();
    }

    LOG_INF("Settings loaded");


    return 0;
}

SYS_INIT(bluetooth_init, APPLICATION, 1);