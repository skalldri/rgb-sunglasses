/** Moved to core/dline.ts (it was always platform-agnostic) so the browser
 * scenario player can replay `type: "features"` audio too. Re-exported here
 * so existing node/ imports keep working. */
export * from "../core/dline";
