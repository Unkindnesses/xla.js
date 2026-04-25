import path from 'node:path'

import { native, type Handle } from './native.js'

export {
  Client,
  Executable,
  defaultCpuPluginPath,
}

type ClientOptions = {
  pluginPath?: string
  cpuDeviceCount?: number
}

function pathFromEnv(name: string) {
  const value = process.env[name]
  return value ? path.resolve(value) : undefined
}

function cpuPluginPathFromXlaDir() {
  const xlaDir = pathFromEnv('XLA_DIR')
  return xlaDir ? path.resolve(xlaDir, 'bazel-bin/xla/pjrt/c/pjrt_c_api_cpu_plugin.so') : undefined
}

const defaultCpuPluginPath = cpuPluginPathFromXlaDir()

function envCpuPluginPath() {
  if (defaultCpuPluginPath) return defaultCpuPluginPath
  throw new Error(
    'Pass pluginPath or set XLA_DIR to the OpenXLA source/build tree',
  )
}

class Client {
  #handle: Handle

  constructor(options: ClientOptions = {}) {
    const pluginPath = options.pluginPath ?? envCpuPluginPath()
    this.#handle = native.createClient(pluginPath, options.cpuDeviceCount ?? 1)
  }

  get platformName() {
    return native.platformName(this.#handle)
  }

  get deviceCount() {
    return native.deviceCount(this.#handle)
  }

  compileMlir(code: string) {
    return new Executable(this, native.compile(this.#handle, code, 'mlir'))
  }

  get handle() {
    return this.#handle
  }
}

class Executable {
  #client: Client
  #handle: Handle

  constructor(client: Client, handle: Handle) {
    this.#client = client
    this.#handle = handle
  }

  executeF32Scalar(input: number) {
    void this.#client
    return native.executeF32Scalar(this.#handle, input)
  }
}
