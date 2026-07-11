#include <bluetooth/bt_state_observer.h>
#include <bluetooth/services/nsms.h>
#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(bluetooth, LOG_LEVEL_DBG);

static BtStateObserver *sBtStateObserver = nullptr;

void bluetooth_register_state_observer(BtStateObserver *observer) {
    sBtStateObserver = observer;
}

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

// We cannot simply use BT_LE_ADV_CONN directly here, because C++ and C have slightly different
// semantics about taking the address of a temporary. We will reconstruct the same values as
// BT_LE_ADV_CONN and use this const instead
static const struct bt_le_adv_param adv_param =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_NAME, BT_GAP_ADV_FAST_INT_MIN_2,
                         BT_GAP_ADV_FAST_INT_MAX_2, NULL);

// Requested once the peer reaches CONNECTED, to cut per-GATT-operation latency during the
// app's discovery read loop (issue #41). Neither side requests a fast interval otherwise, so
// the connection runs at the default ~30-50ms (BT_GAP_INIT_CONN_INT_MIN/MAX) for the entire
// ~170-operation sequential discovery walk. 6-12 (7.5-15ms) mirrors Android's "high priority"
// connection request; latency=0, timeout=400 (4s) are conventional defaults for a peripheral
// with no power-sensitivity concerns during this initial handshake.
static const struct bt_le_conn_param fast_conn_param = BT_LE_CONN_PARAM_INIT(6, 12, 0, 400);

// Storage for runtime-built BT device name (base name + " XXXX" serial suffix)
static char sBtDeviceName[CONFIG_BT_DEVICE_NAME_MAX + 1];

// Reads FICR device ID and builds "<CONFIG_BT_DEVICE_NAME> XXXX" into sBtDeviceName,
// where XXXX is the last 2 bytes of the 8-byte device ID as uppercase hex.
// Falls back to CONFIG_BT_DEVICE_NAME if hwinfo is unavailable.
static void build_bt_device_name(void) {
    uint8_t dev_id[8] = {};
    ssize_t len = hwinfo_get_device_id(dev_id, sizeof(dev_id));

    if (len < 2) {
        strncpy(sBtDeviceName, CONFIG_BT_DEVICE_NAME, sizeof(sBtDeviceName) - 1);
        sBtDeviceName[sizeof(sBtDeviceName) - 1] = '\0';
        return;
    }

    snprintk(sBtDeviceName, sizeof(sBtDeviceName), "%s %02X%02X", CONFIG_BT_DEVICE_NAME,
             dev_id[len - 2], dev_id[len - 1]);
}

enum class BtThreadState {
    IDLE,
    ADVERTISING,
    CONNECTING,
    CONNECTED,
};

enum class BtThreadEvent {
    NEW_CONNECTION,
    PAIRING_NEEDED,
    SECURITY_CHANGED,
    DISCONNECTION,
};

#define BT_THREAD_MSGQ_ALIGNMENT 4
#define BT_THREAD_MSGQ_MAX_DEPTH 10

#define REQUIRED_BT_SECURITY_LEVEL BT_SECURITY_L4

struct BtThreadCommand {
    BtThreadEvent event;   // Event that happened
    struct bt_conn *conn;  // Connection associated with the event
    bt_security_t level;   // Security level, if event == SECURITY_CHANGED
    uint8_t reason;        // Disconnection reason
    unsigned int pairingKey;
};
static_assert(sizeof(BtThreadCommand) % BT_THREAD_MSGQ_ALIGNMENT == 0,
              "BtThreadCommand must be a multiple of BT_THREAD_MSGQ_ALIGNMENT");

struct BtThreadContext {
    BtThreadState state;  // Current state
    struct bt_conn
        *conn;  // Current connection info, reference counted with bt_conn_ref/bt_conn_unref
};

K_MSGQ_DEFINE(bt_thread_command_msgq,  /* name */
              sizeof(BtThreadCommand), /* message size */
              BT_THREAD_MSGQ_MAX_DEPTH /* max msgs */, BT_THREAD_MSGQ_ALIGNMENT /* alignment */);

void bt_thread_func(void *a, void *b, void *c);

