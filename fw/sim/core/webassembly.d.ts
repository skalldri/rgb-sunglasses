/**
 * Minimal WebAssembly type surface for the Node build.
 *
 * TypeScript only ships WebAssembly declarations in lib.dom / lib.webworker;
 * the Node runtime has the API but @types/node doesn't declare it. Declaring
 * just what the simulator uses avoids pulling every DOM global into the
 * Node-side type space. The browser build (tsconfig.browser.json) uses the
 * real lib.dom declarations — this file is compatible with them.
 */

declare namespace WebAssembly {
  interface Memory {
    readonly buffer: ArrayBuffer;
    grow(delta: number): number;
  }
  const Memory: {
    prototype: Memory;
    new (descriptor: { initial: number; maximum?: number; shared?: boolean }): Memory;
  };

  interface Global {
    value: unknown;
    valueOf(): unknown;
  }
  const Global: {
    prototype: Global;
    new (descriptor: { value: string; mutable?: boolean }, v?: unknown): Global;
  };

  type Exports = Record<string, unknown>;

  interface Instance {
    readonly exports: Exports;
  }
  const Instance: {
    prototype: Instance;
    new (module: Module, importObject?: object): Instance;
  };

  interface Module {}
  const Module: {
    prototype: Module;
    new (bytes: ArrayBuffer | ArrayBufferView): Module;
    imports(module: Module): { module: string; name: string; kind: string }[];
    exports(module: Module): { name: string; kind: string }[];
  };

  interface WebAssemblyInstantiatedSource {
    instance: Instance;
    module: Module;
  }

  function compile(bytes: ArrayBuffer | ArrayBufferView): Promise<Module>;
  function instantiate(
    bytes: ArrayBuffer | ArrayBufferView,
    importObject?: object,
  ): Promise<WebAssemblyInstantiatedSource>;
  function instantiate(module: Module, importObject?: object): Promise<Instance>;
}
