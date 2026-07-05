/*
 * Tests for the fallible GATT write hook (onWriteChecked, issue #97) and the
 * battery telemetry quantizer.
 *
 * onWriteChecked lets a characteristic reject a remote write when its side
 * effect (e.g. an I2C register write) fails: storage must roll back to the
 * previous value and the ATT operation must fail with
 * BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED), so the app's optimistic UI
 * update reverts deterministically (see fw/CLAUDE.md's "Refusing a GATT
 * write" rule). The tests drive the characteristic's static write() callback
 * directly with a crafted bt_gatt_attr — no BT stack needed.
 */

#include <zephyr/ztest.h>

#include <battery_util.h>
#include <bluetooth/bt_service_cpp.h>

namespace {

/* Characteristic with a fallible hook whose result the test controls. */
class CheckedChar : public BtGattAutoCharacteristicExt<CheckedChar, "Checked", false /* Notify */,
                                                       false /* ReadOnly */, uint32_t, 7> {
   public:
    using Base = BtGattAutoCharacteristicExt<CheckedChar, "Checked", false, false, uint32_t, 7>;
    using Base::operator=;

    int nextResult = 0;
    int callCount = 0;
    uint32_t lastValue = 0;

    int onWriteChecked(const uint32_t &value) {
        callCount++;
        lastValue = value;
        return nextResult;
    }
};

/* Characteristic with the classic infallible hook — must keep working unchanged. */
class PlainHookChar
    : public BtGattAutoCharacteristicExt<PlainHookChar, "Plain", false, false, uint32_t, 0> {
   public:
    using Base = BtGattAutoCharacteristicExt<PlainHookChar, "Plain", false, false, uint32_t, 0>;
    using Base::operator=;

    int callCount = 0;

    void onWrite(const uint32_t &) { callCount++; }
};

/* Characteristic with no hook at all. */
class NoHookChar
    : public BtGattAutoCharacteristicExt<NoHookChar, "NoHook", false, false, uint32_t, 0> {
   public:
    using Base = BtGattAutoCharacteristicExt<NoHookChar, "NoHook", false, false, uint32_t, 0>;
    using Base::operator=;
};

/* Drives the characteristic's static write() callback the way the ATT layer would. */
template <typename TChar>
ssize_t remote_write(TChar &instance, uint32_t value) {
    bt_gatt_attr attr{};
    attr.user_data = &instance;
    return TChar::write(nullptr, &attr, &value, sizeof(value), 0 /* offset */, 0 /* flags */);
}

}  // namespace

ZTEST_SUITE(checked_write_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(checked_write_tests, test_checked_write_success_applies_value) {
    CheckedChar ch;
    ch.nextResult = 0;

    ssize_t ret = remote_write(ch, 42u);

    zassert_equal(ret, (ssize_t)sizeof(uint32_t), "expected full length, got %zd", ret);
    zassert_equal((uint32_t)ch, 42u, "storage should hold the written value");
    zassert_equal(ch.callCount, 1);
    zassert_equal(ch.lastValue, 42u, "hook must see the new value");
}

ZTEST(checked_write_tests, test_checked_write_failure_restores_previous_value) {
    CheckedChar ch;
    ch = 13u; /* establish a previous value distinct from the default */
    ch.nextResult = -EIO;

    ssize_t ret = remote_write(ch, 99u);

    zassert_equal(ret, BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED),
                  "a failing hook must reject the ATT write, got %zd", ret);
    zassert_equal((uint32_t)ch, 13u, "storage must roll back to the previous value");
    zassert_equal(ch.callCount, 1, "hook must have been consulted exactly once");
    zassert_equal(ch.lastValue, 99u, "hook must have seen the attempted value");
}

ZTEST(checked_write_tests, test_checked_write_failure_then_success) {
    CheckedChar ch;
    ch.nextResult = -EIO;
    zassert_true(remote_write(ch, 50u) < 0);
    zassert_equal((uint32_t)ch, 7u, "default value must survive a rejected write");

    ch.nextResult = 0;
    zassert_equal(remote_write(ch, 50u), (ssize_t)sizeof(uint32_t));
    zassert_equal((uint32_t)ch, 50u);
    zassert_equal(ch.callCount, 2);
}

ZTEST(checked_write_tests, test_plain_hook_unaffected) {
    PlainHookChar ch;

    ssize_t ret = remote_write(ch, 5u);

    zassert_equal(ret, (ssize_t)sizeof(uint32_t));
    zassert_equal((uint32_t)ch, 5u);
    zassert_equal(ch.callCount, 1);
}

ZTEST(checked_write_tests, test_no_hook_unaffected) {
    NoHookChar ch;

    ssize_t ret = remote_write(ch, 5u);

    zassert_equal(ret, (ssize_t)sizeof(uint32_t));
    zassert_equal((uint32_t)ch, 5u);
}

/* ---- battery_quantize ---- */

ZTEST_SUITE(battery_quantize_tests, NULL, NULL, NULL, NULL, NULL);

ZTEST(battery_quantize_tests, test_rounds_to_nearest) {
    zassert_equal(battery_quantize(7914, 10), 7910);
    zassert_equal(battery_quantize(7915, 10), 7920);
    zassert_equal(battery_quantize(7916, 10), 7920);
    zassert_equal(battery_quantize(7910, 10), 7910);
    zassert_equal(battery_quantize(0, 10), 0);
}

ZTEST(battery_quantize_tests, test_negative_values_symmetric) {
    /* Sign encodes charge/discharge direction — rounding must be symmetric. */
    zassert_equal(battery_quantize(-14, 10), -10);
    zassert_equal(battery_quantize(-15, 10), -20);
    zassert_equal(battery_quantize(-350, 10), -350);
    zassert_equal(battery_quantize(-7914, 10), -7910);
}

ZTEST(battery_quantize_tests, test_degenerate_steps) {
    zassert_equal(battery_quantize(1234, 1), 1234);
    zassert_equal(battery_quantize(1234, 0), 1234);
    zassert_equal(battery_quantize(-5, -3), -5);
}
