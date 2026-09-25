---
paths:
  - "fw/src/**/*.cpp"
  - "fw/drivers/**/*.cpp"
  - "fw/tests/**/*.cpp"
---

# C++ logging: why enums and bools need an `(int)` cast

Loads with firmware C++ sources. The rules are in `fw/CLAUDE.md` "Coding rules"; this is the
mechanism, so you can recognise the symptom.

`LOG_*("%d", someEnum)` / `LOG_*("%d", someBool)` from a `.cpp` file silently prints the true value
in the low byte with three bytes of adjacent stack in the upper bits — e.g. `BT_SECURITY_L4`
(literally `4`) printed as `59609092` (`0x038D9004`), and `err 2` as `66701314` (`0x03F9C802`). It
does **not** crash and is **not** obviously wrong at a glance: it cost most of a debugging session
on a real BLE pairing failure (2026-08-14) because `PIN_OR_KEY_MISSING` was unreadable.

Mechanism (`zephyr/include/zephyr/sys/cbprintf_cxx.h`): under `CONFIG_LOG_MODE_DEFERRED` log
arguments are packed, not passed as varargs, so **no integer promotion happens**. The C path
(`_Generic` in `cbprintf_internal.h`) promotes `char`/`short`/etc.; the C++ path has matching
`z_cbprintf_cxx_store_arg()` overloads for those scalars (`int tmp = arg + 0;`) but **none for
enums and none for `bool`**, so both fall through to the generic template (for `bool` and a scoped
enum, `T` is an *exact* match, so it wins outright), which copies `MAX(sizeof(T), 4)` = 4 bytes out
of a 1-byte object. Nearly every enum here is 1 byte: Zephyr marks many `enum __packed`
(`bt_security_t`, `bt_conn_type`) and the ARM EABI build uses short enums, which shrinks small
**project-defined** enums too; `bool` is 1 byte everywhere. Examples of the cast done right:
`security_changed()` / `pairing_complete()` in `fw/src/bluetooth.cpp`, `fw/src/power/charger_policy.cpp`.

**`enum class` is NOT safe by construction.** Zephyr's argument checker (`z_log_printf_arg_checker`, `log_core.h`)
is only `__printf_like`, so a mismatch is a `-Wformat` **warning**, and this build enables no
`-Werror` — `LOG_INF("state %d", BtThreadState::CONNECTED)` compiles clean and ships the same
garbage. An *unscoped* packed enum produces no warning at all. Treat the warning as a bonus, never
the gate. Where the SDK ships a stringifier, prefer it over a hand-rolled name table
(`bt_security_err_to_str()` needs `CONFIG_BT_SECURITY_ERR_TO_STR`, EXPERIMENTAL and a string
table — currently off).

**No `%f`/`%g`**: `CONFIG_CBPRINTF_FP_SUPPORT` and `CONFIG_PICOLIBC_IO_FLOAT` are disabled (~10 KB
FLASH, issue #79); a `%f` prints the literal specifier. Use integer fixed-point: `fmt_fixed4()` /
`agc_gain_db10()` in `fw/src/sound/sound.cpp`, `fmt_param_f32()` in `fw/src/extensions/extension_host.cpp`.
