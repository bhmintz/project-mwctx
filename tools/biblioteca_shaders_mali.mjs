// Builds the Mali-G52 variant of nfsmw_shaders.nfsp (docs/plan-backend-mali-handoff.md, 0bis).
// Same pipeline as biblioteca_shaders.mjs (the installer's scanner, translator, rewrites and packer), with
// three changes:
//   - shader_common.h gets "#define NFSMW_MALI 1" (4 descriptor sets, bounded heaps, no 64-bit pointer);
//   - DXC is the native one in WSL with -fspv-target-env=vulkan1.1spirv1.4 (the WASM one is fixed at
//     vulkan1.2 = SPIR-V 1.5, which a Vulkan 1.1 device with VK_KHR_spirv_1_4 cannot take), and
//     vk::RawBufferLoad is rewritten as NfsmwSinPuntero before compiling;
//   - every module goes through spirv-val (vulkan1.1spv1.4) and is refused if it declares a capability the
//     Mali does not have (Int64, RuntimeDescriptorArray, PhysicalStorageBufferAddresses).
//
// Usage (Windows, node; DXC for Linux in WSL, spirv-val from the Android NDK):
//   node tools/biblioteca_shaders_mali.mjs <game folder> <output .nfsp>
// Environment: WSL_DISTRO (Ubuntu), WSL_DXC (~/dxc/bin/dxc), SPIRV_VAL (NDK shader-tools spirv-val.exe).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { pathToFileURL, fileURLToPath } from 'node:url';

const [game, output] = process.argv.slice(2);
if (!output) {
  console.error('uso: node tools/biblioteca_shaders_mali.mjs <carpeta del juego> <salida .nfsp>');
  process.exit(1);
}
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const installer = path.join(repo, 'android', 'app', 'src', 'main', 'assets', 'shaders');
const distro = process.env.WSL_DISTRO || 'Ubuntu';
const wslDxc = process.env.WSL_DXC || '~/dxc/bin/dxc';
const spirvVal = process.env.SPIRV_VAL ||
  'D:/android/sdk/ndk/29.0.14206865/shader-tools/windows-x86_64/spirv-val.exe';

const base = pathToFileURL(installer + '/');
const load = async (rel) => import(new URL(rel, base));
const { default: createHlslModule } = await load('wasm/hlsl.mjs');
const { default: createPackModule } = await load('wasm/pack.mjs');
const { default: createLzxModule } = await load('wasm/lzx.mjs');
const { readXexImage } = await load('lib/xex.js');
const { ContainerScanner } = await load('lib/containers.js');
const { buildShaderLibrary } = await load('lib/shaders.js');
const quiet = () => ({ print: () => {}, printErr: () => {} });

