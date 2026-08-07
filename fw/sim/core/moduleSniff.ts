/**
 * Magic-number sniffing for uploaded extension modules. The interesting
 * rejection is an ELF: dropping the ARM `.llext` artifact itself is THE
 * predictable mistake (it's the file the firmware consumes), and it
 * deserves a targeted explanation instead of a generic instantiate error —
 * the simulator runs wasm builds of the same source, never ARM machine
 * code (see fw/sim/PARITY.md).
 */

export type ModuleKind = "wasm" | "elf" | "unknown";

const WASM_MAGIC = [0x00, 0x61, 0x73, 0x6d]; // "\0asm"
const ELF_MAGIC = [0x7f, 0x45, 0x4c, 0x46]; // "\x7fELF"

function startsWith(bytes: Uint8Array, magic: number[]): boolean {
  if (bytes.length < magic.length) {
    return false;
  }
  for (let i = 0; i < magic.length; i++) {
    if (bytes[i] !== magic[i]) {
      return false;
    }
  }
  return true;
}

export function sniffModuleKind(bytes: Uint8Array): ModuleKind {
  if (startsWith(bytes, WASM_MAGIC)) {
    return "wasm";
  }
  if (startsWith(bytes, ELF_MAGIC)) {
    return "elf";
  }
  return "unknown";
}

/** The user-facing rejection message per non-wasm kind. */
export function rejectionMessage(name: string, kind: Exclude<ModuleKind, "wasm">): string {
  if (kind === "elf") {
    return (
      `"${name}" is an ARM .llext (ELF) — the file the DEVICE runs. The simulator ` +
      `runs WebAssembly builds of the same source: fw/sim/build-extensions.sh <name> ` +
      `produces the .wasm twin (see fw/sim/README.md).`
    );
  }
  return `"${name}" is not a WebAssembly module (missing \\0asm magic).`;
}
