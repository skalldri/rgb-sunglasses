# Fiber-walk recipes (`execute_in_app`)

For when a tap can't be targeted reliably — two elements with the same label, a `Switch` (which uses
`onValueChange`, not `onPress`), or a phone whose coordinate taps land high (OnePlus 9 Pro,
`phones.md`). These walk the React fiber tree and call a component's own handler. Prefer the parent
skill's decision table first; these are the fallback.

**These fibers only exist while the screen that renders them is mounted** — Is Active toggles live on
the Controls list, per-param characteristics only on that animation's detail page (navigate there
first or the walk returns "not found"). Remember the whole navigation stack stays mounted, so a walk
can also find an instance on a covered screen — disambiguate by props, never by index.

## Press an `AppButton` by title

When `tap(text=…, strategy="accessibility")` can't distinguish two elements with the same label (e.g. a
"Connect" `AppButton` and a "Connect" `BottomTabItem`):

```javascript
// Find the first AppButton whose title matches, fire its onPress
(function() {
  var hook = globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__;
  var roots = hook.getFiberRoots(1);
  var root = null;
  roots.forEach(function(r) { if (!root) root = r; });
  var q = [root.current];
  while (q.length) {
    var f = q.shift();
    if (!f) continue;
    var n = f.type && (f.type.displayName || f.type.name || '');
    if (n === 'AppButton') {
      var p = f.memoizedProps || {};
      if (p.title === 'Connect' && p.onPress) { p.onPress(); return 'fired'; }
    }
    if (f.child) q.push(f.child);
    if (f.sibling) q.push(f.sibling);
  }
  return 'not found';
})()
```

`BaseExpoRouterLink` children are not a bare string in the fiber props —
`tap(text=..., strategy="accessibility")` handles those correctly without fiber surgery.

## Toggle a Switch (boolean) characteristic, or write any `onWrite` input

`Switch` components use `onValueChange`, so they can't be triggered via `tap()` by name or
coordinates. Walk the tree and call the component's `onWrite` prop directly:

```javascript
// In execute_in_app:
(function () {
  var hook = globalThis.__REACT_DEVTOOLS_GLOBAL_HOOK__;
  var fiberRoots = hook.getFiberRoots(1);
  var firstRoot = null;
  fiberRoots.forEach(function (r) {
    if (!firstRoot) firstRoot = r;
  });

  var target = null;
  var queue = [firstRoot.current];
  while (queue.length > 0) {
    var fiber = queue.shift();
    if (!fiber) continue;
    var name = fiber.type && (fiber.type.displayName || fiber.type.name || "");
    if (name === "CharacteristicBoolean") {
      var props = fiber.memoizedProps || {};
      // Is Active uses the SAME UUID in every animation service — match the service too.
      if (props.charUuid === "TARGET-CHAR-UUID" &&
          props.charInfo.characteristic.serviceUUID === "TARGET-SERVICE-UUID") {
        target = props;
        break;
      }
    }
    if (fiber.child) queue.push(fiber.child);
    if (fiber.sibling) queue.push(fiber.sibling);
  }

  // onWrite signature: (charUuid, encodedNewValue, encodedPreviousValue)
  // true  → 'AQ=='  (btoa of byte 0x01)
  // false → 'AA=='  (btoa of byte 0x00)
  target.onWrite(target.charUuid, "AQ==", target.charInfo.value);
  return "done";
})();
```

Find the current value first: iterate `CharacteristicBoolean` fibers and read `charInfo.value`
(`AA==` = false, `AQ==` = true).

**UUIDs** (full scheme: `.claude/rules/ble-gatt-contract.md`):

- Animation service: `12345678-1234-5678-{anim_id<<8:04x}-56789abd0000`, e.g. `Animation::Rainbow = 5`
  → `12345678-1234-5678-0500-56789abd0000` (enum values in `fw/src/animations/animation_types.h`).
  Extension animations (ids `0x40 + slot`) → groups `4000`, `4100`, ….
- **Is Active is the fixed `12345678-1234-5678-bbbb-56789abd0000` in every animation service** —
  built-in and extension alike — so **always disambiguate by
  `charInfo.characteristic.serviceUUID`, never by `charUuid` alone.**
- Parameter characteristics use auto UUIDs `…-{group}-56789abd0001/0002/…` in declaration order
  (extension params in manifest order, ids start at 1).

**Components that take `onWrite(charUuid, encodedNewValue, encodedPreviousValue)`** — so the recipe
above works for each by changing the name and the encoded value: `CharacteristicBoolean`,
`CharacteristicUint32` (4-byte LE, e.g. 50 = `MgAAAA==`), `CharacteristicFloat32` (4-byte LE IEEE-754),
`CharacteristicUtf8` (`btoa("text")`). **`CharacteristicColor` and `CharacteristicDropdown` have no
`onWrite`** — a color opens the color-picker modal (4 bytes `b,g,r,mode`; drive the modal instead) and a
dropdown writes through its own picker (`app/components/characteristic-dropdown.tsx`). The walk matches
`fiber.type.displayName || fiber.type.name`; these are named function exports in
`app/components/characteristic-*.tsx` with no `displayName`, so the function *name* is what matches —
keep those names stable (nothing enforces it).