// --- DXC replacement: same interface the installer uses from the WASM module (FS + ccall('compile')) ---
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'nfsmw_mali_'));
const local = (p) => path.join(work, p.replace(/^\//, ''));
const toWsl = (p) => {
  const abs = path.resolve(p).replace(/\\/g, '/');
  return `/mnt/${abs[0].toLowerCase()}${abs.slice(2)}`;
};
const CAP_PROHIBIDAS = { 11: 'Int64', 5302: 'RuntimeDescriptorArray', 5347: 'PhysicalStorageBufferAddresses' };
let compilados = 0;
const dxc = {
  FS: {
    mkdirTree: (p) => fs.mkdirSync(local(p), { recursive: true }),
    writeFile: (p, data) => fs.writeFileSync(local(p), data),
    readFile: (p) => new Uint8Array(fs.readFileSync(local(p))),
    unlink: (p) => fs.unlinkSync(local(p)),
  },
  ccall: (_name, _ret, _types, [input, out, vertex]) => {
    const src = local(input);
    const texto = fs.readFileSync(src, 'utf8').split('vk::RawBufferLoad<').join('NfsmwSinPuntero<');
    fs.writeFileSync(src, texto);
    const args = ['-spirv', '-T', vertex ? 'vs_6_6' : 'ps_6_6', '-E', 'main', '-HV', '2021',
      '-fspv-target-env=vulkan1.1spirv1.4', '-fvk-use-dx-layout', '-Werror=parameter-usage'];
    if (vertex) args.push('-fvk-invert-y');
    const cmd = `${wslDxc} ${args.join(' ')} '${toWsl(src)}' -Fo '${toWsl(local(out))}'`;
    try {
      execFileSync('wsl', ['-d', distro, '-e', 'bash', '-c', cmd], { stdio: ['ignore', 'ignore', 'pipe'] });
    } catch (e) {
      console.error(`DXC rechazo ${input}:\n${e.stderr}`);
      fs.copyFileSync(src, path.join(path.dirname(output), path.basename(input)));  // for inspection
      return 1;
    }
    try {
      execFileSync(spirvVal, ['--target-env', 'vulkan1.1spv1.4', '--scalar-block-layout', local(out)],
        { stdio: ['ignore', 'ignore', 'pipe'] });
    } catch (e) {
      console.error(`spirv-val rechazo ${input}:\n${e.stderr}`);
      return 4;
    }
    const spv = fs.readFileSync(local(out));
    const w = new Uint32Array(spv.buffer, spv.byteOffset, spv.length >> 2);
    if (w[1] > 0x00010400) {
      console.error(`${input}: SPIR-V ${(w[1] >> 16) & 0xff}.${(w[1] >> 8) & 0xff} (> 1.4)`);
      return 5;
    }
    for (let i = 5; i < w.length;) {
      const n = w[i] >>> 16;
      if (!n) break;
      if ((w[i] & 0xffff) === 17 && CAP_PROHIBIDAS[w[i + 1]]) {
        console.error(`${input}: OpCapability ${CAP_PROHIBIDAS[w[i + 1]]} (la Mali no la tiene)`);
        fs.copyFileSync(src, path.join(path.dirname(output), path.basename(input)));
        fs.copyFileSync(local(out), path.join(path.dirname(output), path.basename(out)));
        return 6;
      }
      i += n;
    }
    if (++compilados % 25 === 0) console.log(`  ${compilados} compilados`);
    return 0;
  },
};

const modules = { hlsl: await createHlslModule(quiet()), dxc, pack: await createPackModule(quiet()),
  lzx: await createLzxModule(quiet()) };
const sha = (b) => createHash('sha256').update(b).digest('hex');
const manifest = JSON.parse(fs.readFileSync(new URL('release/manifest.json', base)));

// <game folder> can be "adb:<folder on the device>": then each NFS/*.bin is pulled, scanned and deleted
// one at a time (the NFS folder is 5.4 GB). The containers found are kept in out/contenedores_cache, so
// later runs need neither the game nor the device.
const cache = path.join(repo, 'out', 'contenedores_cache');
const porAdb = game.startsWith('adb:');
const tmpJuego = fs.mkdtempSync(path.join(os.tmpdir(), 'nfsmw_juego_'));
const traer = (rel) => {
  if (!porAdb) return path.join(game, rel);
  const destino = path.join(tmpJuego, path.basename(rel));
  execFileSync('adb', ['pull', `${game.slice(4)}/${rel}`, destino], { stdio: 'ignore' });
  return destino;
};
const soltar = (file) => { if (porAdb) fs.rmSync(file, { force: true }); };

let containers;
let build;
if (fs.existsSync(path.join(cache, 'orden.json'))) {
  const cab = JSON.parse(fs.readFileSync(path.join(cache, 'orden.json'), 'utf8'));
  build = manifest.builds.find((b) => b.xex_sha256 === cab.xex_sha256);
  containers = cab.nombres.map((name) => ({ name, bytes: new Uint8Array(fs.readFileSync(path.join(cache, name))) }));
  console.log(`contenedores: ${containers.length} de la cache (${cache})`);
} else {
  const xexFile = traer('default.xex');
  const xex = new Uint8Array(fs.readFileSync(xexFile));
  soltar(xexFile);
  const xexSha = sha(xex);
  build = manifest.builds.find((b) => b.xex_sha256 === xexSha);
  console.log(`default.xex: ${build ? build.edition : 'edicion desconocida'}`);
  const { image } = await readXexImage(xex, async (c, bits, size) => {
    modules.lzx.FS.writeFile('/i.lzx', c);
    if (modules.lzx.callMain(['/i.lzx', '/i.bin', String(bits), String(size)])) throw new Error('LZX failed');
    return modules.lzx.FS.readFile('/i.bin');
  });
  const exe = new ContainerScanner('xex_');
  exe.scanWhole('default.xex', image);

  // Same files and order as the installer: NFS/*.bin sorted by lower-case path.
  let listado;
  if (porAdb) {
    const salida = execFileSync('adb', ['shell', `cd '${game.slice(4)}/NFS' && stat -c '%s %n' *`], { encoding: 'utf8' });
    listado = salida.trim().split(/\r?\n/).map((l) => l.trim().split(' ')).map(([s, n]) => ({ n, size: Number(s) }));
  } else {
    const nfs = path.join(game, 'NFS');
    listado = fs.readdirSync(nfs).map((n) => ({ n, size: fs.statSync(path.join(nfs, n)).size }));
  }
  const files = listado.filter((f) => /\.bin$/i.test(f.n)).map((f) => ({ rel: `NFS/${f.n}`, size: f.size }))
    .filter((f) => f.size >= 24 && f.size <= 1024 * 1024 * 1024)
    .sort((a, b) => a.rel.toLowerCase().localeCompare(b.rel.toLowerCase()));
  const disc = new ContainerScanner('');
  const chunk = Buffer.alloc(16 << 20);
  for (const f of files) {
    console.log(`  escaneando ${f.rel} (${(f.size / 1048576).toFixed(0)} MB)`);
    const file = traer(f.rel);
    disc.beginFile(f.rel, f.size);
    const fd = fs.openSync(file, 'r');
    let done = 0;
    while (done < f.size) {
      const n = fs.readSync(fd, chunk, 0, chunk.length, done);
      done += n;
      disc.push(new Uint8Array(chunk.buffer, 0, n).slice(), done >= f.size);
    }
    fs.closeSync(fd);
    soltar(file);
  }
  containers = [...disc.found, ...exe.found];
  console.log(`contenedores: ${disc.found.length} en el disco + ${exe.found.length} en el XEX`);
  fs.mkdirSync(cache, { recursive: true });
  for (const c of containers) fs.writeFileSync(path.join(cache, c.name), c.bytes);
  fs.writeFileSync(path.join(cache, 'orden.json'),
    JSON.stringify({ xex_sha256: xexSha, nombres: containers.map((c) => c.name) }));
}
fs.rmSync(tmpJuego, { recursive: true, force: true });

const blurSha = build ? build.blur_container_sha256 : 'e80cca037bd4becd8a192daa4583e3a3a9281e06436015746583f30701a296f9';
const blur = containers.find((c) => sha(c.bytes) === blurSha);
if (!blur) throw new Error('composition shader not found');
const comun = Buffer.concat([Buffer.from('#define NFSMW_MALI 1\n'),
  fs.readFileSync(path.join(installer, 'shader_common.h'))]);
try {
  const library = await buildShaderLibrary(containers, modules, new Uint8Array(comun), (t) => console.log(t),
    blur.name.slice(0, -4));
  fs.writeFileSync(output, library);
  console.log(`biblioteca Mali: ${library.length} bytes, SHA-256 ${sha(library)} -> ${output}`);
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}
