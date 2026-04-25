# xla.js

TypeScript/Node wrapper around the PJRT C API. The current implementation targets the PJRT CPU plugin built in the local OpenXLA checkout at `./xla`.

## Prerequisites

- Node.js 25+
- A C++17 compiler
- A built PJRT CPU plugin at `xla/bazel-bin/xla/pjrt/c/pjrt_c_api_cpu_plugin.so`

The repository expects `./xla` to point at an OpenXLA source/build tree. In this workspace it is a symlink to `/Users/mike/projects/xla`.

## Install

```sh
npm install
```

## Build

```sh
npm run build
```

This runs `node-gyp rebuild` to compile the native Node addon, then `tsc` to compile the TypeScript wrapper and tests.

## Test

```sh
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

Pass a custom plugin path when the PJRT build is somewhere else:

```ts
const client = new Client({
  pluginPath: '/path/to/pjrt_c_api_cpu_plugin.so',
  cpuDeviceCount: 4,
})
```

## Current Scope

This is a small first wrapper over PJRT:

- dynamically loads a PJRT C API plugin
- creates a CPU client
- compiles MLIR through `PJRT_Client_Compile`
- transfers scalar `f32` inputs to the CPU device
- executes one-output scalar programs
- copies scalar `f32` outputs back to JavaScript

Broader tensor shapes, more element types, richer compile options, and explicit resource disposal can be layered on top of this native binding.
