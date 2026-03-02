# GATT Server Stack-Usage Refactor Plan

## Objective
Refactor the `BtGattServer` assembly path to eliminate large constructor-time temporaries and reduce startup stack usage during static initialization.

Primary hotspot:
- `BtGattServer::BtGattServer(...)` in `src/bluetooth/bt_service_cpp.h`

## Suspected root cause
Current assembly uses by-value tuple construction and conversion:
- `concatenateAttrTuples(...)`
- `tupleToArray(...)`
- assignment into `attrs_`

For services with many characteristics (especially string slots), this likely creates large temporary objects on the startup stack and can trigger early UsageFaults.

---

## Scope
In scope:
- `src/bluetooth/bt_service_cpp.h`
- Provider types used by `BtGattServer` (`BtGattPrimaryService`, `BtGattCharacteristic`, `BtGattAutoCharacteristic`)

Out of scope (unless needed for compatibility):
- Animation business logic
- UUID schema changes
- BLE behavior changes

---

## Success criteria
1. No UsageFault during boot with normal `CONFIG_MAIN_STACK_SIZE`.
2. `west build --build-dir /workspaces/rgb-sunglasses/build /workspaces/rgb-sunglasses` passes.
3. `twister -T /workspaces/rgb-sunglasses/tests -p native_sim` passes.
4. Public call-site ergonomics remain unchanged:
   - `BtGattServer server(provider1, provider2, ...);`
   - `BT_GATT_SERVER_REGISTER(name, server);`

---

## Implementation phases

### Phase 1: Add in-place provider API (no behavior change yet)
Add a second provider contract (alongside existing tuple contract):
- `static constexpr size_t kAttrCount`
- `void copyAttrs(bt_gatt_attr *dst)`

Apply first to built-in providers:
- `BtGattPrimaryService`
- `BtGattCharacteristic`
- `BtGattAutoCharacteristic`

Notes:
- Keep `getAttrsTuple()` during this phase for compatibility.
- `copyAttrs` should write exactly `kAttrCount` attributes.

Validation gate:
- Build + Twister must pass before Phase 2.

---

### Phase 2: Add `BtGattServer` in-place assembly path
In `BtGattServer` constructor, replace tuple assembly with three explicit passes:
1. Identify primary service UUID.
2. Assign auto UUIDs.
3. Copy provider attrs directly into `attrs_` using running offset.

Implementation details:
- Keep `kTotalAttrCount` compile-time sizing.
- Replace tuple-based fill with offset-based copy:
  - `size_t offset = 0;`
  - per provider: `provider.copyAttrs(attrs_.data() + offset);`
  - `offset += provider.kAttrCount;`
- Preserve provider binding pass (`bindProviders`).

Validation gate:
- Build + Twister must pass.

---

### Phase 3: Remove tuple helper dependency from server path
After in-place path is stable:
- Stop using these in `BtGattServer` constructor:
  - `concatenateAttrTuples(...)`
  - `tupleToArray(...)`
- Keep/deprecate old helpers temporarily only if needed by any remaining provider.

Validation gate:
- Build + Twister must pass.
- Boot test on hardware with normal stack settings.

---

### Phase 4: Cleanup and hardening
- Remove dead tuple-only helpers if no longer used.
- Add lightweight internal assertions/checks:
  - provider copy count consistency
  - final offset equals `kTotalAttrCount`
- Keep code-size-conscious implementation (no unnecessary runtime containers).

Validation gate:
- Build + Twister pass.
- Flash + boot smoke test pass.

---

## Concrete symbol checklist

### Update/introduce in `src/bluetooth/bt_service_cpp.h`
- New provider concept (example):
  - `BtGattAttrCopier` requiring `kAttrCount` + `copyAttrs(bt_gatt_attr *)`
- Provider static counts:
  - `BtGattPrimaryService::kAttrCount = 1`
  - Characteristic classes: `kAttrCount = Notify ? 5 : 4`
- Provider copy methods:
  - `BtGattPrimaryService::copyAttrs(...)`
  - `BtGattCharacteristic::copyAttrs(...)`
  - `BtGattAutoCharacteristic::copyAttrs(...)`
- `BtGattServer` constructor:
  - replace tuple fill with in-place copy loop/fold

---

## Risk list and mitigations

1. **Incorrect attr ordering/offsets**
   - Mitigation: keep provider order unchanged; assert final offset.

2. **Mismatch between `kAttrCount` and actual copied attrs**
   - Mitigation: debug assertions and strict local counting per provider.

3. **Behavior drift in notify/read/write attrs**
   - Mitigation: only move assembly mechanism; do not modify attr definitions.

4. **Template compile regressions**
   - Mitigation: phase-by-phase compile gates.

---

## Rollout sequence
1. Implement Phase 1 and validate.
2. Implement Phase 2 and validate.
3. Implement Phase 3 and validate.
4. Implement Phase 4 and validate.
5. Hardware boot verification at the end of each phase touching constructor logic.

---

## Optional short-term fallback
If needed while refactor is in progress:
- Temporarily increase startup stack (`CONFIG_MAIN_STACK_SIZE`) to unblock testing.
- Remove once in-place assembly refactor confirms stable boot.
