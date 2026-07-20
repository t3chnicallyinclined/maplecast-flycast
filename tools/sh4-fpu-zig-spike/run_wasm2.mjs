import fs from 'fs';
const { instance } = await WebAssembly.instantiate(fs.readFileSync('fpu2.wasm'), {});
const c = instance.exports.count(), m = instance.exports.run();
console.log(`wasm fp-ops: ${c} records (${c*6} outputs), ${m} mismatches`);
process.exit(m === 0 ? 0 : 1);