// Stack size verified against real connect/disconnect/GATT-write traffic (issue #75):
// high-water mark stayed at 724 B out of the previous 8096 B budget. 2048 B leaves ~2.8x margin.
// Kernel-only thread: K_KERNEL_* skips the 1KB CONFIG_USERSPACE privileged stack;
// this stack can never host a K_USER thread.
K_KERNEL_THREAD_DEFINE(bt_thread, 2048, bt_thread_func, NULL, NULL, NULL, 6, 0, 0);

// Diagnostic-only (issue #41 investigation): tracks the currently connected peer so the
// `bt_conn_info` shell command (below) can report live LE connection parameters on demand,
// and so le_param_updated() can log the *actual* negotiated interval (vs. the interval we
// merely requested via bt_conn_le_param_update) with a timestamp, to verify whether the
// fast-interval request has converged by the time the app's GATT discovery walk runs.
static struct bt_conn *s_active_conn = NULL;

// Mirrors the BT thread's private ctx.state so the `bt_state` shell command (below) can
// report ADVERTISING/CONNECTING/CONNECTED from any thread without reaching into the BT
// thread's stack. Written only from bt_state_change_to() (BT thread), read from the shell
// thread; a torn read is harmless here (worst case a one-transition-stale label in a
// human-facing diagnostic), so no lock is needed.
static volatile BtThreadState s_current_state = BtThreadState::IDLE;

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        LOG_ERR("Connection failed (err %u)", err);
        return;
    }

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Connected to %s", addr);

    if (s_active_conn) {
        bt_conn_unref(s_active_conn);
    }
    s_active_conn = bt_conn_ref(conn);

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

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    LOG_INF("Disconnected (reason %u)", reason);

    if (s_active_conn == conn) {
        bt_conn_unref(s_active_conn);
        s_active_conn = NULL;
    }

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

// Fires whenever the LE connection's actual parameters change - including the response to
// our own bt_conn_le_param_update(&fast_conn_param) call in
// bt_state_connecting_handle_command() below. This is the ground truth for "did the fast
// interval actually take effect, and when" - bt_conn_le_param_update() only sends a request,
// it doesn't tell us when (or whether) the peer actually applied it.
static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                             uint16_t timeout) {
    // interval/timeout are in 1.25ms/10ms units respectively (Core spec); avoid float printf
    // (CONFIG_CBPRINTF_FP_SUPPORT isn't guaranteed enabled) by computing hundredths of a ms by hand.
    LOG_INF("LE conn param updated: interval=%u units (%u.%02ums), latency=%u, timeout=%u units (%ums)",
            interval, (interval * 125) / 100, (interval * 125) % 100, latency, timeout,
            timeout * 10);
}

#if IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED)
static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err) {
        LOG_INF("Security changed: %s level %u", addr, level);
    } else {
        LOG_ERR("Security failed: %s level %u err %d", addr, level, err);
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
    .le_param_updated = le_param_updated,
#if IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED)
    .security_changed = security_changed,
#endif
};

#if IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey) {
    BtThreadCommand cmd;
    cmd.event = BtThreadEvent::PAIRING_NEEDED;

    // Do not decref the conn yet, we will do that in the BT thread. We want the conn object
    // to remain valid while this event sits in the msgq
    cmd.conn = conn;
    cmd.pairingKey = passkey;

    int ret = k_msgq_put(&bt_thread_command_msgq, &cmd, K_NO_WAIT);
    if (ret) {
        LOG_ERR("Failed to put Bluetooth disconnection event on thread msgq!");
    }

    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Passkey for %s: %06u", addr, passkey);
}

static void auth_cancel(struct bt_conn *conn) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Pairing cancelled: %s", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Pairing failed conn: %s, reason %d", addr, reason);
}

static void bond_deleted(uint8_t id, const bt_addr_le_t *peer) {
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(peer, addr, sizeof(addr));

    LOG_INF("Bond deleted: id %u, peer %s", id, addr);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .passkey_display = auth_passkey_display,
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {.pairing_complete = pairing_complete,
                                                               .pairing_failed = pairing_failed,
                                                               .bond_deleted = bond_deleted};
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
int bt_start_advertising() {
    return bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL /* scan response contents array */,
                           0 /* scan response contents array length */);
}

