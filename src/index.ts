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

const defaultCpuPluginPath = path.resolve('xla/bazel-bin/xla/pjrt/c/pjrt_c_api_cpu_plugin.so')

class Client {
  #handle: Handle

  constructor(options: ClientOptions = {}) {
    const pluginPath = options.pluginPath ?? defaultCpuPluginPath
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
