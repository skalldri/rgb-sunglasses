/**
 * Deterministic RNG for the simulator.
 *
 * The only randomness in the whole tick pipeline is ColorModeSource's
 * sys_rand32_get (random hue rolls). The device's sequence is not
 * reproducible anyway, so the sim substitutes a small seeded generator —
 * SEMANTICS (>= 60-degree hue distance) are identical, the sequence is not
 * (documented in PARITY.md). Seeding makes runs bit-reproducible, which
 * golden tests and the determinism self-test rely on.
 */

/** mulberry32: tiny, well-distributed 32-bit PRNG. Returns uint32. */
export function mulberry32(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return (t ^ (t >>> 14)) >>> 0;
  };
}
