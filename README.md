# xla.js

TypeScript/Node wrapper around the PJRT C API. The current implementation targets the PJRT CPU plugin from a built OpenXLA checkout.

## Prerequisites

- Node.js 25+
- A C++17 compiler
- A built OpenXLA checkout
- A built PJRT CPU plugin at `$XLA_DIR/bazel-bin/xla/pjrt/c/pjrt_c_api_cpu_plugin.so`

Set `XLA_DIR` to the OpenXLA source/build tree before building or running the default CPU client:

```sh
export XLA_DIR=/path/to/openxla
```

The native addon uses `XLA_DIR` to find PJRT C API headers at build time. At runtime, `new Client()` loads the CPU plugin from `$XLA_DIR/bazel-bin/xla/pjrt/c/pjrt_c_api_cpu_plugin.so`.

If the CPU plugin is somewhere else, pass `pluginPath` to `Client`.

## Install

```sh
export XLA_DIR=/path/to/openxla
npm install
```

## Build

```sh
export XLA_DIR=/path/to/openxla
npm run build
```

This runs `node-gyp rebuild` to compile the native Node addon, then `tsc` to compile the TypeScript wrapper and tests.

## Test

```sh
export XLA_DIR=/path/to/openxla
npm test
```

The tests create a CPU PJRT client, compile a small MLIR program that adds `1.0` to an `f32` scalar, execute it through PJRT, and copy the result back to JavaScript.

## Usage

```ts
import { Client } from 'xla.js'

const mlir = `module {
func.func @main(%arg0: tensor<f32>) -> tensor<f32> {
  %0 = "mhlo.copy"(%arg0) : (tensor<f32>) -> tensor<f32>
  %1 = mhlo.constant dense<1.000000e+00> : tensor<f32>
  %2 = mhlo.add %0, %1 : tensor<f32>
  return %2 : tensor<f32>
}}`

const client = new Client()
const executable = client.compileMlir(mlir)

console.log(await executable.executeF32Scalar(41)) // 42
```

Pass a custom plugin path when the PJRT plugin is somewhere else:

```ts
const client = new Client({
  pluginPath: '/path/to/pjrt_c_api_cpu_plugin.so',
  cpuDeviceCount: 4,
})
```

Set `cpuDeviceCount` to expose more CPU devices. `Client` and `Executable`
also provide explicit `.dispose()` methods for code that cannot use `using`.

## Current Scope

This is a small first wrapper over PJRT:

- dynamically loads a PJRT C API plugin
- creates a CPU client
- compiles MLIR through `PJRT_Client_Compile`
- transfers scalar `f32` inputs to the CPU device
- executes one-output scalar programs
- copies scalar `f32` outputs back to JavaScript

Broader tensor shapes, more element types, richer compile options, and explicit resource disposal can be layered on top of this native binding.
