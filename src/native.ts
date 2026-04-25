import { createRequire } from 'node:module'
import path from 'node:path'

const require = createRequire(import.meta.url)
const native = require(path.resolve('build/Release/xla_js.node')) as Native

type Handle = object

type Native = {
  createClient(path: string, cpuDeviceCount?: number): Handle
  platformName(client: Handle): string
  deviceCount(client: Handle): number
  compile(client: Handle, code: string, format: string): Handle
  executeF32Scalar(executable: Handle, input: number): number
}

export {
  native,
  type Handle,
}
