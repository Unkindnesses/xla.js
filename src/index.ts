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
  #handle: Handle | null
  #executables = new Set<Executable>()

  constructor(options: ClientOptions = {}) {
    const pluginPath = options.pluginPath ?? envCpuPluginPath()
    this.#handle = native.createClient(pluginPath, options.cpuDeviceCount ?? 1)
  }

  get platformName() {
    return native.platformName(this.handle)
  }

  get deviceCount() {
    return native.deviceCount(this.handle)
  }

  compileMlir(code: string) {
    const executable = new Executable(this, native.compile(this.handle, code, 'mlir'))
    this.#executables.add(executable)
    return executable
  }

  get handle() {
    if (this.#handle === null) throw new ReferenceError('Client has been disposed')
    return this.#handle
  }

  dispose() {
    const handle = this.#handle
    if (handle === null) return
    for (const executable of [...this.#executables]) executable.dispose()
    native.disposeClient(handle)
    this.#handle = null
  }

  [Symbol.dispose]() {
    this.dispose()
  }

  _detach(executable: Executable) {
    this.#executables.delete(executable)
  }
}

class Executable {
  #client: Client
  #handle: Handle | null

  constructor(client: Client, handle: Handle) {
    this.#client = client
    this.#handle = handle
  }

  executeF32Scalar(input: number) {
    void this.#client
    return native.executeF32Scalar(this.handle, input)
  }

  dispose() {
    const handle = this.#handle
    if (handle === null) return
    native.disposeExecutable(handle)
    this.#handle = null
    this.#client._detach(this)
  }

  [Symbol.dispose]() {
    this.dispose()
  }

  get handle() {
    if (this.#handle === null) throw new ReferenceError('Executable has been disposed')
    return this.#handle
  }
}
