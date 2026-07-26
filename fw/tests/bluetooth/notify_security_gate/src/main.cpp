/*
 * Tests for bleAnyConnEncrypted() — the notify() security gate in
 * bt_service_cpp.h (issue #232).
 *
 * The gate must mirror gatt.c's bt_gatt_check_perm() AUTHEN branch: with every
 * characteristic value now carrying BT_GATT_PERM_*_AUTHEN, a link satisfies the
 * gate only at security level L3 or above. Before issue #232 the threshold was
 * L2 (encryption without MITM) — these tests pin the new semantics so a future
 * revert to L2 (or a perms change back to _ENCRYPT without touching the gate)
 * fails visibly.
 *
 * Like bluetooth.checked_write, this suite compiles bt_service_cpp.h with NO BT
 * host linked (CONFIG_BT=n). bt_conn_foreach / bt_conn_get_security are
 * link-time-stubbed below, so the REAL lambda inside bleAnyConnEncrypted()
 * executes against a scripted connection table — no BT stack needed.
 */

#include <zephyr/ztest.h>

#include <bluetooth/bt_service_cpp.h>

namespace {

/* Scripted connection table consumed by the stubs below. Security levels of
 * the "connected" peers; bt_conn pointers handed to the callback are just
 * indices into this array disguised as opaque pointers. */
constexpr size_t kMaxFakeConns = 4;
bt_security_t fake_levels[kMaxFakeConns];
size_t fake_conn_count;

void set_fake_conns(std::initializer_list<bt_security_t> levels) {
    fake_conn_count = 0;
    for (bt_security_t l : levels) {
        zassert_true(fake_conn_count < kMaxFakeConns, "too many fake conns");
        fake_levels[fake_conn_count++] = l;
    }
}

}  // namespace

/* Link-time stubs for the two host APIs bleAnyConnEncrypted() uses. With
 * CONFIG_BT=n the real host is not linked, so these definitions satisfy the
 * references without any symbol clash. */
extern "C" {

void bt_conn_foreach(enum bt_conn_type type,
                     void (*func)(struct bt_conn *conn, void *data), void *data) {
    zassert_equal(type, BT_CONN_TYPE_LE, "gate must iterate LE connections");
    for (size_t i = 0; i < fake_conn_count; i++) {
        /* Opaque handle: index+1 so no conn is ever NULL. */
        func(reinterpret_cast<struct bt_conn *>(i + 1), data);
    }
}

bt_security_t bt_conn_get_security(const struct bt_conn *conn) {
    size_t idx = reinterpret_cast<uintptr_t>(conn) - 1;
    zassert_true(idx < fake_conn_count, "stub handed an unknown conn");
    return fake_levels[idx];
}

}  // extern "C"

ZTEST(notify_security_gate, test_no_connections_is_blocked) {
    set_fake_conns({});
    zassert_false(bleAnyConnEncrypted(), "no connections must never satisfy the gate");
}

ZTEST(notify_security_gate, test_l1_unencrypted_is_blocked) {
    set_fake_conns({BT_SECURITY_L1});
    zassert_false(bleAnyConnEncrypted(), "an unencrypted link must not satisfy the gate");
}

ZTEST(notify_security_gate, test_l2_encrypted_unauthenticated_is_blocked) {
    /* The issue #232 semantic change: L2 (encryption without MITM) used to
     * pass the old >= L2 check, but _AUTHEN perms demand an authenticated
     * link — gatt.c would reject the notify, so the gate must too. */
    set_fake_conns({BT_SECURITY_L2});
    zassert_false(bleAnyConnEncrypted(),
                  "L2 (no MITM) must not satisfy the _AUTHEN notify gate");
}

ZTEST(notify_security_gate, test_l3_authenticated_passes) {
    set_fake_conns({BT_SECURITY_L3});
    zassert_true(bleAnyConnEncrypted(), "L3 (authenticated) must satisfy the gate");
}

ZTEST(notify_security_gate, test_l4_secure_connections_passes) {
    /* The only steady state reachable in practice: CONFIG_BT_SMP_SC_ONLY
     * rejects every pairing below L4. */
    set_fake_conns({BT_SECURITY_L4});
    zassert_true(bleAnyConnEncrypted(), "L4 must satisfy the gate");
}

ZTEST(notify_security_gate, test_any_of_semantics_across_connections) {
    set_fake_conns({BT_SECURITY_L1, BT_SECURITY_L4});
    zassert_true(bleAnyConnEncrypted(), "one qualifying link among several is enough");
}

ZTEST_SUITE(notify_security_gate, NULL, NULL, NULL, NULL, NULL);
