import fs from 'fs';
const { instance } = await WebAssembly.instantiate(fs.readFileSync('fpu.wasm'), {});
const c = instance.exports.count();
const m = instance.exports.run();
console.log(`wasm: ${c} records, ${m} mismatches`);
process.exit(m === 0 ? 0 : 1);
