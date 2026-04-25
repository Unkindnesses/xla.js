const path = require('node:path')

const xlaDir = process.env.XLA_DIR

function requireXlaDir() {
  if (xlaDir) return path.resolve(xlaDir)
  console.error('Set XLA_DIR to the OpenXLA source/build tree before running node-gyp.')
  process.exit(1)
}

const commands = {
  includeDir: requireXlaDir,
}

const command = process.argv[2]
if (!command || !commands[command]) {
  console.error(`Usage: node scripts/xla-paths.cjs ${Object.keys(commands).join('|')}`)
  process.exit(1)
}

console.log(commands[command]())
