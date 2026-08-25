// @tensorflow/tfjs-node 4.22.0's install step, on this Node version (whose
// reported N-API version is 10 -- see process.versions.napi), creates
// lib/napi-v10/ with a tensorflow.dll symlink but no compiled
// tfjs_binding.node (no prebuilt binary exists for that NAPI version from
// this package release), while the actual working binding sits in
// lib/napi-v8/ -- but *without* tensorflow.dll next to it, so loading it
// fails with a generic "the specified module could not be found" (Windows'
// error for "a DLL this DLL depends on is missing"). Confirmed by hand
// during development: copying deps/lib/tensorflow.dll into lib/napi-v8/
// fixes it immediately, and the module then reports the real native
// 'tensorflow' backend, not the pure-JS fallback.
//
// This script makes that fix reproducible after every `npm install`
// instead of relying on someone remembering to do it by hand. Safe to run
// even if tfjs-node isn't installed (analysis kinds other than
// transcribePolyphonic don't need it) or if a future tfjs-node release
// fixes this upstream (the copy becomes a harmless no-op once the file
// already exists).
import { existsSync, copyFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const engineDir = dirname(dirname(fileURLToPath(import.meta.url)));
const pkgDir = join(engineDir, 'node_modules', '@tensorflow', 'tfjs-node');
const dll = join(pkgDir, 'deps', 'lib', 'tensorflow.dll');
const dest = join(pkgDir, 'lib', 'napi-v8', 'tensorflow.dll');

if (!existsSync(pkgDir)) {
  console.log('[fix-tfjs-node-binding] @tensorflow/tfjs-node not installed -- nothing to do.');
} else if (!existsSync(dll)) {
  console.warn('[fix-tfjs-node-binding] expected DLL not found at ' + dll + ' -- tfjs-node package layout may have changed; skipping.');
} else if (existsSync(dest)) {
  console.log('[fix-tfjs-node-binding] already fixed.');
} else {
  copyFileSync(dll, dest);
  console.log('[fix-tfjs-node-binding] copied tensorflow.dll into lib/napi-v8/ so the native binding can load.');
}
