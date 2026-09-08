/**
 * AppButton sizing.
 *
 * The visible fill (gradient or solid View) sits inside the Pressable that carries the caller's
 * `style`. Two things have to hold at once:
 *
 * - The fill must NOT flex-grow. On React Native's Yoga a flex-grow child of an auto-height
 *   container takes the whole "at most" budget it was measured with, so a button outside a
 *   ScrollView or a fixed-height row fills the screen. Seen in app-v3.5.0 on the Controls tab's
 *   "Go to Connect": 1676 px tall on a 2000 px screenshot.
 * - A caller asking for a taller button (the Audio Tuning footer's 56 dp) must get a taller
 *   fill, not a 44 dp surface floating inside a 56 dp touch target.
 */

import { render } from '@testing-library/react-native';
import React from 'react';
import { StyleSheet } from 'react-native';

import { AppButton, verticalSizeOf } from '@/components/ui/app-button';

/** Flattened style of the first host View under the Pressable, i.e. the visible fill. */
function fillStyleOf(getByRole: ReturnType<typeof render>['getByRole']) {
  const pressable = getByRole('button');
  return StyleSheet.flatten(pressable.props.children[0].props.style);
}

describe('AppButton fill sizing', () => {
  it.each(['primary', 'secondary', 'danger', 'ghost'] as const)('%s fill never flex-grows', variant => {
    const { getByRole } = render(<AppButton title="Go to Connect" variant={variant} />);
    const fill = fillStyleOf(getByRole);
    expect(fill.flexGrow).toBeUndefined();
    expect(fill.flex).toBeUndefined();
    expect(fill.minHeight).toBe(44);
  });

  it.each(['primary', 'secondary'] as const)("%s fill repeats the caller's minHeight", variant => {
    const { getByRole } = render(
      <AppButton title="Presets" variant={variant} style={{ flex: 1, minHeight: 56 }} />
    );
    const fill = fillStyleOf(getByRole);
    expect(fill.minHeight).toBe(56);
    // Only the vertical size crosses over: `flex: 1` belongs to the Pressable's row slot.
    expect(fill.flex).toBeUndefined();
    expect(fill.flexGrow).toBeUndefined();
  });

  it('verticalSizeOf keeps only height, minHeight and maxHeight, from a style array', () => {
    expect(verticalSizeOf([{ flex: 1, marginTop: 8 }, { minHeight: 56, maxHeight: 80 }, undefined])).toEqual({
      minHeight: 56,
      maxHeight: 80,
    });
    expect(verticalSizeOf({ height: 48 })).toEqual({ height: 48 });
    expect(verticalSizeOf(undefined)).toEqual({});
  });
});
