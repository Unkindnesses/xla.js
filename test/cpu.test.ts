import assert from 'node:assert/strict'
import test from 'node:test'

import { Client } from '../src/index.js'

const addOne = `module {
func.func @main(%arg0: tensor<f32>) -> tensor<f32> {
  %0 = "mhlo.copy"(%arg0) : (tensor<f32>) -> tensor<f32>
  %1 = mhlo.constant dense<1.000000e+00> : tensor<f32>
  %2 = mhlo.add %0, %1 : tensor<f32>
  return %2 : tensor<f32>
}}`

test('creates a CPU PJRT client', () => {
  using client = new Client({ cpuDeviceCount: 2 })
  assert.equal(client.platformName, 'cpu')
  assert.equal(client.deviceCount, 2)
})

test('compiles and evaluates scalar f32 MLIR on CPU', async () => {
  using client = new Client()
  using executable = client.compileMlir(addOne)
  assert.equal(await executable.executeF32Scalar(41), 42)
})

test('disposes executables with their client', () => {
  const client = new Client()
  const executable = client.compileMlir(addOne)
  client.dispose()

  assert.throws(() => client.platformName, ReferenceError)
  assert.throws(() => executable.executeF32Scalar(41), ReferenceError)
})