int bt_stop_advertising() {
    return bt_le_adv_stop();
}

const char *bt_state_to_string(const BtThreadState &state) {
    switch (state) {
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

int bt_state_change_to(BtThreadContext *ctx, const BtThreadState &targetState) {
    int err = 0;

    // Perform the onExit action for the current state
    // If error occurrs leaving the current state, set err to a non-zero value and
    // entry to the next state will be aborted
    LOG_DBG("%s: onExit", bt_state_to_string(ctx->state));

    switch (ctx->state) {
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

    if (err) {
        LOG_ERR("Failed to exit %s: err %d", bt_state_to_string(ctx->state), err);
        return err;
    }

    // Perform onEntry action for the targetState
    LOG_DBG("%s: onEntry", bt_state_to_string(targetState));
    switch (targetState) {
        case BtThreadState::IDLE:
            break;

        case BtThreadState::ADVERTISING:
            // On entry to Advertising state, start advertising
            err = bt_start_advertising();
            if (sBtStateObserver)
                sBtStateObserver->onAdvertisingStarted();
            break;

        case BtThreadState::CONNECTING:
            if (sBtStateObserver)
                sBtStateObserver->onConnectingStarted();
            break;

        case BtThreadState::CONNECTED:
            if (sBtStateObserver)
                sBtStateObserver->onConnected();
            break;
    }

    if (err) {
        LOG_ERR("Failed to enter %s: err %d", bt_state_to_string(targetState), err);
        return err;
    }

    LOG_DBG("State changed from %s -> %s", bt_state_to_string(ctx->state),
            bt_state_to_string(targetState));
    ctx->state = targetState;
    s_current_state = targetState;  // mirror for the `bt_state` shell command

    return 0;
}

void bt_state_idle_handle_command(BtThreadContext *ctx, const BtThreadCommand *command) {
    switch (command->event) {
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

void bt_state_advertising_handle_command(BtThreadContext *ctx, const BtThreadCommand *command) {
    char addr[BT_ADDR_LE_STR_LEN];

    switch (command->event) {
        case BtThreadEvent::NEW_CONNECTION:
            // Record the address of this connection in our thread context
            ctx->conn = command->conn;

            // Indicate that this connection requires L4 security
            bt_conn_set_security(ctx->conn, REQUIRED_BT_SECURITY_LEVEL);

            // Change to the connecting state
            bt_state_change_to(ctx, BtThreadState::CONNECTING);
            break;

        case BtThreadEvent::DISCONNECTION:
            // Generally shouldn't be possible since we try hard to only be connected to a single
            // peer Not critical? Lets log a warning and continue advertising
            bt_addr_le_to_str(bt_conn_get_dst(command->conn), addr, sizeof(addr));
            LOG_WRN("Unexpected BT disconnection during advertising: %s", addr);
            break;

        case BtThreadEvent::SECURITY_CHANGED:
            // Generally shouldn't be possible since we try hard to only be connected to a single
            // peer Not critical? Lets log a warning and continue advertising
            bt_addr_le_to_str(bt_conn_get_dst(command->conn), addr, sizeof(addr));
            LOG_WRN("Unexpected security change during advertising: %s", addr);
            break;

        case BtThreadEvent::PAIRING_NEEDED:
            // Generally shouldn't be possible since we try hard to only be connected to a single
            // peer Not critical? Lets log a warning and continue advertising
            bt_addr_le_to_str(bt_conn_get_dst(command->conn), addr, sizeof(addr));
            LOG_WRN("Unexpected pairing event during advertising: %s", addr);
            break;

            // No default here: we want the compiler kicking and screaming if we forget one of these
    }
}

void bt_state_connecting_handle_command(BtThreadContext *ctx, const BtThreadCommand *cmd) {
    char addrConnecting[BT_ADDR_LE_STR_LEN];
    char addrNew[BT_ADDR_LE_STR_LEN];

    if (ctx->conn != cmd->conn) {
        bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
        bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
        LOG_ERR(
            "Got a command for a different connection: in-progress connection with '%s', new "
            "connection to '%s'",
            addrConnecting, addrNew);
        return;
    }

    switch (cmd->event) {
        case BtThreadEvent::NEW_CONNECTION:
            // Generally shouldn't be possible since we try hard to only be connected to a single
            // peer Not critical? Lets log a warning and continue advertising
            bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
            bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
            LOG_WRN("Unexpected new connection from '%s' while attempting to connect with '%s'",
                    addrNew, addrConnecting);
            LOG_WRN("New connection rejected");
            break;

        case BtThreadEvent::DISCONNECTION:
            LOG_WRN("Peer left before we could complete a connection. Return to advertising");
            bt_conn_unref(ctx->conn);  // Decref the conn so we don't have a leak
            ctx->conn = NULL;
            bt_state_change_to(ctx, BtThreadState::ADVERTISING);
            break;

        case BtThreadEvent::SECURITY_CHANGED:
            // What level of security did the peer upgrade to?
            LOG_INF("Peer security level changed to %d", cmd->level);

            if (cmd->level == REQUIRED_BT_SECURITY_LEVEL) {
                LOG_DBG("Required security level achieved");

                int ret = bt_conn_le_param_update(ctx->conn, &fast_conn_param);
                if (ret) {
                    LOG_WRN("Failed to request fast connection parameters: %d", ret);
                }

                bt_state_change_to(ctx, BtThreadState::CONNECTED);
            } else {
                LOG_ERR("Failed to reach required security level %d, got %d instead",
                        REQUIRED_BT_SECURITY_LEVEL, cmd->level);

                // Disconnect from the peer
                // int ret = bt_conn_disconnect(ctx->conn, BT_HCI_ERR_AUTH_FAIL);
                // if (ret) {
                //     LOG_ERR("Failed to disconnect remote connection! %d", ret);
                // }
            }
            break;

        case BtThreadEvent::PAIRING_NEEDED:
            // Peer needs to enter a pin code to pair with us
            LOG_INF("Peer needs to enter a pin code to pair");
            if (sBtStateObserver)
                sBtStateObserver->onPairingCodeRequired(cmd->pairingKey);
            break;

            // No default here: we want the compiler kicking and screaming if we forget one of these
    }
}

void bt_state_connected_handle_command(BtThreadContext *ctx, const BtThreadCommand *cmd) {
    char addrConnecting[BT_ADDR_LE_STR_LEN];
    char addrNew[BT_ADDR_LE_STR_LEN];

    if (ctx->conn != cmd->conn) {
        bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
        bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
        LOG_ERR(
            "Got a command for a different connection: in-progress connection with '%s', new "
            "connection to '%s'",
            addrConnecting, addrNew);
        return;
    }

    switch (cmd->event) {
        case BtThreadEvent::NEW_CONNECTION:
            // Generally shouldn't be possible since we try hard to only be connected to a single
            // peer Not critical? Lets log a warning and continue advertising
            bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
            bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
            LOG_WRN("Unexpected new connection from '%s' while attempting to connect with '%s'",
                    addrNew, addrConnecting);
            LOG_WRN("New connection rejected");
            break;

        case BtThreadEvent::DISCONNECTION:
            LOG_WRN("Peer left. Return to advertising");
            bt_conn_unref(ctx->conn);  // Decref the conn so we don't have a leak
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
                LOG_WRN("Reached security %d, must reach %d to connect", cmd->level,
                        REQUIRED_BT_SECURITY_LEVEL);
                // TODO: need to implement timeout to disconnect if we don't reach "CONNECTED" state
                // within ~60s
            }
            break;

        case BtThreadEvent::PAIRING_NEEDED:
            // Generally shouldn't be possible since we try hard to only be connected to a single
            // peer Not critical? Lets log a warning and continue advertising
            bt_addr_le_to_str(bt_conn_get_dst(cmd->conn), addrNew, sizeof(addrNew));
            bt_addr_le_to_str(bt_conn_get_dst(ctx->conn), addrConnecting, sizeof(addrConnecting));
            LOG_WRN("Unexpected pairing needed '%s' while attempting to connect with '%s'", addrNew,
                    addrConnecting);
            LOG_WRN("Rejecting command");
            break;

            // No default here: we want the compiler kicking and screaming if we forget one of these
    }
}

void bt_thread_func(void *a, void *b, void *c) {
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

    while (true) {
        // Get a message off the queue
        BtThreadCommand command;
        int ret = k_msgq_get(&bt_thread_command_msgq, &command, K_FOREVER);

        if (ret != 0) {
            LOG_ERR("Unexpected return code from k_msgq_get(): %d", ret);
            continue;
        }

        // Decide what to do with the message based on the current state
        switch (ctx.state) {
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

static int bluetooth_init(void) {
    int err = 0;
    if (IS_ENABLED(CONFIG_BT_STATUS_SECURITY_ENABLED)) {
        err = bt_conn_auth_cb_register(&conn_auth_callbacks);
        if (err) {
            LOG_ERR("Failed to register authorization callbacks.");
            return -EFAULT;
        }

        err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
        if (err) {
            LOG_ERR("Failed to register authorization info callbacks.");
            return -EFAULT;
        }
    }

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return -EFAULT;
    }

    LOG_INF("Bluetooth initialized");

    build_bt_device_name();
    err = bt_set_name(sBtDeviceName);
    if (err) {
        LOG_WRN("Failed to set BT device name (err %d), using default", err);
    }
    LOG_INF("BT device name: %s", sBtDeviceName);

    // Settings must be loaded after bt_enable() because BT uses settings to store bonding
    // information
    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }

    LOG_INF("Settings loaded");

    return 0;
}

SYS_INIT(bluetooth_init, APPLICATION, 1);

static int cmd_serial_print(const struct shell *sh, size_t argc, char **argv) {
    uint8_t dev_id[8] = {};
    ssize_t len = hwinfo_get_device_id(dev_id, sizeof(dev_id));

    if (len <= 0) {
        shell_error(sh, "Failed to read device ID (err %d)", (int)len);
        return -EIO;
    }

    shell_print(sh, "Device serial (%d bytes):", (int)len);
    for (ssize_t i = 0; i < len; i++) {
        shell_fprintf(sh, SHELL_NORMAL, "%02X", dev_id[i]);
    }
    shell_fprintf(sh, SHELL_NORMAL, "\n");
    shell_print(sh, "BT device name: %s", sBtDeviceName);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(serial_cmds,
                               SHELL_CMD(print, NULL, "Print device serial number and BT name",
                                         cmd_serial_print),
                               SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(serial, &serial_cmds, "Serial number commands", NULL);

// Diagnostic-only (issue #41 investigation): prints the *actual* current LE connection
// parameters on demand, so they can be polled mid-discovery to verify whether the fast
// interval requested by bt_conn_le_param_update() has converged. See le_param_updated()
// above for the complementary "log it the moment it changes" half of this investigation.
static int cmd_bt_conn_info(const struct shell *sh, size_t argc, char **argv) {
    if (!s_active_conn) {
        shell_print(sh, "No active BLE connection");
        return 0;
    }

    struct bt_conn_info info;
    int ret = bt_conn_get_info(s_active_conn, &info);
    if (ret) {
        shell_error(sh, "bt_conn_get_info failed: %d", ret);
        return ret;
    }

    if (info.type != BT_CONN_TYPE_LE) {
        shell_print(sh, "Active connection is not LE (type %d)", info.type);
        return 0;
    }

    shell_print(sh, "LE connection interval: %u units (%u.%02ums)", info.le.interval,
                (info.le.interval * 125) / 100, (info.le.interval * 125) % 100);
    shell_print(sh, "LE connection latency: %u", info.le.latency);
    shell_print(sh, "LE connection supervision timeout: %u units (%ums)", info.le.timeout,
                info.le.timeout * 10);

    return 0;
}

SHELL_CMD_REGISTER(bt_conn_info, NULL, "Print current LE connection parameters", cmd_bt_conn_info);

// One-shot snapshot of everything that distinguishes a healthy BLE link from a
// "split-brain" (board thinks it's connected, phone's app timed out). Added after
// issue #90's multi-day debug of exactly that: the tells are the SECURITY level
// (did LE Secure Connections pairing finish?) and the negotiated ATT MTU (an MTU
// still at the 23-byte default means the phone's Exchange MTU never completed and
// its GATT ops are almost certainly hung). Prefer this over piecing the same
// picture together from a native `adb logcat` BLE trace.
static int cmd_bt_state(const struct shell *sh, size_t argc, char **argv) {
    const BtThreadState state = s_current_state;  // one stable read of the volatile mirror
    shell_print(sh, "BT state machine: %s", bt_state_to_string(state));
    shell_print(sh, "Advertising: %s", state == BtThreadState::ADVERTISING ? "yes" : "no");

    // This shell command runs on the shell thread while disconnected() runs on
    // the BT RX thread and does `bt_conn_unref(s_active_conn); s_active_conn =
    // NULL`. Reading the global repeatedly below would race that: it could go
    // NULL (and its last ref drop) between two of our uses. Take our own ref up
    // front into a local and use only that - bt_conn objects live in a static
    // pool, so bt_conn_ref() safely returns NULL if the refcount already hit zero
    // in the tiny window between reading the global and ref-ing it (we treat that
    // exactly like "no connection"). Every field below reads `conn`, never the
    // global, and we drop our ref before returning.
    struct bt_conn *conn = s_active_conn;
    conn = conn ? bt_conn_ref(conn) : NULL;
    if (!conn) {
        shell_print(sh, "Active LE connection: none");
        return 0;
    }

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    shell_print(sh, "Active LE connection: %s", addr);

    // Security level: 1 = unencrypted (pairing NOT complete), 2 = encrypted,
    // 4 = LE Secure Connections + bonding (what this firmware requires before it
    // will serve GATT). Anything < 4 mid-connection means the handshake stalled.
    bt_security_t sec = bt_conn_get_security(conn);
    shell_print(sh, "Security level: L%d%s", (int)sec,
                sec >= BT_SECURITY_L4 ? " (LE Secure Connections + bonding)"
                : sec >= BT_SECURITY_L2 ? " (encrypted)"
                                        : " (UNENCRYPTED - pairing not complete)");

    // Negotiated ATT MTU. 23 = the BLE default, i.e. the phone's Exchange MTU
    // Request was never answered/never sent - the hallmark of the split-brain hang.
    uint16_t mtu = bt_gatt_get_mtu(conn);
    shell_print(sh, "ATT MTU: %u%s", mtu,
                mtu <= 23 ? " (DEFAULT - MTU exchange did not complete)" : "");

    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0 && info.type == BT_CONN_TYPE_LE) {
        shell_print(sh, "Conn interval: %u units (%u.%02ums), latency: %u, timeout: %ums",
                    info.le.interval, (info.le.interval * 125) / 100,
                    (info.le.interval * 125) % 100, info.le.latency, info.le.timeout * 10);
    }

    bt_conn_unref(conn);
    return 0;
}

SHELL_CMD_REGISTER(bt_state, NULL,
                   "Snapshot BLE link health (state, security, ATT MTU) - use first when "
                   "debugging a connection that looks stuck",
                   cmd_bt_state);

// TEST AID: toggle BLE advertising at runtime, e.g. to exercise the app's scan
// freshness (a device disappearing from / appearing in "Nearby devices"). This
// deliberately does NOT persist anything - it just calls the runtime adv start/stop
// API, so the board ALWAYS boots back into advertising via the normal state-machine
// path. It bypasses the BT state machine and is only meaningful in the ADVERTISING
// (disconnected) state; do not use it to fiddle with a live connection.
static int cmd_bt_adv(const struct shell *sh, size_t argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        shell_print(sh, "usage: bt_adv <on|off>  (test aid; not persisted)");
        return -EINVAL;
    }
    bool on = strcmp(argv[1], "on") == 0;
    int err = on ? bt_start_advertising() : bt_stop_advertising();
    // -EALREADY: already in the requested advertising state - fine for a test toggle.
    if (err && err != -EALREADY) {
        shell_error(sh, "bt_adv %s failed: %d", argv[1], err);
        return err;
    }
    shell_print(sh, "Advertising %s%s (runtime only - not persisted)",
                on ? "ENABLED" : "DISABLED", err == -EALREADY ? " [already]" : "");
    return 0;
}

SHELL_CMD_REGISTER(bt_adv, NULL,
                   "TEST AID: bt_adv <on|off> - toggle advertising at runtime (NOT persisted; "
                   "board always boots advertising). Use to exercise app scan freshness.",
                   cmd_bt_adv);
